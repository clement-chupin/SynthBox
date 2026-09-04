// sim_main.cpp — entry point for the GrvEP PC simulator
// Calls the real setup() in a pthread, then runs the SDL event loop.

// SDL_main.h must be included before main() is defined.
// On Android it applies: #define main SDL_main
// so SDLActivity can find the SDL_main symbol in libmain.so.
// On Linux/Desktop this include is harmless (no rename occurs).
#include <SDL2/SDL.h>

#include "sim_state.h"
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

// Forward declarations from the real main.cpp (C++ functions)
void setup();
void loop();

// sim_window API
bool simWindowInit();
void simWindowDestroy();
bool simWindowPollEvents();
void simWindowRender();

static volatile bool s_quit = false;

static void* arduinoThread(void*) {
    setup();
    while (!s_quit) {
        loop();
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (!simWindowInit()) {
        fprintf(stderr, "Failed to create window\n");
        return 1;
    }

    // Start the Arduino setup()/loop() thread
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, arduinoThread, nullptr);
    pthread_attr_destroy(&attr);

    // SDL event loop (must run on main thread on most platforms)
    while (!s_quit) {
        if (!simWindowPollEvents()) {
            s_quit = true;
            break;
        }
        simWindowRender();
        // ~60 fps
        usleep(16000);
    }

    simWindowDestroy();
    return 0;
}
