#include <Arduino.h>

template<typename T>
inline void log(const char* file, int line, T msg) {
    Serial.print(file);
    Serial.print(':');
    Serial.print(line);
    Serial.print(": ");
    Serial.println(msg);   // picks the right overload for T
}

#define LOG(msg) do { log(__FILE__, __LINE__, (msg)); } while (0)