#ifndef DEBUG_H_
#define DEBUG_H_

#include <Arduino.h>

#define DEBUG 1

void _printf(const char *format, ...);
void _println(const String &s);
void _println();
void _print(const String &s);

#endif