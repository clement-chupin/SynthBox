# GrvEP — Simulateur PC (SDL2)

Le simulateur compile le code C++ du GrvEP **sans modification** en natif Linux.  
Les drivers matériels (clavier TCA8418, LEDs SK6812, OLED SH1107, pots, MIDI) sont remplacés par des stubs SDL2.

---

## Architecture

```
simulator/
├── CMakeLists.txt      Build natif Linux (SDL2 + pthreads)
├── build.sh            Script de build (installe SDL2 localement si absent)
├── sim_main.cpp        Entry point : setup() dans un pthread, SDL event loop principal
├── sim_state.h/cpp     Variables partagées SDL↔stubs (g_simJoyX, g_simSlider…)
├── sim_window.cpp      Rendu SDL2 : OLED, clavier, pots, joystick, LEDs
├── sim_keyboard.cpp    Injecte les touches via simKeyPress()
├── sim_mux.cpp         Convertit g_simSlider[] en signal mux analogique (angles)
├── sim_oled.cpp        U8G2 RAM buffer (pas d'I2C)
├── sim_leds.cpp        g_simLeds[] copié depuis FastLED.show()
├── sim_midi.cpp        No-ops (pas d'USB MIDI en simulation)
├── sim_mp3dec.cpp      Décodeur MP3 via libmpg123 (dlopen)
├── hal/                Headers Arduino/FastLED/U8g2/SPI/Wire/freertos…
└── sdl2_local/         SDL2 installé localement (pas besoin de sudo)
```

Le code compilé inclut directement `src/main.cpp`, `src/audio_engine.cpp`, `src/NoteMap.cpp` — même code que l'ESP32.

---

## État partagé (sim_state.h)

```c
float g_simJoyX;        // -1.0 .. +1.0  (axe X joystick)
float g_simJoyY;        // -1.0 .. +1.0  (axe Y joystick)
bool  g_simJoySW;       // bouton joystick pressé
float g_simSlider[16];  // 0.0 .. 1.0   (16 canaux MUX / 7 pots)
volatile bool g_oledDirty;  // U8G2 a appelé sendBuffer()
volatile bool g_ledsDirty;  // FastLED.show() a appelé simLedsShow()
```

`sim_window.cpp` écrit dans ces variables depuis l'event loop SDL.  
Les stubs `sim_mux.cpp`, `sim_keyboard.cpp` lisent/exposent ces variables.

---

## Threads

```
Thread SDL (principal, Core 1)
  └── SDL_PollEvents → mouse/keyboard → g_simJoy*, g_simSlider[], simKeyPress()
  └── SDL_Render → OLED (g_oledDirty), LEDs (g_ledsDirty), pots, joystick

Thread Arduino (pthread détaché)
  └── setup()  → init AMY, clavier, OLED, LEDs, pots
  └── loop()   → lecture pots (via sim_mux), events clavier (sim_keyboard),
                  mise à jour OLED (U8G2 → g_oledDirty), LEDs (FastLED → g_ledsDirty)
```

L'audio AMY tourne dans ses propres threads internes (miniaudio).

---

## Conversion pots (sim_mux.cpp)

`main.cpp` lit les pots via `Multiplexer::getValue(addr)` qui retourne une valeur ADC 0–4095.  
`readPots()` dans `main.cpp` calcule : `angle = atan2(muxCache[3+i*2]-2048, muxCache[2+i*2]-2048)`.

`sim_mux.cpp` simule ce protocole : `g_simSlider[i]` (0→1) est converti en un angle interne qui évolue progressivement, puis converti en cosinus/sinus → valeurs ADC 0–4095. Cela permet à `readPots()` original de fonctionner sans modification.

Mapping des amplitudes angulaires :
- Pot 0 (volume, pMax=2.0) : `slider 0→1` → `angle +4π`
- Pot 1 (shape, pMax=1.0)  : `slider 0→1` → `angle +2π`
- Pots 2–6 (pMax=1.0)      : `slider 0→1` → `angle -2π` (signe négatif car sign=-1)

---

## OLED (sim_oled.cpp + sim_window.cpp)

U8G2 est initialisé en mode RAM buffer (`U8G2_SH1107_128X128_F_HW_I2C`) sans callback I2C réel.  
`oled.sendBuffer()` positionne `g_oledDirty = true`.

`sim_window.cpp` détecte `g_oledDirty` et lit `oled.getBufferPtr()` (2048 octets, format 1-bit page-major) pour mettre à jour une texture SDL.

Format buffer U8G2 SH1107 128×128 :
```
buf[page * 128 + col]  (page = 0..15, col = 0..127)
bit 0 = pixel du haut de la page
```

Rendu SDL : chaque bit → pixel cyan (`#1ad8ff`) ou noir.  
Scale : ×3 → affichage 384×384 px.

---

## LEDs (sim_leds.cpp + sim_window.cpp)

`FastLED.show()` appelle `simLedsShow(leds, NUM_LEDS)` qui copie dans `g_simLeds[]`.

`sim_window.cpp` affiche les 36 LEDs en dessous de la grille de touches, avec la table de correspondance `kLedTable[]` qui mappe `(row, col)` → index LED :

```cpp
static const int kLedTable[40] = {
    32, 33, 34, 35,            // row 4 cols 0-3 (boutons)
    32, 33, 34, 35,            // row 4 cols 4-7
    31, 30, 29, 28, 27, 26, 25, 24,  // row 3 (grave)
    16, 17, 18, 19, 20, 21, 22, 23,  // row 2
    15, 14, 13, 12, 11, 10,  9,  8,  // row 1
     0,  1,  2,  3,  4,  5,  6,  7,  // row 0 (aigu)
};
```

Index calculé : `x = (4 - row) * 8 + (7 - col)` → `kLedTable[x]` → index dans `g_simLeds[]`.

---

## Décodeur MP3 (sim_mp3dec.cpp)

`audio_engine.cpp` utilise l'API Helix MP3 (`MP3InitDecoder`, `MP3Decode`, etc.).  
Le simulateur branche cette API sur `libmpg123` via `dlopen` dynamique.

Contrat avec `audio_engine.cpp` :
- `MP3Decode` : feed **tous** les bytes restants, retourne `ERR_MP3_NONE` + PCM décodé
- `MP3GetLastFrameInfo` : retourne sample-rate, channels, outputSamps du dernier frame
- Retourne `ERR_MP3_MAINDATA_UNDERFLOW` quand plus d'input (caller refill et recommence)

---

## Build

```bash
cd simulator
./build.sh         # configure + compile
./build/grvep_sim  # lancer le simulateur
```

Dépendances : SDL2 (installé localement dans `sdl2_local/`), libmpg123 (package système).

---

## Raccourcis clavier SDL

| Touche | Action |
|--------|--------|
| Rangée 1 : `1`–`8`   | Note row 0 (aigu), col 0–7 |
| Rangée 2 : `Q`–`I`   | Note row 1, col 0–7 |
| Rangée 3 : `A`–`K`   | Note row 2, col 0–7 |
| Rangée 4 : `Z`–`,`   | Note row 3 (grave), col 0–7 |
| `←` / `→`            | Octave − / + |
| `↑`                   | Gamme suivante |
| `Esc`                 | Clic joystick (ouvre/ferme menu) |
| Espace                | All notes off |
| Glisser pots          | Clic + drag vertical sur les sliders |
| Clic joystick         | Clic sur le cercle joystick |
| Drag joystick         | Drag dans le cercle joystick |
