#include <WalterModem.h>

#include "tls.h"
#include "mqtt.h"
#include "debug.h"

extern WalterModem modem;

/**
 * @brief Writes TLS credentials to the modem's NVS and configures the TLS profile.
 *
 * This function stores the provided TLS certificates and private keys into the modem's
 * non-volatile storage (NVS), and then sets up a TLS profile for secure communication.
 * These configuration changes are persistent across reboots.
 *
 * @note
 * - Certificate indexes 0–10 are reserved for Sequans and BlueCherry internal usage.
 * - Private key index 1 is reserved for BlueCherry internal usage.
 * - Do not attempt to override or use these reserved indexes.
 *
 * @return
 * - true if the credentials were successfully written and the profile configured.
 * - false otherwise.
 */
bool setupTLSProfile(void)
{

  if(!modem.tlsWriteCredential(false, 12, ca_cert)) {
    _println("Error: CA cert upload failed");
    return false;
  }

  if(modem.tlsConfigProfile(MQTTS_TLS_PROFILE, WALTER_MODEM_TLS_VALIDATION_CA,
                            WALTER_MODEM_TLS_VERSION_12, 12)) {
    _println("TLS profile configured");
  } else {
    _println("Error: TLS profile configuration failed");
    return false;
  }

  return true;
}