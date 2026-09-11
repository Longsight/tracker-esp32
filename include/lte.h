#ifndef LTE_H_
#define LTE_H_

#define CELLULAR_APN ""

bool lteConnected();
bool waitForNetwork(int timeout_sec = 300);
bool lteDisconnect();
bool lteConnect();

#endif