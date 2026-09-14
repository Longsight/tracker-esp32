#ifndef HTTP_H_
#define HTTP_H_

#include <WalterModem.h>

const int HTTPS_PROFILE = 1;

void myHTTPEventHandler(WMHTTPEventType event, const WMHTTPEventData* data, void* args);
bool setupHTTPSProfile();
bool httpGet(const char* path);

#endif