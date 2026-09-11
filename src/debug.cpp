#include "debug.h"

void _printf(const char *format, ...) {
#if DEBUG
  va_list arg;
  va_start(arg, format);
  Serial.vprintf(format, arg);
  va_end(arg);
#endif
}

void _println(const String &s) {
#if DEBUG
  Serial.println(s);
#endif
}

void _println() {
#if DEBUG
  Serial.println();
#endif
}

void _print(const String &s) {
#if DEBUG
  Serial.print(s);
#endif
}

