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
#ifdef _WIN32
#include <windows.h>
#endif

// Forward declarations from the real main.cpp (C++ functions)
void setup();
void loop();

// sim_window API
bool simWindowInit();
void simWindowDestroy();
bool simWindowPollEvents();
void simWindowRender();

// sim_spectrogram API (desktop only — see sim_spectrogram.cpp)
#ifndef __ANDROID__
bool simSpectrogramInit();
void simSpectrogramRender();
void simSpectrogramDestroy();
#endif

// sim_eq API (desktop only — see sim_eq.cpp)
#ifndef __ANDROID__
bool simEqInit();
void simEqRender();
void simEqDestroy();
#endif

static volatile bool s_quit = false;

static void* arduinoThread(void*) {
    setup();
    while (!s_quit) {
        loop();
    }
    return nullptr;
}

#ifdef _WIN32
// The Windows build links with -mwindows (GUI subsystem, no console window, so a
// plain double-click doesn't flash a black cmd box behind the app) — but that also
// means every Serial.printf()/printf() call (all the SD-card/file-load diagnostics
// this app relies on for troubleshooting) had nowhere to go and was silently
// discarded. AllocConsole() + redirecting stdout/stderr to it opens a normal console
// window alongside the app so that output is visible — simplest, most standard fix
// for this exact GUI-subsystem-vs-debug-output tradeoff; the app still has no
// console flash on systems where nothing gets printed before this runs, but here we
// always want it since debug visibility was the actual ask.
static void winOpenDebugConsole() {
    AllocConsole();
    SetConsoleTitleA("GrvEP - console de debug");
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$",  "r", stdin);
    printf("GrvEP - console de debug. Les messages IMG:/CACHE:/WAV:/MP3:/etc. du\n");
    printf("chargement de fichiers s'affichent ici. Fermer cette fenetre ferme l'appli.\n\n");
}
#endif

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
#ifdef _WIN32
    winOpenDebugConsole();
#endif

    if (!simWindowInit()) {
        fprintf(stderr, "Failed to create window\n");
        return 1;
    }
#ifndef __ANDROID__
    if (!simSpectrogramInit())
        fprintf(stderr, "Failed to create spectrogram window (continuing without it)\n");
    if (!simEqInit())
        fprintf(stderr, "Failed to create EQ window (continuing without it)\n");
#endif

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
#ifndef __ANDROID__
        simSpectrogramRender();
        simEqRender();
#endif
        // ~60 fps
        usleep(16000);
    }

#ifndef __ANDROID__
    simSpectrogramDestroy();
    simEqDestroy();
#endif
    simWindowDestroy();
    return 0;
}
