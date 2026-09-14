#ifndef MQTT_H_
#define MQTT_H_

#include <WalterModem.h>
#include <FS.h>

void myMQTTEventHandler(WMMQTTEventType event, const WMMQTTEventData* data, void* args);
bool connect();
void disconnect();
bool sendLine(const uint8_t* reading);
bool sendQueue(File* readingQueue);
bool requestConfig();

#endif