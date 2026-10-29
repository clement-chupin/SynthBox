#pragma once
// pgmspace stub for the simulator — on ESP32 PROGMEM is already a no-op;
// here all flash pointers are regular RAM so pgm_read_* are plain dereferences.
#define PROGMEM
#define pgm_read_byte(addr)  (*(const uint8_t*)(addr))
#define pgm_read_word(addr)  (*(const uint16_t*)(addr))
#define pgm_read_ptr(addr)   (*(const void* const*)(addr))
