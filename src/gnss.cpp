#include <WalterModem.h>

#include "gnss.h"
#include "debug.h"

extern WalterModem modem;

volatile bool gnss_fix_received = false;
bool assistance_update_received = false;
WMGNSSFixEvent latestGnssFix = {};

#if DEBUG
const double MAX_GNSS_CONFIDENCE = 2000.0;
#else
const double MAX_GNSS_CONFIDENCE = 80.0;
#endif

/**
 * @brief GNSS event handler
 *
 * This function will be called on various GNSS events such as fix received or assistance update.
 *
 * @note Make sure to keep this handler as lightweight as possible to avoid blocking the event
 * processing task.
 *
 * @param[out] type The type of GNSS event.
 * @param[out] data The data associated with the GNSS event.
 * @param[out] args User argument pointer passed to gnssSetEventHandler
 *
 * @return None.
 */
void myGNSSEventHandler(WMGNSSEventType type, const WMGNSSEventData* data, void* args)
{
  uint8_t goodSatCount = 0;

  switch(type) {
  case WALTER_MODEM_GNSS_EVENT_FIX:
    memcpy(&latestGnssFix, &data->gnssfix, sizeof(WMGNSSFixEvent));

    /* Count satellites with good signal strength */
    for(int i = 0; i < latestGnssFix.satCount; ++i) {
      if (latestGnssFix.sats[i].signalStrength >= 30) {
        ++goodSatCount;
      }
    }
    _println();
    _printf("GNSS fix received:"
                  "  Confidence: %.02f"
                  "  Latitude: %.06f"
                  "  Longitude: %.06f"
                  "  Satcount: %d"
                  "  Good sats: %d\r\n",
                  latestGnssFix.estimatedConfidence, latestGnssFix.latitude,
                  latestGnssFix.longitude, latestGnssFix.satCount, goodSatCount);

    gnss_fix_received = true;
    break;

  case WALTER_MODEM_GNSS_EVENT_ASSISTANCE:
    if (data->assistance == WALTER_MODEM_GNSS_ASSISTANCE_TYPE_ALMANAC) {
      _println("GNSS Assistance: Almanac updated");
    } else if (data->assistance == WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS) {
      _println("GNSS Assistance: Real-time ephemeris updated");
    } else if (data->assistance == WALTER_MODEM_GNSS_ASSISTANCE_TYPE_PREDICTED_EPHEMERIS) {
      _println("GNSS Assistance: Predicted ephemeris updated");
    }

    assistance_update_received = true;
    break;

  default:
    break;
  }
}

/**
 * @brief Inspect GNSS assistance status and optionally set update flags.
 *
 * Prints the availability and recommended update timing for the
 * almanac and real-time ephemeris databases.  If update flags are provided,
 * they are set to:
 *   - true  : update is required (data missing or time-to-update <= 0)
 *   - false : no update required
 *
 * @param[in] rsp Pointer to modem response object.
 * @param[out] updateAlmanac   Optional pointer to bool receiving almanac update.
 * @param[out] updateEphemeris Optional pointer to bool receiving ephemeris update.
 *
 * @return True if assistance status was successfully retrieved and parsed. False on error.
 */
bool checkAssistanceStatus(WalterModemRsp* rsp, bool* updateAlmanac, bool* updateEphemeris)
{
  /* Request assistance status */
  if (!modem.gnssGetAssistanceStatus(rsp) ||
     rsp->type != WALTER_MODEM_RSP_DATA_TYPE_GNSS_ASSISTANCE_DATA) {
    _println("Error: Could not request GNSS assistance status");
    return false;
  }

  /* Default output flags */
  if (updateAlmanac)
    *updateAlmanac = false;
  if (updateEphemeris)
    *updateEphemeris = false;

  /* Helper lambda */
  auto report = [](const char* name, const WMGNSSAssistance& data, bool* updateFlag) {
    _printf("%s data is ", name);

    if (data.available) {
      _printf("available and should be updated within %lds\r\n", data.timeToUpdate);

      if (updateFlag)
        *updateFlag = (data.timeToUpdate <= 0);
    } else {
      _println("not available.");

      if (updateFlag)
        *updateFlag = true;
    }
  };

  const WMGNSSAssistance& almanac =
      rsp->data.gnssAssistance[WALTER_MODEM_GNSS_ASSISTANCE_TYPE_ALMANAC];
  const WMGNSSAssistance& rtEph =
      rsp->data.gnssAssistance[WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS];

  report("Almanac", almanac, updateAlmanac);
  report("Real-time ephemeris", rtEph, updateEphemeris);
  return true;
}

/**
 * @brief Ensure the GNSS subsystem clock is valid, syncing with LTE if needed.
 *
 * If the clock is invalid, this function will attempt to connect to LTE
 * (if not already connected) and sync the clock up to 5 times.
 *
 * @param[in] rsp Pointer to modem response object.
 *
 * @return True if the clock is valid or successfully synchronized. False on error.
 */
bool validateGNSSClock(WalterModemRsp* rsp)
{
  /* Validate the GNSS subsystem clock */
  modem.gnssGetUTCTime(rsp);
  if (rsp->data.clock.epochTime > 4) {
    return true;
  }

  _println("System clock invalid, LTE time sync required");

  /* Connect to LTE (required for time sync) */
  if (!lteConnected() && !lteConnect()) {
    _println("Error: Could not connect to LTE network");
    return false;
  }

  /* Attempt sync clock up to 5 times */
  for(int i = 0; i < 5; ++i) {
    /* Validate the GNSS subsystem clock */
    modem.gnssGetUTCTime(rsp);
    if (rsp->data.clock.epochTime > 4) {
      _printf("Clock synchronized: %" PRIi64 "\r\n", rsp->data.clock.epochTime);
      return true;
    }
    delay(2000);
  }

  _println("Error: Could not sync time with network. Does the network support NITZ?");
  return false;
}

/**
 * @brief Update GNSS assistance data if required.
 *
 * Steps performed:
 *   1. Ensure the system clock is valid (sync with LTE if needed).
 *   2. Check the status of GNSS assistance data (almanac & ephemeris).
 *   3. Connect to LTE (if not already) and download any missing data.
 *
 * LTE is only connected when necessary.
 *
 * @param[in] rsp Pointer to modem response object.
 *
 * @return True if assistance data is valid (or successfully updated). False on error.
 */
bool updateGNSSAssistance(WalterModemRsp* rsp)
{
  bool updateAlmanac = false;
  bool updateEphemeris = false;

  /* Get the latest assistance data */
  if (!checkAssistanceStatus(rsp, &updateAlmanac, &updateEphemeris)) {
    _println("Error: Could not check GNSS assistance status");
    return false;
  }

  /* No update needed */
  if (!updateAlmanac && !updateEphemeris) {
    return true;
  }

  /* Connect to LTE to download assistance data */
  if (!lteConnected() && !lteConnect()) {
    _println("Could not connect to LTE network");
    return false;
  }

  /* Update almanac data if needed */
  assistance_update_received = false;
  if (updateAlmanac && !modem.gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_ALMANAC)) {
    _println("Could not update almanac data");
    return false;
  }

  /* Wait for assistance update event */
  while(updateAlmanac && !assistance_update_received) {
    delay(200);
  }

  /* Update real-time ephemeris data if needed */
  assistance_update_received = false;
  if (updateEphemeris &&
     !modem.gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS)) {
    _println("Could not update real-time ephemeris data");
    return false;
  }

  /* Wait for assistance update event */
  while(updateEphemeris && !assistance_update_received) {
    delay(200);
  }

  /* Recheck assistance data to ensure its valid */
  if (!checkAssistanceStatus(rsp)) {
    _println("Error: Could not check GNSS assistance status");
    return false;
  }

  return true;
}

/**
 * @brief Attempt to obtain a GNSS position fix with acceptable confidence.
 *
 * This function:
 *   1. Updates GNSS assistance data if needed.
 *   2. Requests a GNSS fix up to 5 times.
 *   3. Waits for each fix attempt to complete or time out.
 *   4. Checks the final fix confidence against MAX_GNSS_CONFIDENCE.
 *
 * @return True f a valid GNSS fix was obtained within the confidence threshold. False if assistance
 * update fails, a fix cannot be requested, a timeout occurs, or the final confidence is too low.
 */
bool attemptGNSSFix(WalterModemRsp* rsp, const uint8_t maxAttempts)
{
  if (!validateGNSSClock(rsp)) {
    _println("Error: Could not validate GNSS clock");
    return false;
  }

  /* Ensure assistance data is current */
  if (!updateGNSSAssistance(rsp)) {
    _println(
        "Warning: Could not update GNSS assistance data. Continuing without assistance.");
  }

  /* Disconnect from the network (Required for GNSS) */
  if (lteConnected() && !lteDisconnect()) {
    _println("Error: Could not disconnect from the LTE network");
    return false;
  }

  /* Attempt up to 5 GNSS fixes */
  for(int attempt = 0; attempt < maxAttempts; ++attempt) {
    gnss_fix_received = false;

    /* Optional: Reconfigure GNSS with last valid fix - This might speed up consecutive fixes */
    if (latestGnssFix.estimatedConfidence <= (MAX_GNSS_CONFIDENCE + 80.0) &&
      latestGnssFix.estimatedConfidence > 0) {
      /* Reconfigure GNSS for potential quick fix */
      if (modem.gnssConfig(WALTER_MODEM_GNSS_SENS_MODE_HIGH, WALTER_MODEM_GNSS_ACQ_MODE_HOT_START)) {
        _println("GNSS reconfigured for potential quick fix");
      } else {
        _println("Error: Could not reconfigure GNSS");
      }
    }

    /* Request a GNSS fix */
    if (!modem.gnssPerformAction()) {
      _println("Error: Could not request GNSS fix");
      return false;
    }

    _printf("Started GNSS fix (attempt %d/%d)...", attempt + 1, maxAttempts);

    /* For this example, we block here until the GNSS event handler sets the flag */
    /* Feel free to build your application code asynchronously */
    while(!gnss_fix_received) {
      delay(100);
    }
    _println();

    /* If confidence is acceptable, stop trying. Otherwise, try again */
    if (latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
      _println("Successfully obtained a valid GNSS fix");
      return true;
    } else {
      _printf("GNSS fix confidence %.02f too low",
                    latestGnssFix.estimatedConfidence);
      if (attempt < maxAttempts - 1) {
        _println(", retrying...");
      } else {
        _println();
      }
    }
  }
  return false;
}