#include <Wire.h>
#include <Preferences.h>

#include <LittleFS.h>
#include <FS.h>
#include <SparkFunBQ27441.h>
#include <WalterModem.h>
#include <esp_mac.h>
#include <esp_sleep.h>

#include "main.h"
#include "tls.h"
#include "debug.h"
#include "gnss.h"
#include "mqtt.h"
#include "secret.h"

char mac[12] = {0};

WalterModem modem;
Preferences trackerPrefs;

extern WMGNSSFixEvent latestGnssFix;

#if DEBUG
const int32_t SLEEP_TIME_MIN = 10;
const uint8_t GNSS_ATTEMPTS = 1;
#else
const int32_t SLEEP_TIME_MIN = 30;
const uint8_t GNSS_ATTEMPTS = 3;
#endif

const char* LOG_PATH = "/readings.txt";
const char* FAIL_PATH = "/failed.txt";

const uint8_t SDA_PIN = 15;
const uint8_t SCL_PIN = 16;

bool batteryAvail = false;

const int32_t RECHECK_TIME = 1800;

/* Config */
int32_t sleepTime;
uint8_t queueSizeMax;
uint16_t batteryCapacity;


/**
 * @brief The main Arduino setup method.
 */
void setup()
{
#if DEBUG
  Serial.begin(115200);
#endif

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    delay(5000);
  }
  _println("BEGIN");

  if (!LittleFS.begin(true)) {  // true = format on first run if mount fails
    _println("Error: LittleFS mount failed");
    sleep();
  }

  /* Get the MAC address for board validation */
  uint8_t macBuf[6] = {0};
  esp_read_mac(macBuf, ESP_MAC_WIFI_STA);
  sprintf(mac, "%02x%02x%02x%02x%02x%02x", macBuf[0], macBuf[1],
                macBuf[2], macBuf[3], macBuf[4], macBuf[5]);

  /* Start the modem */
  if (modem.begin(&Serial2)) {
    _println("Successfully initialized the modem");
  } else {
    _println("Error: Could not initialize the modem");
    sleep();
  }

  /* Configure the GNSS subsystem */
  if (!modem.gnssConfig()) {
    _println("Error: Could not configure the GNSS subsystem");
    sleep();
  }

#if USE_TLS
  if (setupTLSProfile()) {
    _println("TLS Profile setup succeeded");
  } else {
    _println("Error: TLS Profile setup failed");
    return;
  }
#endif

  /* Set the event handlers */
  modem.setGNSSEventHandler(myGNSSEventHandler, NULL);
  modem.setMQTTEventHandler(myMQTTEventHandler, NULL);

  requestConfig();

  /* Tracker config */
  trackerPrefs.begin("trackerConfig", RO_MODE);

  if (!trackerPrefs.getBool("ready")) {
    trackerPrefs.end();

    trackerPrefs.begin("trackerConfig", RW_MODE);
    trackerPrefs.putBool("ready", true);
    trackerPrefs.end();
    trackerPrefs.begin("trackerConfig", RO_MODE);
  }

  const int64_t startTime = trackerPrefs.getLong64("startTime");
  sleepTime = trackerPrefs.getInt("sleepTime");
  queueSizeMax = trackerPrefs.getUChar("queueSizeMax");
  batteryCapacity = trackerPrefs.getUShort("batteryCapacity");
   
  /* Check the time, if we can */
  WalterModemRsp timeRsp = {};
  if (!validateGNSSClock(&timeRsp)) {
    sleep(RECHECK_TIME);
  }

  if (timeRsp.data.clock.epochTime < startTime) {
    sleep((int32_t)(startTime - timeRsp.data.clock.epochTime));
  }

  WalterModemRsp rsp = {};

  /* Ensure we are using the preferred RAT */
  /* This is a reboot-persistent setting */
  if (modem.getRAT(&rsp)) {
    if (rsp.data.rat != RADIO_TECHNOLOGY) {
      modem.setRAT(RADIO_TECHNOLOGY);
      _println("Switched modem radio technology");
    }
  } else {
    _println("Error: Could not retrieve radio access technology");
  }

  /* Configure the battery subsystem */
  pinMode(GPIO_NUM_0, OUTPUT);
  digitalWrite(GPIO_NUM_0, LOW);
  Wire.setPins(SDA_PIN, SCL_PIN);
  if (lipo.begin()) {
    lipo.setCapacity(batteryCapacity);
    batteryAvail = true;
  } else {
    _println("Error: Could not configure the battery subsystem");
  }
}

void loop()
{
  if (LittleFS.exists(LOG_PATH)) {
    File readingQueue = LittleFS.open(LOG_PATH, FILE_READ);
    if (!readingQueue) {
      _println("Error: failed to open readings queue");
      sleep();
    }

    size_t queueSize = readingQueue.size() / READING_SIZE;
    _printf("%d readings in the queue\r\n", queueSize);
    bool sent = false;
    if (queueSize >= queueSizeMax) {
      sent = sendQueue(&readingQueue);
      if (mqttConnected() && !mqttDisconnect()) {
        _println("Error: Could not disconnect from MQTT server");
      }
      if (lteConnected() && !lteDisconnect()) {
        _println("Error: Could not disconnect from LTE network");
      }
    }
    readingQueue.close();
    if (sent) {
      if (!LittleFS.rename(FAIL_PATH, LOG_PATH)) {
        _printf("Error: Failed to copy %s to %s\r\n", FAIL_PATH, LOG_PATH);
      }
    }
  }

  WalterModemRsp rsp = {};

#if DEBUG
  attemptGNSSFix(&rsp, GNSS_ATTEMPTS);
#else
  if (attemptGNSSFix(&rsp, GNSS_ATTEMPTS)) {
#endif
    uint16_t battery = 0;
    if (batteryAvail) {
      battery = lipo.soc();
      _printf("Battery level: %d%%\r\n", battery);
    }

    WalterModemRsp timeRsp = {};
    modem.getClock(&timeRsp);
    int64_t timestamp = timeRsp.data.clock.epochTime;
    _printf("Timestamp: %" PRIi64 "\r\n", timestamp);

    float temp = temperatureRead();
    float lat32 = (float) latestGnssFix.latitude;
    float lon32 = (float) latestGnssFix.longitude;

    /* Construct the minimal MAC + sensor + GNSS + timestamp */
    uint8_t reading[READING_SIZE] = {0};

    uint16_t rawTemp = (temp + 50) * 100;
    memcpy(&reading[0], &timestamp, 8);
    memcpy(&reading[8], &battery, 2);
    memcpy(&reading[10], &rawTemp, 2);
    memcpy(&reading[12], &lat32, 4);
    memcpy(&reading[16], &lon32, 4);

    File readings = LittleFS.open(LOG_PATH, (LittleFS.exists(LOG_PATH)) ? FILE_APPEND : FILE_WRITE);
    if (!readings) {
      _println("Error: failed to open readings queue");
      sleep();
    }
    readings.write(reading, sizeof(reading));
    _printf("Reading written to file; %d readings queued\r\n", (readings.size() / READING_SIZE) + 1);
    readings.close();
#if !DEBUG
  }
#endif

  sleep();
}

void sleep(int32_t time)
{
  esp_sleep_config_gpio_isolate();
  modem.setOpState(WALTER_MODEM_OPSTATE_MINIMUM);
#if DEBUG
  Serial.flush();
#endif
  _printf("Sleeping for %d seconds...\r\n", time);
  modem.sleep(time);
}

void sleep()
{
  const int32_t ACTUAL_DELAY = max(sleepTime - (int32_t)(millis() / 1000), SLEEP_TIME_MIN);
  sleep(ACTUAL_DELAY);
}