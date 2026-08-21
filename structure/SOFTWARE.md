# GrvEP / SynthBox — Software Architecture

> **Contexte session Claude** : Ce fichier permet à une nouvelle session Claude de reprendre le projet sans historique. Fournir aussi `HARDWARE.md`. Le projet vit dans `/home/cchupin/projects/GrvEP` (session active) et est mirroir-copié dans `/home/cchupin/projects/SynthBox` (synchro manuelle via `cp`).

---

## Build

```bash
~/.platformio/penv/bin/pio run               # compile seulement
~/.platformio/penv/bin/pio run -t upload     # compile + flash
```

Plateforme : ESP32-S3, framework Arduino, PlatformIO.  
La librairie AMY est compilée depuis `.pio/libdeps/esp32-s3-devkitc-1/AMY Synthesizer/src/`.  
**Toute modification d'AMY doit se faire dans `.pio/libdeps/…/amy.c`, pas dans le dossier source original.**

---

## Structure des fichiers

```
src/
├── main.cpp            Boucle principale : audio → clavier → joystick → LEDs → pots → écran
├── config.h            Enum AppMode, MenuItem, SynthShape, EnvParams, constantes AMY
├── audio_engine.cpp/h  Toute la couche AMY : init, noteOn/Off, FX, 303S, granular, tracker
├── keyboard.cpp/h      Driver TCA8418 (I2C), file d'événements clavier
├── Leds.cpp/h          Driver SK6812 RGBW
├── mux.cpp/h           MUX 16 canaux analogiques (potentiomètres)
├── NoteMap.cpp/h       Mapping note/gamme/octave
├── oled.cpp/h          Init SH1107 128×128 (U8G2)
├── wifi_audio.cpp/h    Streaming audio Wi-Fi

structure/
├── HARDWARE.md         Pins, composants, câblage
└── SOFTWARE.md         Ce fichier — architecture logicielle
```

---

## Modes (24 implémentés)

| Mode | Const | Description courte |
|------|-------|--------------------|
| SYNTH | `MODE_SYNTH` | Polyphonique, 20+ shapes AMY (SAW, FM, SUPERSAW, ACID…) |
| OMNI | `MODE_OMNI` | Omnichord : accords + strum joystick |
| DRUMS | `MODE_DRUMS` | Drum machine + séquenceur 8 steps |
| SAMPLE | `MODE_SAMPLE` | Lecteur samples SD (wav/mp3) mappés sur le clavier |
| FX | `MODE_FX` | Chaîne d'effets (LPF, reverb, chorus, delay, EQ, overdrive, LFO) |
| LIGHT | `MODE_LIGHT` | LED light show (strip SK6812) |
| SEQ | `MODE_SEQ` | Séquenceur 4 pistes × 8 steps (samples) |
| LIGHTPLAY | `MODE_LIGHTPLAY` | Ripples lumineux en live sur les touches |
| BATTERY | `MODE_BATTERY` | Affichage tension batterie |
| SYSINFO | `MODE_SYSINFO` | HUD système cyberpunk |
| HYBRID | `MODE_HYBRID` | Note synth + sample simultanément |
| MODULAR | `MODE_MODULAR` | 6 encodeurs : OSC/filter/env/LFO + joystick vélocité |
| SYNTH2 | `MODE_SYNTH2` | Diapasonix : 258 patches Juno+DX7 |
| MOD2 | `MODE_MOD2` | PolyAnalog : morphing waveform, filtre power-law, LFO dest toggle |
| 303 | `MODE_303` | TB-303 : SAW/SQR + LPF résonant + envelope filtre + slide + accent |
| GRANULAR | `MODE_GRANULAR` | Granular slicer : 8 ou 16 tranches d'un sample SD |
| GRANULAR2 | `MODE_GRANULAR2` | Granular multi-sample (4 slots), fwd+rev, split par pad |
| MIDI | `MODE_MIDI` | USB MIDI device : clavier → NoteOn/Off, host → feedback LEDs |
| TRACKER | `MODE_TRACKER` | Enregistreur quantisé 32 steps : 4×4 instruments + 4×4 notes |
| DRUM2 | `MODE_DRUM2` | TR-808 style : 8 pads, pitch/decay/vol par pad, séquenceur, FX |
| SYSEQ | `MODE_SYSEQ` | Séquenceur polyphonique 16 steps (jusqu'à 4 notes/step) |
| 303S | `MODE_303S` | Séquenceur TB-303 16 steps (note + accent + slide par step) |
| SS2 | `MODE_SS2` | Séquenceur sample 16 steps : 16 slots, altération par step |
| ANIM | `MODE_ANIM` | Animations visuelles sur l'écran : WAVE/BARS/TECHNO/ACID/8BIT |

---

## Architecture audio AMY

### Canaux AMY (synth channels)

| Canal | Constante | Usage |
|-------|-----------|-------|
| 1 | `SYNTH_CH` | Synth principal (SYNTH, MOD2, MODULAR, HYBRID, OMNI…) |
| 2 | `T303_CH` | TB-303 monophonique (MODE_303, MODE_303S) |
| 3–9 | `TRACKER_SYNTH_CH_BASE + 0..6` | Tracker : 7 pistes synth indépendantes |
| 60 | `PCM_PREVIEW_OSC` | Preview sample SD browser |
| 182–213 | `SAMPLE_OSC_BASE + 0..31` | Lecture keys samples (polyphonique) |
| 150–181 | `GRANULAR_OSC_BASE + 0..31` | Granular slicer slices |

### Bus AMY

| Bus | Usage |
|-----|-------|
| 0 | Drums, synth principal, tout le reste |
| 1 | T303 exclusivement (`e.bus = 1` dans `audioT303Init`) |

**Le bus 1 est mergé dans le bus 0 avant le traitement FX** (implémenté dans `amy.c`, bloc "Merge T303 bus into bus 0 BEFORE FX"). Cela permet à reverb/chorus/echo de traiter le signal T303 sans allouer de delay lines supplémentaires.

### Amplitude des buffers AMY (CRITIQUE)

Les buffers de bus AMY ont une amplitude **±9 environ** (pas ±1). La raison : `volume_scale[bus] = 0.1 × volume[bus]` est appliqué **après** le buffer au mix-down, pas avant. Toute manipulation directe du buffer (ex. wavefolder) doit normaliser d'abord.

### Breakpoints AMY (CRITIQUE)

`bp_arrays_set = AMY_IS_SET(eg0_times[0]) || AMY_IS_SET(eg0_values[0])` — AMY ne met à jour l'enveloppe que si **l'index 0** est défini. Envoyer uniquement l'index 1 ou 2 → silence silencieux. Toujours envoyer les 3 breakpoints (attack, decay, release) avec index 0 inclus.

### Wavefolder (MODE_303S, 303)

- Variable globale dans `amy.c` : `float amy_wavefold_gain = 1.0f;`
- Déclaré extern dans `audio_engine.cpp` : `extern "C" float amy_wavefold_gain;`
- Contrôlé par `audioT303Wavefold(float depth)` → `gain = powf(4.0f, depth)` (1→4)
- Position dans `amy.c` : **avant** le bloc FX par bus, **après** le merge des cores dual-core
- Normalise peak → fold → dénormalise pour éviter le silence par over-folding

---

## MODE_303S — TB-303 Step Sequencer

### Waveforms

```cpp
static const uint8_t kT303Waves[] = { TRIANGLE, SAW_UP, SAW_DOWN, PULSE, T303_NAP_WAVE };
#define T303_NAP_WAVE 200  // sentinelle — PULSE + duty cycle étroit [0.01, 0.15]
```

- **TRI/SAW_UP/SAW_DOWN** : modulé par P2 via wavefolder (`audioT303Wavefold`)
- **PULSE** : P2 contrôle le duty cycle [0.01, 0.49] (`audioT303Duty`)
- **NAP** (Narrow Pulse) : duty cycle [0.01, 0.15], P2 inverse

### Envelope (Dur)

Decay-only ADSR : attack=2ms, decay=`t303Duration×3000ms`, sustain=0, release=30ms.  
Les 3 breakpoints doivent tous être envoyés car AMY vérifie index 0.

### Paramètres affichés

- Gauche : `Dur` (durée note 0–100%), Droite : `Mod` (envMod)
- P1 = volume global (bus 0 ET bus 1 via `audioSetVolume`)
- P2 = wavefold/duty selon wave courante

### Widget waveform (bas de l'écran, 128×20px)

Affiché en temps réel dans les vues SEQ et PAD du mode 303S.  
Calcule la forme d'onde depuis `pots[1].value` à chaque frame d'affichage.

---

## FreeRTOS — Tâches

| Tâche | Core | Priorité | Rôle |
|-------|------|----------|------|
| `loop()` (main) | Core 1 | 1 | Boucle principale |
| `audioHandlerTask` | Core 1 | 22 | Traitement événements clavier → AMY |
| `ledUpdateTask` | Core 0 | 4 | Mise à jour LEDs SK6812 |
| `statsTask` | Core 0 | 1 | Statistiques CPU/RAM |
| AMY render | Core 0 | 23 | Rendu audio (interne AMY) |
| `kbdPollTask` | Core 0 | 8 | Poll TCA8418 clavier |

Les appels AMY depuis le clavier passent par une file (`s_audioEventQueue`) traitée dans `audioHandlerTask` sur Core 1 — même core que le render AMY pour éviter les races.

---

## Affichage OLED (SH1107 128×128)

- Bibliothèque : U8G2 full buffer (`U8G2_SH1107_128X128_F_HW_I2C`)
- I2C à 400kHz → `sendBuffer()` bloque ~47ms
- Refresh limité : 100ms normal, **200ms si BPM > 250** (évite jitter séquenceur)
- Toutes les fonctions `oled.draw*()` écrivent en RAM (rapide), `sendBuffer()` transfère via I2C (lent)

---

## Menu

- Grille 3 colonnes × 8 rangées (24 items total)
- 4 rangées visibles à la fois, scroll vertical via joystick
- Navigation : joystick Y = haut/bas, joystick X = gauche/droite
- ANIM est en **rangée 7, colonne 2** (dernière cellule)

---

## MODE_ANIM — Animations

5 animations cyclables (B1=suivante, B3=précédente, touche grille=suivante) :

| Index | Nom | Algorithme | P2 |
|-------|-----|------------|-----|
| 0 | WAVE | Double onde sinus scrollante | Amplitude 2ème vague |
| 1 | BARS | 16 barres VU-meter rebondissantes | Hauteur max |
| 2 | TECHNO | Radar tournant + 3 cercles concentriques | Position du blip |
| 3 | ACID | Figure de Lissajous (ratio freq 2→3) | Ratio fréquence |
| 4 | 8BIT | Pluie de pixels style Matrix (16 colonnes) | Longueur traînées |

P1 = vitesse pour toutes. Séquenceur continue de jouer en mode ANIM.  
LEDs : rainbow wave synchro avec la vitesse animation.

---

## Effets (MODE_FX)

7 effets configurables, stockés dans `fxList[]` :

| Index | Nom | Implémentation AMY |
|-------|-----|--------------------|
| 0 | LPF | `audioSetAllFiltersT` |
| 1 | Reverb | `config_reverb(bus 0, …)` |
| 2 | Chorus | `config_chorus(bus 0, …)` |
| 3 | Delay | `config_echo(bus 0, …)` |
| 4 | EQ | `config_eq(bus 0, …)` |
| 5 | Overdrive | `audioSetOverdrive` |
| 6 | LFO | LFO interne, pilote cutoff/pitch |

Les FX ciblent le bus 0. La 303S (bus 1) est mergée dans le bus 0 **avant** le traitement FX dans `amy.c`, donc les effets s'appliquent au signal combiné sans allocation mémoire supplémentaire.

---

## Pots (encodeurs rotatifs)

| Pot | Plage | Rôle contextuel |
|-----|-------|-----------------|
| P0 (pot0) | 0.0–2.0 | Volume global (pMax=2.0 spécifiquement) |
| P1 (pot1) | 0.0–1.0 | Cutoff / modulation / P2 contextuel |
| P2–P6 | 0.0–1.0 | Contextuels selon le mode |

`pots[0].value` peut atteindre 2.0 — toujours clamp avec `min(100, (int)(val*100))` pour les barres de progression.

---

## Bugs corrigés (non-évidents)

### AMY breakpoint index 0 obligatoire
Envoyer uniquement `eg0_times[1]` ne fait rien. AMY vérifie `AMY_IS_SET(eg0_times[0])` avant de traiter. Toujours envoyer index 0 (attack).

### Amplitude buffer bus AMY ≠ ±1
Buffer AMY bus ≈ ±9. Le wavefolder original sans normalisation produisait du quasi-silence (over-fold). Fix : scan peak → normalise → fold → dénormalise.

### Volume T303 (bus 1)
`audioSetVolume` ne mettait à jour que `e.volume[0]`. Bus 1 restait à `volume[1]=1.0` → scale=0.1×1.0=0.1, quasi-inaudible. Fix : mettre à jour `e.volume[0]` et `e.volume[1]` ensemble.

### Barre volume > 100%
`pots[0].value` peut valoir 2.0 → `(int)(val*100) = 200`. Toujours `min(100, ...)`.

### `config_reverb/echo` sur bus 1 → crash
Ces fonctions allouent ~256KB de delay lines en PSRAM. Les appeler sur bus 0 ET bus 1 double l'allocation → PSRAM saturée. Solution : merger bus 1 dans bus 0 dans `amy.c` avant les FX.
