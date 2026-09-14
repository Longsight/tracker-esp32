#include <WalterModem.h>
#include <LittleFS.h>
#include <FS.h>

#include "main.h"
#include "http.h"
#include "secret.h"
#include "lte.h"
#include "tls.h"
#include "debug.h"

extern WalterModem modem;

uint8_t in_buf[1500] = { 0 };

/**
 * @brief The HTTP event handler.
 *
 * This function will be called on various HTTP events such as connection, disconnection,
 * ring, etc. You can modify this handler to implement your own logic based on the events received.
 *
 * @note Make sure to keep this handler as lightweight as possible to avoid blocking the event
 * processing task.
 *
 * @param[out] event The type of HTTP event.
 * @param[out] data The data associated with the event.
 * @param[out] args User arguments.
 *
 * @return void
 */
void myHTTPEventHandler(WMHTTPEventType event, const WMHTTPEventData* data, void* args)
{
  switch(event) {
  case WALTER_MODEM_HTTP_EVENT_CONNECTED:
    if(data->rc != 0) {
      _printf("HTTP: Connection (profile %d) could not be established. (CURL: %d)\r\n",
                    data->profile_id, data->rc);
    } else {
      _printf("HTTP: Connected successfully (profile %d)\r\n", data->profile_id);
    }
    break;

  case WALTER_MODEM_HTTP_EVENT_DISCONNECTED:
    _printf("HTTP: Disconnected successfully (profile %d)\r\n", data->profile_id);
    break;

  case WALTER_MODEM_HTTP_EVENT_CONNECTION_CLOSED:
    _printf("HTTP: Connection (profile %d) was interrupted (CURL: %d)\r\n", data->profile_id,
                  data->rc);
    break;

  case WALTER_MODEM_HTTP_EVENT_RING:
    _printf(
        "HTTP: Message received on profile %d. (status: %d | content-type: %s | size: %u)\r\n",
        data->profile_id, data->status, data->content_type, data->data_len);

    /* Receive the HTTP message from the modem buffer */
    memset(in_buf, 0, sizeof(in_buf));
    if(modem.httpReceive(data->profile_id, in_buf, data->data_len)) {
      _printf("Received message for profile %d: %s\r\n", data->profile_id, in_buf);
    } else {
      _printf("Could not receive HTTP message for profile %d\r\n", data->profile_id);
    }
    break;
  }
}

bool setupHTTPSProfile()
{
    return modem.httpConfigProfile(HTTPS_PROFILE, SECRET_HTTPS_HOST, SECRET_HTTPS_PORT, TLS_PROFILE);
}

/**
 * @brief Perform an HTTP GET request.
 */
bool httpGet(const char* path)
{
  if (!lteConnected() && !lteConnect()) {
    _println("Error: Could not connect to the network");
    return false;
  }
  char* ctBuf = "application/json";

  _printf("Sending HTTP GET to %s%s\r\n", SECRET_HTTPS_HOST, path);
  if(!modem.httpQuery(HTTPS_PROFILE, path, WALTER_MODEM_HTTP_QUERY_CMD_GET, ctBuf,
                      sizeof(ctBuf))) {
    _println("Error: HTTP GET query failed");
    return false;
  }
  _println("HTTP GET successfully sent");
  return true;
}
