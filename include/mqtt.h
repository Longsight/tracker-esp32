#ifndef MQTT_H_
#define MQTT_H_

#include <WalterModem.h>
#include <FS.h>

void myMQTTEventHandler(WMMQTTEventType event, const WMMQTTEventData* data, void* args);
bool sendLine(const uint8_t* reading, char* mac);
bool sendQueue(File* readingQueue, char* mac);

#endif