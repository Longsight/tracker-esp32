#ifndef GNSS_H_
#define GNSS_H_

#include "lte.h"

void myGNSSEventHandler(WMGNSSEventType type, const WMGNSSEventData* data, void* args);
bool checkAssistanceStatus(WalterModemRsp* rsp, bool* updateAlmanac = nullptr,
                           bool* updateEphemeris = nullptr);
bool validateGNSSClock(WalterModemRsp* rsp);
bool updateGNSSAssistance(WalterModemRsp* rsp);
bool attemptGNSSFix(WalterModemRsp* rsp, const uint8_t maxAttempts);

#endif