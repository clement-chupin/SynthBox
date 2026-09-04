# GrvEP — Simulateur Web (WebAssembly / Emscripten)

Le simulateur web compile le même code C++ que l'ESP32 en WebAssembly.  
La page HTML `docs/index.html` est un pont I/O pur — elle ne contient aucune logique applicative, seulement les entrées/sorties.

---

## Principe

```
docs/index.html              ← pont I/O pur (boutons, joystick, pots, OLED canvas, LEDs canvas)
docs/grvep.js                ← WASM + glue JS générés par Emscripten (Single-file, ~2 MB)

src/main.cpp  ─────────────────────────────────────────┐
src/audio_engine.cpp          code ESP32 compilé        │  emcc
src/NoteMap.cpp                SANS modification  ───────┤──────► docs/grvep.js
lib/AMY Synthesizer/src/                                │
lib/U8g2/src/                                          │
web/web_*.cpp  (drivers I/O)  ─────────────────────────┘
```

---

## Architecture des fichiers

```
web/
├── CMakeLists.txt      Build Emscripten → docs/grvep.js
├── build.sh            Script de build (source emsdk + emcmake + emmake)
├── web_main.cpp        Entry point : emscripten_set_main_loop(loop, 0, 1)
│                       + fonctions WASM exportées pour le pont JS
├── web_state.h/cpp     Globaux partagés WASM↔stubs (g_simJoyX, g_simSlider…)
├── web_keyboard.cpp    simKeyPress() — injecte les touches dans main.cpp
├── web_mux.cpp         g_simSlider[] → signal mux analogique (même logique que sim_mux.cpp)
├── web_oled.cpp        U8G2 en RAM buffer (pas d'I2C réel)
├── web_leds.cpp        g_simLeds[] lu par JS (même logique que sim_leds.cpp)
├── web_midi.cpp        No-ops (pas de USB MIDI en WASM)
├── web_mp3dec.cpp      Stubs Helix MP3 no-op (pas de filesystem en WASM)
├── web_amy_stubs.c     Stubs emscripten_lock_* (build single-threaded, sans pthreads)
└── hal/                Headers Arduino/FastLED/U8g2/SPI/Wire… (copie de simulator/hal/)

docs/
├── index.html          Interface HTML + JS pont I/O
└── grvep.js            Fichier généré (WASM embarqué en base64, ne pas éditer)
```

---

## Pont I/O JS ↔ WASM

### Inputs → WASM

| Entrée | Mécanisme |
|--------|-----------|
| Touche (press) | `Module.ccall('simKeyPress', null, ['number','number','number'], [row, col, 1])` |
| Touche (release) | `Module.ccall('simKeyPress', null, ['number','number','number'], [row, col, 0])` |
| Joystick X | `Module.HEAPF32[joyXPtr >> 2] = value` (-1.0 .. +1.0) |
| Joystick Y | `Module.HEAPF32[joyYPtr >> 2] = value` (-1.0 .. +1.0) |
| Joystick SW | `Module.HEAP8[joySWPtr] = 1` (puis 0 au release) |
| Potard i | `Module.HEAPF32[(sliderPtr >> 2) + i] = value` (0.0 .. 1.0) |

### Outputs ← WASM

| Sortie | Mécanisme |
|--------|-----------|
| OLED 128×128 | `getOledBuffer()` → ptr dans `HEAPU8`, format 1-bit page-major |
| LEDs SK6812 | `getLedsBuffer()` → ptr dans `HEAPU8`, 36 × 3 octets R,G,B |
| Drapeau OLED | `isOledDirty()` → bool (se réinitialise à la lecture) |
| Drapeau LEDs | `isLedsDirty()` → bool (se réinitialise à la lecture) |

### Format buffer OLED

```
buf[page * 128 + col]   (page = 0..15, col = 0..127)
bit 0 = pixel du haut de la page
```
16 pages × 128 colonnes × 8 bits = 128×128 pixels, 2048 octets total.

### Format buffer LEDs

```
buf[i * 3 + 0] = R
buf[i * 3 + 1] = G
buf[i * 3 + 2] = B
```
36 LEDs, 108 octets total.

---

## Fonctions exportées (web_main.cpp)

```c
extern "C" {
  void            simKeyPress(uint8_t row, uint8_t col, bool pressed);
  float*          getJoyXPtr();     // pointeur vers g_simJoyX
  float*          getJoyYPtr();     // pointeur vers g_simJoyY
  bool*           getJoySWPtr();    // pointeur vers g_simJoySW
  float*          getSliderPtr();   // pointeur vers g_simSlider[0]
  const uint8_t*  getOledBuffer();  // pointeur vers oled.getBufferPtr()
  int             getOledWidth();   // 128
  int             getOledHeight();  // 128
  const uint8_t*  getLedsBuffer();  // pointeur vers g_simLeds[0].r
  int             getLedsCount();   // NUM_LEDS (36)
  bool            isOledDirty();    // OLED rafraîchi depuis dernier appel ?
  bool            isLedsDirty();    // LEDs rafraîchies depuis dernier appel ?
}
```

Pour les appeler en JS :
```js
// Méthode directe (via WASM exports)
Module._simKeyPress(row, col, 1);

// Via ccall (type-safe)
Module.ccall('simKeyPress', null, ['number','number','number'], [row, col, 1]);

// Pointeurs (lecture/écriture mémoire WASM)
const joyXPtr = Module._getJoyXPtr();
Module.HEAPF32[joyXPtr >> 2] = 0.5;
```

---

## Flags de build Emscripten

| Flag | Rôle |
|------|------|
| `-sSINGLE_FILE=1` | Embarque le WASM en base64 dans le `.js` — pas de CORS sur GitHub Pages |
| `-sMODULARIZE=1 -sEXPORT_NAME=createGrvEP` | Module créé via `createGrvEP({...})` au lieu d'un global automatique |
| `-sASYNCIFY` | Requis par miniaudio pour le callback WebAudio (coroutine coopérative) |
| `-sINITIAL_MEMORY=67108864` | 64 Mo de départ pour AMY + audio buffers |
| `-sALLOW_MEMORY_GROWTH=1` | Permettre la croissance mémoire dynamique |
| `--no-entry` | `_main` géré par `emscripten_set_main_loop`, pas de valeur de retour directe |

---

## Différences avec l'ESP32 et le simulateur SDL2

| Aspect | ESP32 | Simulateur SDL2 | Simulateur Web |
|--------|-------|-----------------|----------------|
| Entrée clavier | TCA8418 I2C IRQ | SDL keyboard events | `simKeyPress()` via JS |
| Audio | I2S DAC | miniaudio → ALSA | miniaudio → WebAudio |
| OLED | SH1107 via I2C | SDL texture (×3) | Canvas 2D (bit-unpack) |
| LEDs | SK6812 DMA | SDL circles colorés | Canvas 2D rectangles |
| MP3 | Helix MP3 | libmpg123 (dlopen) | Stubs no-op |
| FreeRTOS | 6 tâches | pthread détaché | `emscripten_set_main_loop` |
| Filesystem SD | SDMMC | Natif Linux | Non disponible |
| Threads AMY | FreeRTOS tasks | pthreads | Single-threaded (stubs lock) |

---

## Build

### 1. Installer Emscripten (une seule fois)

```bash
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest
~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh

# Ajouter à ~/.bashrc pour que emcc soit disponible automatiquement
echo 'source ~/emsdk/emsdk_env.sh' >> ~/.bashrc
```

### 2. Compiler

```bash
cd web
./build.sh
```

Résultat : `docs/grvep.js` (~2 MB, WASM + JS glue embarqués en base64).

### 3. Tester localement

```bash
python3 -m http.server 8080
# Ouvrir http://localhost:8080/docs/
```

> Le fichier doit être servi via HTTP (pas `file://`). Les headers COOP/COEP ne sont **pas** requis (build single-threaded, pas de SharedArrayBuffer).

### 4. Rebuild incrémental

```bash
source ~/emsdk/emsdk_env.sh
cd web/build && emmake make -j$(nproc)
```

### 5. Publier sur GitHub Pages

```bash
git add docs/grvep.js docs/index.html
git commit -m "Update web simulator"
git push
# URL : https://clement-chupin.github.io/SynthBox/
```

---

## Stubs spécifiques au WASM

### web_amy_stubs.c — Threads AMY

AMY utilise `emscripten_lock_*` pour la synchronisation interne.  
Ces symboles n'existent pas dans un build single-threaded (sans `-pthread`).  
Solution : stubs triviaux qui simulent un mutex toujours libre.

### web_mp3dec.cpp — Décodeur MP3

Le décodeur Helix MP3 est remplacé par des stubs no-op.  
`MP3Decode` retourne `ERR_MP3_INDATA_UNDERFLOW` (comme si le buffer était vide).  
`audio_engine.cpp` détecte l'erreur et n'envoie pas de PCM — pas de crash.  
WAV fonctionne normalement via le chemin PCM direct de `audio_engine.cpp`.

---

## Limitations connues

- **Pas de MP3** : seul WAV fonctionne.
- **Pas d'accès SD** : les modes navigateur de fichiers (SAMPLE, GRANU, SS2) s'affichent mais ne listent pas de fichiers.
- **Pas de MIDI USB** : fonctions no-ops.
- **ASYNCIFY overhead** : le flag ASYNCIFY augmente légèrement la taille du bundle et le temps de démarrage.
- **AudioContext** : le navigateur exige une interaction utilisateur avant de lancer l'audio. Cliquer une touche suffit à démarrer le son.
