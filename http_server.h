#ifndef RONWIFI_HTTP_SERVER_H
#define RONWIFI_HTTP_SERVER_H

#include <Arduino.h>

void httpServerInit();
void httpServerLoop();

// Triggers for main loop
bool shouldArmCoinslot();
void clearArmRequest();

#endif
