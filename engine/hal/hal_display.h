#pragma once
// HAL: display. The platform owns and configures the u8g2 instance (bus, pins,
// controller); the application only ever receives a ready-to-draw u8g2_t*.
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Size of the display in pixels (a multiple of 8). The application reads the real size from the u8g2 instance
// (u8g2_GetDisplayWidth / Height) and lays itself out from it (gui_style_init), so changing the screen means changing these two
// numbers (or passing -DDISPLAY_WIDTH=128 -DDISPLAY_HEIGHT=128 to the build) and the platform's display driver; the UI code stays as it is.
#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH  128
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT 128
#endif

// A boot animation, for a board that plays one by itself while the machine starts (the first prototype; nothing to do elsewhere):
// `count` images of w x h pixels stacked in `frames` (1 bit a pixel, rows of ceil(w / 8) bytes, the leftmost pixel in the top bit:
// the layout of ui/gui.h's sprite sheets), shown frame_ms each, back and forth, in the middle of the screen. draw_text (or NULL)
// draws what stays over every frame (a name, a version): called once, with the display's own buffer. Call it before
// display_init; the struct and its frames must stay valid. Without it there is no animation.
typedef struct {
    const uint8_t *frames;
    uint8_t w, h, count;
    uint16_t frame_ms;
    void (*draw_text)(u8g2_t *g);
} display_boot_t;
void display_boot_config(const display_boot_t *boot);

// Creates/initialises the display and returns it. Never returns NULL.
u8g2_t *display_init(void);

// Shows the frame buffer the application has drawn (in place of u8g2_SendBuffer). The platform may copy it and send it later from its own
// task (the prototype: only the changed tiles, so a slow bus never holds up notes and knobs); the caller may draw the next frame at once.
void display_send(u8g2_t *g);

// The application has finished starting (the card was read, its settings and the pads' samples are in). A platform that plays a boot
// animation of its own until then (display_boot_config; the prototype loops it back and forth in a task) lets it end and then shows
// the newest frame sent; the frames sent before are kept, not shown. Called once; nothing to do elsewhere.
// display_boot_progress: how far that start is, 0..100, for a loading bar under such an animation (called while it changes, before
// display_boot_end; it never has to go back).
void display_boot_end(void);
void display_boot_progress(int percent);

#ifdef __cplusplus
}
#endif
