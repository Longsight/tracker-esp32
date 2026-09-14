#ifndef MAIN_H_
#define MAIN_H_

#define RADIO_TECHNOLOGY WALTER_MODEM_RAT_LTEM
#define READING_SIZE 20

#define RW_MODE false
#define RO_MODE true

void loop();
void setup();
void sleep();
void sleep(int32_t time);

#endif