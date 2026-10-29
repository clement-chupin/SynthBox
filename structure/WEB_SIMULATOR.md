# GrvEP — Simulateur Web (WebAssembly / Emscripten)

Le simulateur web compile le même code C++ que l'ESP32 en WebAssembly.  
La page HTML `docs/index.html` est un pont I/O pur — elle ne contient aucune logique applicative, seulement les entrées/sorties.

---

## Principe

```
docs/index.html              ← pont I/O pur (boutons, joystick, pots, OLED canvas, LEDs canvas)
docs/coi-serviceworker.js    ← vendoré tel quel (voir "COOP/COEP" plus bas) — injecte les
                                headers nécessaires à SharedArrayBuffer côté client
docs/grvep.js + docs/grvep.wasm   ← glue JS + binaire WASM générés par Emscripten (PAS
                                en un seul fichier — voir "Flags de build" plus bas)

src/main.cpp  ─────────────────────────────────────────┐
src/audio_engine.cpp          code ESP32 compilé        │  emcc
src/NoteMap.cpp                SANS modification  ───────┤──────► docs/grvep.js + .wasm
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
├── web_amy_stubs.c     Stubs emscripten_lock_* qu'amy.c appelle mais que son chemin
│                       __EMSCRIPTEN__ normal (pthread réel) ne fournit pas directement
├── web_tasks.h/cpp     Scheduler coopératif à base d'Emscripten Fibers (voir sa propre
│                       section plus bas) — émule les 6 tâches FreeRTOS de l'ESP32/du
│                       simulateur SDL2 sur un seul thread JS
└── hal/                Headers Arduino/FastLED/U8g2/SPI/Wire… (copie de simulator/hal/,
                        sauf U8g2lib.h et SD.h — voir "Différences" plus bas)

docs/
├── index.html              Interface HTML + JS pont I/O (charge coi-serviceworker.js en 1er)
├── coi-serviceworker.js    Vendoré tel quel (gzuidhof/coi-serviceworker, MIT) — ne PAS éditer
├── grvep.js                Glue JS générée (ne pas éditer)
└── grvep.wasm              Binaire WASM généré (ne pas éditer)
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

**⚠️ Révision importante** — le plan d'origine (single-file, single-threaded, sans
pthread/SharedArrayBuffer) s'est révélé irréalisable une fois le build réellement tenté :
la copie vendorée de `miniaudio.h` (`lib/AMY Synthesizer/src/`) **n'a aucun chemin non-
worklet** dans sa branche `__EMSCRIPTEN__` — confirmé en lisant le fichier, chaque chemin
sous `#if defined(__EMSCRIPTEN__)` passe par `emscripten_create_wasm_audio_worklet_node()`.
Un Web Audio Worklet s'exécute nécessairement sur un thread séparé du thread principal, ce
qui exige de la mémoire **partagée** entre les deux — donc `-pthread` côté Emscripten, donc
`SharedArrayBuffer`, donc des pages servies **cross-origin-isolated** (headers
`Cross-Origin-Opener-Policy`/`Cross-Origin-Embedder-Policy`) dans tout navigateur qui
applique les mitigations Spectre. Rien de tout ça n'est optionnel une fois qu'on utilise
cette version de miniaudio en Emscripten — ce n'est pas un choix de design, c'est une
contrainte du fichier déjà vendoré.

| Flag | Rôle |
|------|------|
| `-sMODULARIZE=1 -sEXPORT_NAME=createGrvEP` | Module créé via `createGrvEP({...})` au lieu d'un global automatique |
| `-sASYNCIFY` | Requis par le scheduler de tâches à base de Fibers (`web_tasks.cpp`) |
| `-pthread` (compile **et** link) | Requis par le chemin Audio Worklet de miniaudio (`pthread_mutex_*`/`pthread_create`) — sans lui au compile, erreurs de lien "undefined symbol" |
| `-sAUDIO_WORKLET=1 -sWASM_WORKERS=1` | Active le support Wasm Audio Worklets d'Emscripten — seul chemin audio que ce miniaudio.h sait emprunter en Emscripten |
| `-sINITIAL_MEMORY=67108864` | 64 Mo de départ pour AMY + audio buffers |
| `-sALLOW_MEMORY_GROWTH=1` | Permettre la croissance mémoire dynamique |
| `--no-entry` | `_main` géré par `emscripten_set_main_loop`/`grvepBoot()`, pas de valeur de retour directe |

**`-sSINGLE_FILE=1` a dû être abandonné** — Emscripten refuse catégoriquement de le
combiner avec `-sAUDIO_WORKLET`/`-sWASM_WORKERS` ("`-sAUDIO_WORKLET does not support
-sSINGLE_FILE`", erreur de build directement observée). Résultat : `docs/grvep.js` +
`docs/grvep.wasm` comme deux fichiers séparés au lieu d'un seul — sans impact réel sur
l'hébergement GitHub Pages (ce sont deux fichiers statiques de même origine, aucun souci
CORS puisque CORS ne s'applique qu'aux requêtes cross-origin).

### COOP/COEP sur GitHub Pages : `docs/coi-serviceworker.js`

GitHub Pages ne permet de personnaliser **aucun** header de réponse — donc impossible de
servir nativement les headers COOP/COEP qu'exige `SharedArrayBuffer`. Solution : un
service worker (vendoré tel quel, `docs/coi-serviceworker.js`, MIT,
github.com/gzuidhof/coi-serviceworker) qui intercepte les requêtes de la page et y
injecte les headers côté client, sans configuration serveur. Chargé en tout premier dans
`docs/index.html` (avant `grvep.js`). Conséquence attendue, pas un bug : **un seul
rechargement silencieux de la page** au premier chargement dans un navigateur/session
donné (le service worker s'enregistre, puis force un reload pour prendre effet).

---

## Différences avec l'ESP32 et le simulateur SDL2

| Aspect | ESP32 | Simulateur SDL2 | Simulateur Web |
|--------|-------|-----------------|----------------|
| Entrée clavier | TCA8418 I2C IRQ | SDL keyboard events | `simKeyPress()` via JS |
| Audio | I2S DAC | miniaudio → ALSA | miniaudio → WebAudio |
| OLED | SH1107 via I2C | SDL texture (×3) | Canvas 2D (bit-unpack) |
| LEDs | SK6812 DMA | SDL circles colorés | Canvas 2D rectangles |
| MP3 | Helix MP3 | libmpg123 (dlopen) | Stubs no-op |
| FreeRTOS | 6 tâches | pthread détaché | Emscripten Fibers (`web_tasks.cpp`), un seul thread JS — voir plus bas |
| Filesystem SD | SDMMC | Natif Linux | Non disponible (`SD.h` du web HAL retourne toujours "pas de carte") |
| Threads AMY | FreeRTOS tasks | pthreads | **pthread réel** (Audio Worklet oblige — voir "Flags de build" plus haut), pas single-threaded comme prévu à l'origine |

### `web_tasks.cpp` — émulation des tâches FreeRTOS sans vrais threads JS

`xTaskCreatePinnedToCore` (dans `src/main.cpp`/`src/audio_engine.cpp`) crée des tâches
`for(;;){attente-bloquante; travail;}` — l'ESP32 et le simulateur SDL2 les exécutent comme
de vrais threads (FreeRTOS / pthread). Un seul thread JS principal ne peut pas faire ça
directement (bloquer dedans gèlerait l'onglet au premier appel). Solution : chaque tâche
tourne sur sa propre pile via **Emscripten Fibers** (`emscripten/fiber.h`, nécessite déjà
`-sASYNCIFY`) ; `vTaskDelay`/`xQueueReceive`/`xSemaphoreTake`/`ulTaskNotifyTake` rendent la
main au scheduler au lieu de bloquer, et `web_main.cpp` appelle `webTasksPump()` une fois
par frame (`requestAnimationFrame`) pour donner un tour à chaque tâche. Le fichier
`structure/WEB_SIMULATOR.md` d'origine ne détaillait pas ce point (le tableau ci-dessus
disait juste "`emscripten_set_main_loop`", qui gère la boucle `loop()` principale mais pas
ces 6 tâches séparées) — c'est la pièce de conception qui a dû être inventée en construisant
réellement ce build, pas une simple transcription du simulateur SDL2.

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

Résultat : `docs/grvep.js` (glue JS) + `docs/grvep.wasm` (binaire WASM, ~3 Mo).

### 3. Tester localement

```bash
python3 -m http.server 8080
# Ouvrir http://localhost:8080/docs/
```

> Le fichier doit être servi via HTTP (pas `file://`). **`docs/coi-serviceworker.js`
> émule les headers COOP/COEP nécessaires à `SharedArrayBuffer`** (voir "Flags de build"
> plus haut) — un simple `python3 -m http.server` suffit, aucune configuration serveur
> requise, mais attendre un rechargement automatique de la page au premier essai.

### 4. Rebuild incrémental

```bash
source ~/emsdk/emsdk_env.sh
cd web/build && emmake make -j$(nproc)
```

### 5. Publier sur GitHub Pages

```bash
git add docs/grvep.js docs/grvep.wasm docs/index.html docs/coi-serviceworker.js
git commit -m "Update web simulator"
git push
# URL : https://clement-chupin.github.io/SynthBox/
# Activer GitHub Pages dans Settings → Pages → Source = "main" / dossier "/docs" (une
# seule fois, ce n'est pas automatique) — aucun workflow Actions nécessaire.
```

---

## Stubs spécifiques au WASM

### web_amy_stubs.c — Threads AMY

AMY appelle `emscripten_lock_busyspin_wait_acquire()`/`emscripten_lock_release()` à
certains points de synchronisation internes qui n'ont pas d'équivalent direct dans ce
contexte. Stubs triviaux qui simulent un verrou toujours disponible (`bool
emscripten_lock_busyspin_wait_acquire(...)`, pas `void` — la signature exacte attendue
par `amy.c`, source de la seule vraie erreur de compilation rencontrée en écrivant ces
stubs).

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
- **AudioContext** : le navigateur exige une interaction utilisateur avant de lancer l'audio. `docs/index.html` appelle `grvepBoot()` au premier clic sur une touche/le joystick/un potard (pas avant), ce qui satisfait cette exigence.
- **Pas de pthread limité à l'audio** : contrairement au plan d'origine, ce build utilise bel et bien `-pthread` (imposé par l'Audio Worklet, voir "Flags de build" plus haut) — `web_tasks.cpp`'s propre scheduler de tâches reste cependant basé sur des Fibers coopératives sur un seul thread logique, pas sur de vrais threads système supplémentaires.
- **⚠️ Non vérifié dans un vrai navigateur** : l'environnement où ce build a été écrit n'avait ni Chrome/Chromium ni Node avec `window.AudioContext` disponible (Node n'a jamais cet objet — ce n'est pas un navigateur). Le build compile, link, et `setup()` tourne jusqu'à `"Ready"` sous Node (clavier/OLED/LEDs/mux tous fonctionnels) ; seul le tout dernier maillon — `emscripten_create_wasm_audio_worklet_node()` créant réellement un `AudioContext`/`AudioWorkletNode` et produisant du son — reste à confirmer en ouvrant `docs/index.html` dans un vrai navigateur et en vérifiant la console (erreurs liées à l'AudioContext ou au cross-origin-isolation) au premier clic.
