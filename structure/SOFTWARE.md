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

## Modes (32 implémentés)

> Table régénérée depuis l'enum `AppMode` de `config.h` (source de vérité — en cas de
> doute, relire l'enum directement). Les anciens noms `MODE_DRUMS`/`MODE_FX`/`MODE_SEQ`/
> `MODE_HYBRID`/`MODE_SYNTH2`/`MODE_303`/`MODE_GRANULAR` qui apparaissaient dans une
> version antérieure de cette table n'existent plus dans le code actuel — ne pas s'y fier.

| Mode | Const | Description courte |
|------|-------|--------------------|
| SYNTH | `MODE_SYNTH` | Polyphonique, 35+ shapes AMY (SAW, FM, SUPERSAW, ACID, patches Juno/DX7…) |
| OMNI | `MODE_OMNI` | Omnichord : accords + strum joystick |
| SAMPLE | `MODE_SAMPLE` | Lecteur samples SD (wav/mp3) mappés sur le clavier |
| LIGHT | `MODE_LIGHT` | LED light show (strip SK6812) |
| LIGHTPLAY | `MODE_LIGHTPLAY` | Ripples lumineux en live sur les touches |
| BATTERY | `MODE_BATTERY` | Affichage tension batterie |
| SYSINFO | `MODE_SYSINFO` | HUD système cyberpunk |
| MOD2 | `MODE_MOD2` | Synthé analogique modulaire : algorithmes + modulation P4-P7 |
| GRANULAR2 | `MODE_GRANULAR2` | Granular multi-sample (2 ou 4 slots), fwd+rev, split par pad |
| MIDI | `MODE_MIDI` | USB MIDI device : clavier → NoteOn/Off, host → feedback LEDs |
| TRACKER | `MODE_TRACKER` | Enregistreur quantisé 32 steps : 4×4 instruments + 4×4 notes |
| DRUM2 | `MODE_DRUM2` | TR-808 style : 8 pads, pitch/decay/vol par pad, séquenceur, FX |
| SYSEQ | `MODE_SYSEQ` | Séquenceur polyphonique 16 steps (jusqu'à 4 notes/step) |
| 303S | `MODE_303S` | Séquenceur TB-303 16 steps (note + accent + slide par step) |
| SS2 | `MODE_SS2` | Séquenceur sample 16 steps : 16 slots, altération par step |
| ANIM | `MODE_ANIM` | Animations visuelles sur l'écran : WAVE/BARS/TECHNO/ACID/8BIT |
| I303 | `MODE_I303` | TB-303 polyphonique : 6 voix accord/mélodie, moteur sonore 303S complet |
| VID | `MODE_VID` | Lecteur vidéo : fichiers `.bvid` 128×128 1bpp depuis la carte SD |
| LANIM | `MODE_LANIM` | Animations LED : flash/rainbow/chase/noise/organic, pilotées aux pots |
| EXP | `MODE_EXP` | Thérémine expérimental : joystick Y=pitch X=texture, touches=modificateurs |
| EXP2 | `MODE_EXP2` | PolyBounce : balles physiques dans un hexagone tournant, rebonds = notes |
| EXP3 | `MODE_EXP3` | Orbital : planètes en orbite autour d'une étoile, zone de passage = notes |
| 303S2 | `MODE_303S2` | Enregistreur 303 live : les 13 dernières notes jouées bouclent au BPM |
| POKEMON | `MODE_POKEMON` | Mode thérémine à la Pokémon : 25 timbres de synthèse AMY |
| MODULAR | `MODE_MODULAR` | **Synthé wavetable double-oscillateur** (voir section dédiée plus bas) |
| GEST | `MODE_GEST` | Gestionnaire de séquenceurs : grille de patterns 4×8, copier/coller, LOOP/LIVE |
| PCMCLEAN | `MODE_PCMCLEAN` | Nettoyeur de cache SD : supprime récursivement les `.pcm`/`.pcm16` |
| STONE | `MODE_STONE` | Sample-tone : un sample SD réparti sur tout le clavier |
| DR2 | `MODE_DR2` | Séquenceur batterie hiérarchique : 64 steps adressés en beat.step.micro (4.4.4) |
| IMPORT | `MODE_IMPORT` | Android uniquement : sélecteur de dossier SAF, importe les fichiers du téléphone |
| **LIFE** | `MODE_LIFE` | **Nouveau** — Jeu de la vie de Conway sur la grille de touches (colonne=hauteur, naissance=note) |
| **SWARM** | `MODE_SWARM` | **Nouveau** — Essaim de boids (cohésion/séparation/alignement), zone d'attraction pilotée au joystick |

Trois modes « expérimentaux » historiques (EXP/EXP2/EXP3) partagent une philosophie : jouer
des notes/sons intéressants sans connaissance de théorie musicale, contrairement aux modes
plus classiques (SYNTH, 303S…) qui s'adressent à un public plus musicien. LIFE et SWARM
prolongent cette famille — voir la section « Modes expérimentaux » plus bas.

---

## Architecture audio AMY

### Canaux AMY (synth channels)

| Canal | Constante | Usage |
|-------|-----------|-------|
| 1 | `SYNTH_CH` | Synth principal (SYNTH, MOD2, OMNI, POKEMON…) |
| 2 | `T303_CH` | TB-303 monophonique (303S, I303, 303S2) |
| 3–9 | `TRACKER_SYNTH_CH_BASE + 0..6` | Tracker : 7 pistes synth indépendantes |
| 10–13 | `SW2_CH_BASE + 0..3` | SW2 : couches sous-octave ±1/±2 demi-tons (303S/I303) |
| 14 | `MOD3_OSCA_CH` | **Nouveau** — MODE_MODULAR, oscillateur A (wavetable) |
| 15 | `MOD3_OSCB_CH` | **Nouveau** — MODE_MODULAR, oscillateur B (wavetable, détune fixe) |
| 60 | `PCM_PREVIEW_OSC` | Preview sample SD browser |
| 70–101 | `DRUM_OSC_BASE + 0..31` | Pads batterie (32 sons uniques) |
| 150–181 | `GRANULAR_OSC_BASE + 0..31` | Granular slicer slices |
| 182–213 | `SAMPLE_OSC_BASE + 0..31` | Lecture keys samples (polyphonique) |
| 240–245 | `STONE_OSC_BASE + 0..5` | STONE : 6 voix round-robin sur un seul sample |

`MODE_MODULAR` (canaux 14-15) n'appartient PAS à `SYNTH_CH` — c'est le seul autre mode
(avec STONE/GRANULAR2/DRUM2/SAMPLE) à utiliser des canaux/oscillateurs dédiés plutôt que
l'allocateur de voix dynamique d'AMY. Piège classique : `audioSetAllFilters()` /
`audioSetAllFiltersT()` (utilisées par le FX FILT partagé) sont câblées par défaut pour
`SYNTH_CH` — tout nouveau canal dédié doit explicitement y être ajouté (voir
`audioApplyFilterToStone()` / `audioApplyFilterToModular()` dans `audio_engine.cpp` comme
patron), sinon le FX FILT semble actif mais ne modifie rien pour ce canal.

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

## Effets (grille FX, overlay `OVERLAY_FX`)

16 effets configurables, stockés dans `fxList[]` (`main.cpp`), accessibles via une grille
4×4 dans l'overlay FX (B1 court sur SYNTH/POKEMON/I303/STONE/OMNI/MODULAR/GRANULAR2/DR2/GEST) :

| Index | Nom | Implémentation |
|-------|-----|-----------------|
| 0 | FILT | `audioSetAllFiltersT` (LPF/HPF/BPF) ou `audioSetLadderFilter` (type LADDER, bus 0, saturation tanh) |
| 1 | DISTORT | `audioSetDistortion` |
| 2 | REVERB | `audioSetReverb` (bus 0) |
| 3 | CHORUS | `audioSetChorus` (bus 0) |
| 4 | FLANGER | `config_chorus` (délai court = comb filter) |
| 5 | DELAY | `audioSetDelay`, synchronisé au BPM |
| 6 | LFO | Modulation du cutoff FILT, tick 10ms dans `loop()` |
| 7 | EQ | `audioSetEq` (3 bandes) |
| 8 | RESECHO | Écho synchronisé BPM avec filtre tonal dans la boucle de feedback |
| 9 | REP | Wavefold global (`audioSetWavefold`) |
| 10 | BITCRS | Wavefold à gain extrême (simule une réduction de bit-depth) + LPF optionnel |
| 11 | TREMOLO | Modulation du volume, tick 10ms |
| 12 | AUTOPAN | Modulation du pan, tick 10ms |
| 13 | OVERDRIVE | `audioSetOverdrive` |
| 14 | RINGMOD | Modulation en anneau (porteuse sinus, bus 0) |
| 15 | COMPRESSOR | Compresseur feedforward à enveloppe de crête (bus 0) |

Les FX bus-0 (REVERB/CHORUS/DELAY/EQ/RESECHO/REP/BITCRS/RINGMOD/COMPRESSOR/FILT-LADDER)
s'appliquent après le mix, donc à **toute** source sonore (synthé, sample, granulaire…).
FILT (types LPF/HPF/BPF) et DISTORT/OVERDRIVE en revanche ciblent des canaux/oscillateurs
AMY précis — voir l'avertissement dans la table des canaux ci-dessus : historiquement,
seul `SYNTH_CH` (+ GRANULAR2 via un masque d'oscillateurs actifs) était couvert ; STONE et
MODULAR ont été ajoutés ensuite (`audioApplyFilterToStone`/`audioApplyFilterToModular`).
La 303S (bus 1) est mergée dans le bus 0 **avant** le traitement FX dans `amy.c`, donc les
effets bus-0 s'appliquent au signal combiné sans allocation mémoire supplémentaire.

### Automatisation des FX (double-clic) — `OVERLAY_FX_MOD`

Double-cliquer un slot FX **déjà actif** dans la grille FX ouvre un éditeur dédié
(`OVERLAY_FX_MOD`) plutôt que de désactiver l'effet : joystick X change quel paramètre du
FX est automatisé, les pots P4-P7 règlent profondeur/vitesse/forme d'onde/synchro BPM.
Repose sur un moteur de modulation générique (`gModSlots[]`, struct `ModSlot`, `main.cpp`)
— sine/tri/carré/sample&hold, Hz libre ou synchronisé au BPM (table `kDelaySubdiv[]`) —
qui généralise le pattern déjà utilisé par les FX LFO/TREMOLO/AUTOPAN (accumulateur de
phase + porte de profondeur + restauration propre à la désactivation), sans les modifier :
`gModSlots[0..3]` sont un pool général alloué dynamiquement par cette UI,
`gModSlots[4]` est réservé au LFO du synthé modulaire (voir plus bas).
La cible `MODDEST_FX_PARAM` applique la modulation en écrivant temporairement dans
`fxList[fx].params[param]`, en appelant `applyFxEffect(fx)`, puis en restaurant la valeur
« centre » réglée par l'utilisateur — aucune modification n'était nécessaire dans
`applyFxEffect()` lui-même.

---

## MODE_MODULAR — Synthé wavetable double-oscillateur

Refonte complète (inspirée de Serum) de l'ancien MODE_MODULAR (qui était un simple synthé
mono-oscillateur avec vibrato). Utilise pour la première fois dans l'app le type d'onde
`WAVETABLE` d'AMY (`render_wavetable()` dans `oscillators.c`), inactif jusqu'ici car le flag
de build `AMY_WAVETABLE` n'était défini nulle part (`platformio.ini`, `simulator/CMakeLists.txt`,
`android-native/.../CMakeLists.txt` — les trois l'ont maintenant). AMY embarque déjà 5
wavetables 64×256 prêtes à l'emploi (`pcm_samples_tiny.h`) : `111`, `BRAIDS01`, `PPG_WA00`,
`SINE2SAW`, `VIRAL` — aucune donnée à créer.

- **2 oscillateurs** (canaux dédiés `MOD3_OSCA_CH`/`MOD3_OSCB_CH`, pas `SYNTH_CH`), table
  d'onde partagée (P2), position de morphing indépendante par oscillateur (P4/P5) —
  `e.duty_coefs[COEF_CONST]` pilote un cross-fade continu **natif AMY** entre les 64
  formes d'onde d'une table, aucun cross-fade côté app nécessaire. Oscillateur B avec un
  détune fixe léger (`MOD_OSCB_DETUNE_SEMIS`) pour l'épaisseur.
- **Filtre partagé** (P6) via `audioModularSetFilter()` — setter dédié car
  `audioSetFilter`/`audioSetAllFiltersT` ciblent `SYNTH_CH` en dur.
- **1 LFO** (P7, profondeur) qui fait onduler la position de morphing de l'oscillateur B
  autour de sa valeur de base — branché sur `gModSlots[4]` (voir section FX ci-dessus),
  cible `MODDEST_MOD_WTPOS_B`. C'est un MVP volontairement simplifié : l'enum
  `ModDestKind` prévoit d'autres cibles (pitch A/B, position A, ampli) pour une matrice de
  modulation plus complète plus tard, mais seule WTPOS_B est câblée pour l'instant.
- B1 ouvre désormais la grille FX partagée (`OVERLAY_FX`, absente de ce mode auparavant) ;
  B4 = octave (bug corrigé : `btn==4` ne pouvait jamais se déclencher, même classe de bug
  que celui déjà corrigé sur STONE/MOD2 — `handleButton()` ne produit jamais `btn>3`).

**Piège découvert et corrigé** : changer de table d'onde en cours de note appelait la
fonction de configuration complète (`audioModularOscInit`, qui touche aussi num_voices/
oscs_per_voice/enveloppe) — cela réinitialisait le canal et tuait silencieusement la note en
train de jouer (même classe de bug que `patches_load_patch()`/`reset_osc()` sur un shape
switch classique). Fix : `audioModularSetTable()`, une variante allégée qui ne touche que
`e.preset`, réservée au changement de table en direct (le contrôle P2).

---

## Modes expérimentaux (EXP / EXP2 / EXP3 / LIFE / SWARM)

Philosophie commune : jouer des sons intéressants sans connaissance de théorie musicale
(contrairement à SYNTH/303S/etc., plus musiciens). Tous utilisent la grille de touches
jouables complète (`KBD_NOTE_ROWS × KBD_COLS` = 4×8 = 32 cases) et un jeu de tables
d'échelle partagé (`kExp2Scale{MAJ,MIN,PNT,CHR}[]`, 8 degrés — un par colonne).

| Mode | Concept | Colonne | Ligne |
|------|---------|---------|-------|
| EXP | Thérémine : le joystick pilote une position XY continue | — | — |
| EXP2 | PolyBounce : jusqu'à 4 balles physiques rebondissent dans un polygone tournant | — | — |
| EXP3 | Orbital : 8 balles en orbite à vitesse liée au BPM, franchir une zone déclenche une note | balle | rayon d'orbite (0-3) |
| **LIFE** | Jeu de la vie de Conway, topologie torique (les bords se rebouclent) | hauteur (degré d'échelle) | octave |
| **SWARM** | Boids (cohésion/séparation/alignement), zone d'attraction pilotée au joystick | boid (spawn) | — |

**LIFE** : seules les transitions naissance/mort (pas chaque cellule vivante à chaque tick)
déclenchent note-on/off, et une seule voix sonne par colonne à la fois — plafonne
naturellement la polyphonie à 8 (= `NUM_SYNTH_VOICES`), donc pas besoin d'une plage
d'oscillateurs dédiée contrairement à STONE/SAMPLE/GRANULAR. Vitesse de tick, variante de
règle (classique B3/S23 ou HighLife B36/S23) et réensemencement de densité sont pilotés
par pots.

**SWARM** : réutilise explicitement la structure de balle, la géométrie d'arène tournante
et le clamp de vitesse d'EXP2 (voir son tick physique dans `main.cpp`) — seul le noyau de
règles de flocking (remplace la collision élastique d'EXP2) et le déclenchement par zone
d'attraction sont du code neuf.

Ces trois derniers (EXP2/EXP3-like) ont chacun leur propre masque de FX (`expFxMask` etc.),
indépendant de `fxList[]`/`OVERLAY_FX` — délibéré : ces modes ne peuvent pas ouvrir
`OVERLAY_FX` aujourd'hui (pas de branchement B1), donc aucun conflit réel ; leur grille de
touches sert déjà d'UI FX compacte, bien adaptée à un mode « viewport physique » où un
overlay séparé interromprait le jeu.

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
