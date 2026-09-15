#include <WalterModem.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>

#include "main.h"
#include "mqtt.h"
#include "secret.h"
#include "lte.h"
#include "tls.h"
#include "debug.h"

extern WalterModem modem;
extern Preferences trackerPrefs;
extern const char* FAIL_PATH;

extern char mac[12];

const uint16_t MQTT_KEEPALIVE = 30;
const uint16_t MQTT_TIMEOUT = 60;
const char* MQTT_CLIENTID = "gps-tracker-";

const char* MQTT_HOST = SECRET_MQTT_HOST;
const uint16_t MQTT_PORT = SECRET_MQTT_PORT;
const char* MQTT_USER = SECRET_MQTT_USER;
const char* MQTT_PASS = SECRET_MQTT_PASS;
const char* MQTT_READING_TOPIC = SECRET_MQTT_READING_TOPIC;
const char* MQTT_CONFIG_TOPIC = SECRET_MQTT_CONFIG_TOPIC;
const uint8_t SEND_RATE = SECRET_SEND_RATE;
const uint16_t SEND_DELAY = (60000 / SEND_RATE);

volatile bool mqtt_connected = false;
volatile bool config_fetched = false;

/**
 * @brief The MQTT event handler.
 *
 * This function will be called on various MQTT events such as connection, disconnection,
 * subscription, publication and incoming messages. You can modify this handler to implement your
 * own logic based on the events received.
 *
 * @note Make sure to keep this handler as lightweight as possible to avoid blocking the event
 * processing task.
 *
 * @param[out] event The type of MQTT event.
 * @param[out] data The data associated with the event.
 * @param[out] args User arguments.
 *
 * @return void
 */
void myMQTTEventHandler(WMMQTTEventType event, const WMMQTTEventData* data, void* args)
{
  uint8_t configBuf[128] = { 0 };

  switch(event) {
    case WALTER_MODEM_MQTT_EVENT_CONNECTED:
      if(data->rc != 0) {
        _printf("MQTT: Connection could not be established. (code: %d)\r\n", data->rc);
      } else {
        _printf("MQTT: Connected successfully\r\n");

        mqtt_connected = true;
      }
      break;

    case WALTER_MODEM_MQTT_EVENT_DISCONNECTED:
      if(data->rc != 0) {
        _printf("MQTT: Connection was interrupted (code: %d)\r\n", data->rc);
      } else {
        _printf("MQTT: Disconnected\r\n");
      }
      mqtt_connected = false;
      break;

    case WALTER_MODEM_MQTT_EVENT_SUBSCRIBED:
      if(data->rc != 0) {
        _printf("MQTT: Could not subscribe to topic. (code: %d)\r\n", data->rc);
      } else {
        _printf("MQTT: Successfully subscribed to topic '%s'\r\n", data->topic);
        if (!modem.mqttPublish(MQTT_CONFIG_TOPIC, (uint8_t*)mac, strlen(mac), 0)) {
          _printf("Error: Failed to send message %s to server\r\n", mac);
        }
      }
      break;

    case WALTER_MODEM_MQTT_EVENT_PUBLISHED:
      if(data->rc != 0) {
        _printf("MQTT: Could not publish message (id: %d) to topic. (code: %d)\r\n", data->mid,
                      data->rc);
      } else {
        _printf("MQTT: Successfully published message (id: %d)\r\n", data->mid);
      }
      break;

    case WALTER_MODEM_MQTT_EVENT_MESSAGE:
      _printf("MQTT: Message (id: %d) received on topic '%s' (size: %ld bytes)\r\n", data->mid,
                    data->topic, data->msg_length);

      memset(configBuf, 0, sizeof(configBuf));
      /* Receive the MQTT message from the modem buffer */
      if(modem.mqttReceive(data->topic, data->mid, configBuf, data->msg_length)) {
        _printf("Received message: %s\r\n", configBuf);

        JsonDocument configDoc;
        deserializeJson(configDoc, configBuf);

        trackerPrefs.begin("trackerConfig", RW_MODE);

        trackerPrefs.putLong64("startTime", configDoc["start_time"]);
        trackerPrefs.putLong64("finishTime", configDoc["finish_time"]);
        trackerPrefs.putInt("sleepTime", configDoc["sleep_time"]);
        trackerPrefs.putUChar("queueSizeMax", configDoc["queue_size"]);
        trackerPrefs.putUShort("batteryCapacity", configDoc["battery_capacity"]);

        trackerPrefs.putBool("ready", true);
        trackerPrefs.end();
      } else {
        _println("Could not receive MQTT message");
      }
      config_fetched = true;
      break;

    case WALTER_MODEM_MQTT_EVENT_MEMORY_FULL:
      _println("MQTT: Memory full");
      break;
  }
}

bool mqttConnected()
{
  return mqtt_connected;
}

bool mqttConnect()
{
  if (!lteConnected() && !lteConnect()) {
    _println("Error: Could not connect to LTE network");
    return false;
  }
  char clientId[strlen(MQTT_CLIENTID) + strlen(mac)];
  sprintf(clientId, "%s%s", MQTT_CLIENTID, mac);
#if USE_TLS
  if (!modem.mqttConfig(clientId, MQTT_USER, MQTT_PASS, TLS_PROFILE)) {
#else
  if (!modem.mqttConfig(clientId, MQTT_USER, MQTT_PASS)) {
#endif
    _println("Error: Could not configure MQTT connection");
    return false;
  }
  if (!mqtt_connected && !modem.mqttConnect(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)) {
    _println("Error: Could not connect to MQTT server");
    return false;
  }
  uint16_t attempt = 0;
  while (!mqtt_connected) {
    delay(200);
    if (++attempt > (MQTT_TIMEOUT * 5)) {
      _println("Error: Could not connect to MQTT server");
      return false;
    }
  }
  return true;
}

bool mqttDisconnect()
{
  delay(500);
  if (!modem.mqttDisconnect()) {
    _println("Error: Could not disconnect from MQTT server");
    return false;
  }
  uint16_t attempt = 0;
  while (mqtt_connected) {
    delay(200);
    if (++attempt > (MQTT_TIMEOUT * 5)) {
      _println("Error: Could not disconnect from MQTT server");
      return false;
    }
  }
  return true;
}

bool sendQueue(File* readingQueue)
{
  if (!mqttConnected() && !mqttConnect()) {
    return false;
  }

  File failures = LittleFS.open(FAIL_PATH, FILE_WRITE);
  if (!failures) {
    _println("Error: failed to open failure queue");
    return false;
  }
  while (readingQueue->available()) {
    uint8_t reading[READING_SIZE] = {0};
    readingQueue->read(reading, READING_SIZE);
    delay(SEND_DELAY);
    if (!sendLine(reading)) {
      _println("Failed to send reading");
      failures.write(reading, sizeof(reading));
    }
  }
  failures.close();

  _println("Sent location queue to MQTT server");
  return true;
}

bool sendLine(const uint8_t* reading)
{
  float lat32;
  float lon32;
  int64_t timestamp;
  uint16_t rawTemp;
  uint16_t battery;

  memcpy(&timestamp, &reading[0], sizeof(int64_t));
  memcpy(&battery, &reading[8], sizeof(uint16_t));
  memcpy(&rawTemp, &reading[10], sizeof(uint16_t));
  float temp = ((float)rawTemp / 100.0) - 50.0;
  memcpy(&lat32, &reading[12], sizeof(float));
  memcpy(&lon32, &reading[16], sizeof(float));

  uint8_t sendMac[12] = {0};
  memcpy(sendMac, mac, 12);

  _printf("MAC: %s\r\n", sendMac);
  _printf("Battery: %d%%\r\n", battery);
  _printf("Temp: %.2f\r\n", temp);

  _printf("Latitude: %.5f\r\n", lat32);
  _printf("Longitude: %.5f\r\n", lon32);

  _printf("Timestamp: %" PRIi64 "\r\n\r\n", timestamp);

  char msg[90] = {0};
  sprintf(msg, "mac:%s,time:%" PRIi64 ",bat:%d,temp:%.2f,lat:%.6f,lon:%.6f",
      sendMac, timestamp, battery, temp, lat32, lon32);

  if (!modem.mqttPublish(MQTT_READING_TOPIC, (uint8_t*)msg, strlen(msg), 0)) {
    _printf("Error: Failed to send message %s to server\r\n", msg);
    return false;
  }
  _printf("Info:  Sent message %s to server\r\n", msg);
  return true;
}

bool requestConfig()
{
  if (!mqttConnected() && !mqttConnect()) {
    return false;
  }

  char topic[strlen(MQTT_CONFIG_TOPIC) + 12];
  sprintf(topic, "%s-%s", MQTT_CONFIG_TOPIC, mac);

  if (!modem.mqttSubscribe(topic)) {
    _printf("Error: Failed to subscribe to config topic\r\n", topic);
    return false;
  }

  uint16_t attempt = 0;
  while (!config_fetched) {
    delay(200);
    if (++attempt > (MQTT_TIMEOUT * 5)) {
      _println("Error: Could not fetch config");
      break;
    }
  }
  if (!config_fetched) {
    _println("Error: Could not fetch config");
    return false;
  }

  _println("Fetched config from server");
  return true;
}