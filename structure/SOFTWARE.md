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

## Modes (37 implémentés)

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
| VID | `MODE_VID` | Lecteur vidéo/MEDIA : fichiers `.bvid` 128×128 1bpp, `.jpg`/`.png`/**`.gif`** (auto-convertis), `.wav`/`.mp3` (aperçu audio) depuis la carte SD |
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
| LIFE | `MODE_LIFE` | Jeu de la vie de Conway sur la grille de touches (colonne=hauteur, naissance=note) |
| SWARM | `MODE_SWARM` | Essaim de boids (cohésion/séparation/alignement), zone d'attraction pilotée au joystick |
| GEN | `MODE_GEN` | Génératif : joystick X=texture procédurale, Y=méthode sonore, indépendamment réglables |
| DJ | `MODE_DJ` | Platine DJ/remix : un gros sample SD (mp3/wav), scrub/vitesse/reverse + FX partagés |
| GROOVE | `MODE_GROOVE` | Séquenceur unifié drums+synth+303 : la grille 4×8 entière = le pattern de la piste focus (voir section dédiée plus bas) |
| **EUCLI** | `MODE_EUCLI` | **Nouveau** — Séquenceur euclidien 4 pistes : touche = nombre de pas (palette 4-32), P4-P7 = nombre de coups, affichage en anneaux concentriques (voir section dédiée plus bas) |
| **CRUNCH** | `MODE_CRUNCH` | **Nouveau** — Tracker 4 pistes façon MothOS, enregistrement live (pas d'édition pas-à-pas) : touches = 4 shifts + 12 notes/commandes, 12 slots instrument (2 banks + 10 mélodiques) (voir section dédiée plus bas) |

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
| 214–217 | `CRUNCH_OSC_BASE + 0..3` | **Nouveau** — CRUNCH : 4 pistes, un oscillateur fixe et monophonique par piste (pas de round-robin — une nouvelle note sur une piste déjà sonore retrigger simplement l'oscillateur de cette piste) |
| 240–245 | `STONE_OSC_BASE + 0..5` | STONE : 6 voix round-robin sur un seul sample |

`MODE_MODULAR` (canaux 14-15) n'appartient PAS à `SYNTH_CH` — c'est le seul autre mode
(avec STONE/GRANULAR2/DRUM2/SAMPLE/CRUNCH) à utiliser des canaux/oscillateurs dédiés plutôt
que l'allocateur de voix dynamique d'AMY. Piège classique : `audioSetAllFilters()` /
`audioSetAllFiltersT()` (utilisées par le FX FILT partagé) sont câblées par défaut pour
`SYNTH_CH` — tout nouveau canal dédié doit explicitement y être ajouté (voir
`audioApplyFilterToStone()` / `audioApplyFilterToModular()` / `audioApplyFilterToCrunch()`
dans `audio_engine.cpp` comme
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

15 effets configurables, stockés dans `fxList[]` (`main.cpp`), accessibles via une grille
4×4 dans l'overlay FX (B1 court sur SYNTH/POKEMON/I303/STONE/OMNI/MODULAR/GRANULAR2/DR2/GEST)
— la grille a 16 cellules mais `FX_COUNT=15`, donc la dernière case reste vide :

| Index | Nom | Implémentation |
|-------|-----|-----------------|
| 0 | FILT | `audioSetAllFiltersT` (LPF/HPF/BPF) ou `audioSetLadderFilter` (type LADDER, bus 0, saturation tanh) |
| 1 | DISTORT | `audioSetDistortion` |
| 2 | REVERB | `audioSetReverb` (bus 0) |
| 3 | CHORUS | `audioSetChorus` (bus 0) |
| 4 | FLANGER | `config_chorus` (délai court = comb filter) |
| 5 | DELAY | `audioSetDelay`, synchronisé au BPM |
| 6 | EQ | `audioSetEq` (3 bandes) |
| 7 | RESECHO | Écho synchronisé BPM avec filtre tonal dans la boucle de feedback |
| 8 | REP | Wavefold global (`audioSetWavefold`) |
| 9 | BITCRS | Wavefold à gain extrême (simule une réduction de bit-depth) + LPF optionnel |
| 10 | TREMOLO | Modulation du volume, tick 10ms |
| 11 | AUTOPAN | Modulation du pan, tick 10ms |
| 12 | OVERDRIVE | `audioSetOverdrive` |
| 13 | RINGMOD | Modulation en anneau (porteuse sinus, bus 0) |
| 14 | COMPRESSOR | Compresseur feedforward à enveloppe de crête (bus 0) |

L'ancien FX dédié **LFO** (qui modulait uniquement le cutoff de FILT) a été retiré : son
« réglage spécial » ne produisait pas d'effet audible fiable, et il faisait doublon avec le
moteur d'automatisation générique ci-dessous, qui peut moduler le cutoff de FILT (ou
n'importe quel autre paramètre de n'importe quel FX) de la même façon mais en mieux.

Les FX bus-0 (REVERB/CHORUS/DELAY/EQ/RESECHO/REP/BITCRS/RINGMOD/COMPRESSOR/FILT-LADDER)
s'appliquent après le mix, donc à **toute** source sonore (synthé, sample, granulaire…).
FILT (types LPF/HPF/BPF) et DISTORT/OVERDRIVE en revanche ciblent des canaux/oscillateurs
AMY précis — voir l'avertissement dans la table des canaux ci-dessus : historiquement,
seul `SYNTH_CH` (+ GRANULAR2 via un masque d'oscillateurs actifs) était couvert ; STONE,
MODULAR puis CRUNCH ont été ajoutés ensuite (`audioApplyFilterToStone`/
`audioApplyFilterToModular`/`audioApplyFilterToCrunch`).
La 303S (bus 1) est mergée dans le bus 0 **avant** le traitement FX dans `amy.c`, donc les
effets bus-0 s'appliquent au signal combiné sans allocation mémoire supplémentaire.

### FILT — plages de résonance et filtre natif des patches

Le pot Res (P5 dans la grille FX) est un seul paramètre brut (`e.params[1]`, plage
0.5–3.0) reprojeté séparément pour chaque type de filtre dans `applyFxEffect()` case 0
(`resT` = position 0..1 sur cette plage, indépendante des deux mappings ci-dessous) :

- **LPF/HPF/BPF (natif AMY)** : Q reprojeté sur 0.7 (neutre, valeur par défaut d'AMY
  elle-même — donc « résonance à zéro » reste plat) à 9.7 (pic net, ~20dB). `Q=3.0` (l'ancien
  plafond direct, sans reprojection) était perçu comme trop faible — un vrai filtre
  « screamer » tourne plutôt autour de Q=8-15. `dsps_biquad_gen_lpf_f32` (`filters.c`) ne
  plafonne que le bas (Q≥0.51), stable à n'importe quel Q fini.
- **LADDER** : résonance brute reprojetée sur 2.25–18 (au lieu de `res*4.5` = 2.25–13.5) —
  la borne haute dépasse maintenant volontairement le seuil (~15) où le soft-clip de
  sécurité commence à compresser, pour laisser un peu de grain « intéressant » en bout de
  course une fois le sifflement aigu ci-dessous corrigé.

Pour les patches Juno/DX7 (`s_synthChIsPatch`), cutoff **et** résonance sont reprojetés en
continu vers le filtre natif du patch (`remapCutoffResToNative()`, `audio_engine.cpp`,
utilisé par `audioSetAllFiltersT`/`audioSetAllFilters`/`audioSetFilterFreq`) plutôt que
bornés (`fminf`) : un simple `min(cutoff, nativeCC)` transformait tout le haut du pot
(cutoff ≥ nativeCC, souvent la moitié+ de la course pour la plupart des patches, ex.
~994Hz pour J:PNO sur une plage 65-18000Hz) en un plateau plat identique — plus aucune
différence audible entre par ex. 1000Hz et 18000Hz de cutoff, alors que le bas du pot
restait, lui, pleinement fonctionnel. `remapCutoffResToNative()` réutilise plutôt la même
courbe exponentielle que le pot lui-même pour reprojeter tout `[65, 18000]` sur
`[65, nativeCC]`, en continu — toute la course du potard redevient utile, tout en gardant
exactement les mêmes points fixes qu'avant à chaque extrémité (fermé = plein contrôle FX,
grand ouvert = exactement le son natif du patch, résonance native comprise — sinon activer
le FX avec sa propre résonance réglée change la texture même à cutoff « transparent »).

### LADDER — sifflement dans les aigus (fc2 taper)

Le pic résonant de LADDER vient de la différence entre deux cascades passe-bas (une
normale, une réglée 1.6× plus brillante) — voir le commentaire "History" dans le bloc bus-0
de `amy.c`. À cutoff élevé, l'ancienne implémentation plafonnait dur la cascade brillante
près de Nyquist (`fc2 = min(cutoff*1.6, fs*0.45)`), ce qui la rendait quasi transparente :
la différence utilisée pour le pic devenait alors presque tout le contenu aigu brut du
signal d'entrée, non filtré — entendu comme un grésillement large bande plutôt qu'un pic
résonant propre. Fix : le ratio 1.6× lui-même se réduit progressivement vers 1.0× à mesure
que le cutoff approche Nyquist (`headroom = (nyquist-cutoff)/nyquist`), gardant les deux
cascades proches l'une de l'autre en permanence — le pic reste étroit sur toute la plage.
Comportement inchangé aux cutoffs bas/moyens (la majorité de la plage pratique).

### Automatisation des FX (appui long) — `OVERLAY_FX_MOD`

Un **appui long** (≥`FX_LONGPRESS_MS`=500ms, `s_fxPressOpt`/`s_fxLongPressFired` dans
`main.cpp`) sur un slot FX dans la grille FX ouvre un éditeur dédié (`OVERLAY_FX_MOD`)
plutôt que de togglé l'effet — un appui **court** garde le comportement normal
(active/désactive). Appuyer long sur un FX **inactif** l'active d'abord
(`overlayFxToggle()`) puis entre directement dans l'éditeur, en un seul geste. Remplace
l'ancien mécanisme au double-clic (retiré : sujet à une race condition sur le second clic,
et moins découvrable qu'un appui long standard).

Dans l'éditeur : joystick X change quel paramètre du FX est automatisé. Pots :
- **P4** = profondeur (0 = pas de slot alloué tant qu'on ne monte pas au-dessus d'un seuil)
- **P5** = **vitesse, un seul potard continu** couvrant tout le spectre : subdivisions
  synchronisées au BPM (`kDelaySubdiv[]`, lentes → rapides) dans la moitié basse, Hz libre
  (0.5–20Hz) dans la moitié haute — voir le bloc de dispatch pots dans `main.cpp` pour le
  détail du remapping. Avant cette unification, P5 (Hz libre seul) et P7 (toggle sync +
  subdivision, actif seulement au-dessus de 50%) étaient deux potards séparés ; la moitié
  basse de P7 ne faisait alors *rien* une fois en mode Hz libre — perçu comme « le potard
  reste bloqué ». P7 est désormais inutilisé dans cet overlay.
- **P6** = forme d'onde (sine/tri/carré/sample&hold)

Repose sur un moteur de modulation générique (`gModSlots[]`, struct `ModSlot`, `main.cpp`)
qui généralise le pattern déjà utilisé par les FX TREMOLO/AUTOPAN (accumulateur de
phase + porte de profondeur + restauration propre à la désactivation), sans les modifier :
`gModSlots[0..3]` sont un pool général alloué dynamiquement par cette UI,
`gModSlots[4]` est réservé au LFO du synthé modulaire (voir plus bas).
La cible `MODDEST_FX_PARAM` applique la modulation en écrivant temporairement dans
`fxList[fx].params[param]`, en appelant `applyFxEffect(fx, /*silent=*/true)` (le tick
10ms de cette automatisation appelle `applyFxEffect()` en continu ; le flag `silent`
évite de spammer le terminal à chaque tick — les appels normaux, hors automatisation,
gardent leur log), puis en restaurant la valeur « centre » réglée par l'utilisateur.

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

## MODE_GROOVE — Séquenceur unifié drums/synth/303

Nouveau mode, additif (ne remplace aucun séquenceur existant). Répond au constat que DR2
("GEST2"), bien qu'il combine déjà 8 pads batterie + une piste synthé + une piste 303 sur
une horloge partagée, expose ses 64 pas via une hiérarchie à 3 niveaux (`beat.step.micro`,
4.4.4) browsée par 3 rangées de 4 touches empilées — peu lisible. GROOVE répond en affichant
**le pattern entier d'une seule piste sur toute la grille 4×8** (jusqu'à 32 pas), "ce que tu
vois est ce que tu presses".

- **10 pistes** (`GRV_TRK_DRUM0..7`, `GRV_TRK_SYNTH`, `GRV_TRK_303`), focus cyclé au
  joystick X (armé/désarmé par zone morte — le stick étant à rappel automatique, un mapping
  proportionnel ferait revenir le focus au centre à chaque relâchement). Réutilise
  directement les moteurs sonores existants — `playDrum()` (pads DRUM2), `audioNoteOn/Off`
  (synthé), `audioT303*` (303, monophonique — pas `audioI303*` qui est polyphonique) — aucun
  nouveau moteur de synthèse.
- **Stockage par pas** compact, même forme que DR2/SYSEQ/303S/SS2 : `grvOn[10][32]` (0=off,
  sinon vélocité ou note+1) et `grvAlt[10][32]` (0=normal ; pistes batterie : 1=50%
  probabilité/2=ratchet, repris de `dr2Mod` ; pistes synthé/303 : 1=accent/2=slide, repris
  de `s303Alt`). ~640 octets/pattern, 4 slots de banque — négligeable.
- **Longueur de pattern indépendante par piste** (`grvLen[10]`, 1-32, défaut 16) — chaque
  piste avance sur la **même horloge maître** mais boucle à sa propre longueur, ce qui
  permet un vrai polymètre (ex: batterie sur 8 pas contre 303 sur 13 pas qui déphase au fil
  des mesures) — vérifié en test headless (voir historique de session).
- **Horloge propre**, volontairement indépendante de `drum2Step` (celle partagée par
  DRUM2/SYSEQ/303S/SS2/GEST, qui tourne sans condition sur `currentMode` dans `loop()`) —
  la réutiliser aurait fait entrer en collision audible le pattern de GROOVE avec celui
  d'un DRUM2/SYSEQ/etc. resté en lecture en arrière-plan. Cadence 16ᵉ de note
  (`60000/bpm/4`, comme DRUM2), pas la subdivision 64ᵉ de DR2.
- **Édition "stamp"** (aucune nouvelle UI de type overlay) : un pot/joystick fixe la valeur
  courante à poser (accent/slide/probabilité + note pour les pistes mélodiques), une simple
  pression pose ou efface un pas — identique au principe déjà utilisé par DR2/303S/SYSEQ/SS2.
- **Boutons** — convention séquenceur (pas convention synthé) : **B1** Play/Stop, **B2**
  FX (double-clic = Scale/Arp), **B3** mute piste focus (double-clic = solo — Korg Electribe
  a normalisé cette paire mute/solo), **B4** cycle banque de patterns (double-clic =
  copier/coller, identique à DR2). Le mute/solo comblait un vrai manque : aucun autre mode
  du projet n'a de mute/solo réel (DR2 le documente explicitement en commentaire).
- **Swing persistant** (P2) — sauvegardé avec le pattern, contrairement au swing transitoire
  piloté par le joystick X de DRUM2/SYSEQ (perdu au changement de mode).

---

## MODE_EUCLI — Séquenceur euclidien 4 pistes

Nouveau mode, catégorie SEQNC. Chaque piste (une par ligne de la grille clavier) a son
propre nombre de pas (palette fixe `{4,6,8,12,16,20,26,32}`, une valeur par colonne) et son
propre nombre de coups (P4-P7, un potard par piste), répartis par un accumulateur à la
Bjorklund (`eucliRebuildPattern()`, généralisé depuis la version câblée en dur à 8 pas de
MODE_GEN). Chaque piste tourne sur sa **propre tête de lecture** (`euPlayhead[4]`), pas une
position partagée — un vrai polymètre : deux pistes à pas différents dérivent l'une par
rapport à l'autre au fil des mesures au lieu de rester synchronisées.

- **Horloge dédiée** (`eucliTick()`, extraite en fonction autonome plutôt qu'inline dans
  `loop()` — nécessaire pour pouvoir la piloter manuellement en test headless, `loop()` ne
  tournant jamais pendant `setup()`), pas la pulsation `drum2Step` partagée par
  DRUM2/SYSEQ/303S/SS2/GEST — même raison que GROOVE : éviter une collision audible avec un
  pattern resté en lecture dans un autre mode.
- **Touches = palette, pas placement de pas** : contrairement à tous les autres modes
  d'édition de grille (DRUM2/GROOVE/SS2…), une touche ne pose/efface pas un pas — elle
  choisit d'un coup le nombre de pas de sa ligne (`kEucliStepOptions[col]`). La colonne 0
  physique (la plus à gauche — voir la note sur la convention colonne ci-dessous) donne 4
  pas, la dernière colonne 32.
- **Banques de sons** (B3, `kEucliBanks[4][4]`) : 4 kits curés parmi les 32 pads
  `kDrumPads[]` partagés (voir `audioDrum2Hit()`), un par ligne. Le nom du pad affecté à
  chaque piste est affiché en toutes lettres à côté de sa barre (`audioDrumPadLabel()`) —
  sans ça, changer de banque ne produisait qu'un numéro opaque à l'écran, pas de quoi
  comprendre ce que B3 venait de changer (retour utilisateur direct).
- **Affichage** : moitié haute = 4 anneaux concentriques (modèle : le dessin orbital
  d'EXP3, `drawCircle` par rayon + marqueurs placés à la trigonométrie), moitié basse = une
  barre + lecture `X/S` + le nom du pad par piste.
- **Boutons** : B1 Play/Stop, B2 FX (`OVERLAY_FX` standard), B3 cycle banque, B4 reset
  toutes les têtes de lecture à 0 (sans toucher aux pas/pulses).

Deux inversions gauche/droite et haut/bas ont dû être corrigées après coup (rapportées par
l'utilisateur, pas visibles en lisant le code seul) — voir « Bugs corrigés » plus bas :

## MODE_CRUNCH — Tracker 4 pistes façon MothOS

Nouveau mode, catégorie SEQNC (pas INSTR — un tracker/enregistreur de pattern, pas un
instrument joué). Port du modèle de contrôle de
[MothOS](https://github.com/MothSynths/MothOS) (firmware du synthé MothSynth), pas une
création originale : l'utilisateur avait déjà lui-même adapté MothOS à un clavier 4×8 très
proche de celui de GrvEP dans son propre fork
`/home/cchupin/projects/MothOS_GroovePadBox`, qui a servi de référence directe pour le
portage — voir `Voice.cpp`/`Tracker.cpp`/`InputManager.cpp`/`ScreenManager.cpp` de ce fork
pour la source originale de tout ce qui suit.

### Enregistrement live, pas édition pas-à-pas

Contrairement à tous les autres séquenceurs du projet (DRUM2/GROOVE/SYSEQ/SS2/EUCLI…),
CRUNCH n'a **aucune** grille d'édition de pas. Le modèle MothOS d'origine
(`Tracker::SetNote()`) : tant que le transport tourne (`crunchPlaying`), tout ce qui est
joué au clavier est capturé en direct dans le pattern de la piste sélectionnée, à la
position courante de la tête de lecture — « jouer, c'est enregistrer », sans étape d'armement
séparée. `crunchTick()` (autonome, même raison qu'`eucliTick()` — testable en headless) ne
fait que la **lecture** ; la capture se fait entièrement dans `handleNoteKeyAudio()`.

### Le clavier remplace le clavier 4×4 + 4 touches shift de MothOS

Le firmware MothOS original tourne sur un clavier matriciel 4×4 (16 touches : 4 touches
« shift » collantes M/N/O/P + 12 touches note/commande A-L). Le fork de l'utilisateur avait
déjà résolu l'adaptation à un clavier 4×8 physique (colonnes dupliquées par moitié,
`col % KEY_COLS`) — repris ici à l'identique plutôt que d'inventer un schéma GrvEP maison :
ligne physique du haut (row==3) = les 4 touches shift, les 3 lignes du dessous = les 12
touches note/commande (A-D/E-H/I-L). Une touche shift arme un mode « collant » (montré à
l'écran par un overlay légende façon `UpdateInstructionsScreen`) ; la touche suivante
(n'importe laquelle, y compris une autre touche shift) consomme l'armement et exécute la
commande — `crunchDispatchCommand()` (dans `main.cpp`) est un portage quasi-littéral de
`Tracker::SetCommand()` : sélection d'instrument, octave, sélection/mute/solo/volume de
piste, forme d'enveloppe, longueur de note, effacer piste/pattern, sélection de pattern,
copier/coller, mode chanson, presets BPM, play/stop.

**Convention colonne** : dans ce codebase, la colonne 0 est la touche la plus à **droite**
(col7 la plus à gauche — voir le commentaire d'OMNI). Un `col % 4` brut ferait donc
*descendre* la colonne logique de gauche à droite sur le clavier alors que la légende à
l'écran la dessine en *montant* — d'où `3 - (col % 4)`, pas `col % 4` tout court (bug
rapporté par l'utilisateur, voir « Bugs corrigés »).

### 12 slots instrument, pas 12 pitches d'un seul sample

Piège du premier portage (corrigé après coup, signalé par l'utilisateur) : l'original
distingue clairement, via `Voice::ReadDrumWaveform`/`ReadSfxWaveform` vs `ReadWaveform`,
deux familles d'instruments sous la commande `'I'` (0-11) — pas 12 variations d'un seul
timbre :

- **Slot 0 « DRUM »** et **slot 1 « SFX »** : chacune des 12 touches note joue un
  échantillon **différent** (un kit de percussions / un kit de bruitages), toujours à sa
  hauteur native — aucun pitch-shift.
- **Slots 2-11** (10 instruments mélodiques) : un seul échantillon fixe par slot, dont les
  12 touches note pilotent la hauteur chromatique (`60 + octave*12 + val`).

`audioCrunchResolveSampleIndex()`/`audioCrunchSlotIsBank()`/`audioCrunchSlotIsNative()`/
`audioCrunchSlotName()` (`audio_engine.cpp`) sont la source de vérité unique pour cette
résolution — utilisés à la fois par la lecture audio et par l'UI (légende, header, browser
B4), pour qu'ils ne puissent pas diverger.

Les slots 0-6 (banks + 5 des 10 mélodiques) réutilisent purement et simplement le buffer
PSRAM des 32 pads batterie déjà chargés (`pcm_get_sample_ram_for_preset`, zéro donnée
neuve — même technique que le premier portage). Les slots 7-11 (5 instruments mélodiques
supplémentaires) sont en revanche du **contenu audio réellement nouveau** : les timbres
`instrumentN` originaux de MothOS n'ont pas d'équivalent dans la banque `kDrumPads[]`
existante — 5 des 11 fichiers du fork utilisateur (les plus petits, ~194 Ko une fois
reconditionnés `int32`→`int16_t`) ont été vendorisés dans `src/sounds/crunchinstr1-5.h` et
chargés via un nouveau `pcm_load()` dédié (`audioLoadCrunchNativeInstruments()`) — pas
aliasés, cette donnée n'existe nulle part ailleurs dans GrvEP.

### Ce qui n'a PAS été porté

Les effets par voix de MothOS (echo/arp-chord/whoosh/pitchbend, lowpass/retrig/wobble,
overdrive) n'ont aucun équivalent DSP dans GrvEP — plutôt que d'improviser un mapping vers
des effets sans rapport, les commandes clavier correspondantes (`'D'`/`'A'`) sont
reconnues (la légende les affiche, l'armement se consomme normalement) mais restent des
no-op sonores. Le vrai façonnage du son de CRUNCH passe par le bus FX partagé de GrvEP
(B2 + `OVERLAY_FX`, P4-P7 quand un FX est actif) — convention standard, pas un pont vers les
commandes clavier no-op ci-dessus.

### Contrôles

| Contrôle | Rôle |
|----------|------|
| Clavier | 4 shifts (M/N/O/P, ligne du haut) + 12 notes/commandes (A-L) — voir ci-dessus |
| Joystick X | Cycle la piste sélectionnée (même idiome que GROOVE) |
| Clic joystick | Mute/unmute la piste sélectionnée |
| P2 | Octave de la piste sélectionnée (continu, -2..+2) |
| P4 | Volume/vélocité de la piste sélectionnée (hors FX actif) |
| P5 | Longueur de relâchement de la piste sélectionnée (hors FX actif) |
| P4-P7 | Paramètres du FX actif (`OVERLAY_FX` ouvert) — convention standard |
| B1 | Play/Stop |
| B2 | `OVERLAY_FX` |
| B4 | Browser des 12 slots instrument (`OVERLAY_CRUNCH`) pour la piste sélectionnée |

### Écran

Porté directement du propre fork `MothOS_GroovePadBox` de l'utilisateur
(`ScreenManager::UpdateMainScreen`, déjà adapté là-bas pour un panneau 128×128) : colonne
gauche = contexte (instrument/piste/octave/statut), colonne droite = gros compteur de pas +
numéro de pattern + BPM + marqueur de battement clignotant, bas = une ligne par piste avec
VU-mètre à décroissance de crête. Overlay légende (4×4) affiché à la place tant qu'un shift
est armé.

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

## MODE_VID / MEDIA — lecture de `.gif` animés

En plus des `.bvid` natifs et des stills `.jpg`/`.png` (déjà auto-convertis via
`loadOrConvertBvid()`), le navigateur de fichiers de MEDIA (`isMediaFile()`) accepte
maintenant les `.gif` animés (`isGifFile()`), converties à la volée en `.bvid` multi-frame
au premier accès et mises en cache à côté de la source (`<nom>.gif.anim.bvid` +
`.anim.bvid.meta`, validité basée sur la taille du fichier source — même schéma que le
cache mono-frame des stills). Une fois convertie, la lecture réutilise tel quel le
mécanisme de streaming `.bvid` existant (`vidFile`/`vidPlaying`) — aucun changement côté
lecture, seulement à l'ouverture.

Le décodage GIF passe par `imgDecodeGifFrames()` (nouvelle fonction dans
`{simulator/hal,include}/jpegdec.h`), qui s'appuie sur le décodeur GIF déjà vendorisé dans
stb_image (`stbi_load_gif_from_memory`, jamais compilé avant faute d'appelant) — donc
aucune nouvelle dépendance. **Limitation actuelle : simulateur/Android seulement.** Sur
ESP32, `imgDecodeGifFrames()` est un stub qui log et pointe vers `tools/to_bvid.py` (voir
son commentaire dans `include/jpegdec.h`) — écrire un décodeur GIF (LZW + multi-frame +
palette) dans le même style *streaming* que le décodeur PNG maison de ce fichier serait un
chantier bien plus gros que PNG (mono-frame) et n'a pas été tenté sans matériel réel pour
le valider.

`loadOrConvertBvid()` (utilisé par le picker d'animations de DRUM2, une seule frame par
slot) accepte aussi les `.gif` maintenant, mais n'en garde que la **première frame** — son
cache (`<nom>.bvid`) est délibérément un fichier séparé de celui de MEDIA
(`<nom>.gif.anim.bvid`) : les deux consommateurs veulent un nombre de frames différent
depuis la même source, partager un seul fichier de cache aurait fait que l'un écrase
systématiquement la conversion de l'autre à la prochaine ouverture.

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

### FILT bypass au cutoff max → « s'active » aléatoirement
`audioSetAllFiltersT()` etc. traitaient `cutoff >= 18000.0f` (exact) comme « FX transparent,
ne pas appliquer de filtre du tout ». Le mapping exponentiel du pot Cut (`mn*powf(mx/mn,
potValue)`) ne retombe pas forcément pile sur 18000.0f même à `potValue==1.0` exact — de
simples arrondis flottants dans `powf` suffisent à faire basculer le résultat de quelques
Hz d'un côté ou de l'autre du seuil, donc le filtre passait de façon imprévisible entre
« complètement absent » (`FILTER_NONE`) et « réellement engagé » à cutoff quasi-max —
entendu comme une activation aléatoire, surtout audible avec la résonance également au
maximum. Fix définitif : le bypass ne signifie plus QUE « FX vraiment désactivé » (les
appelants passent `0.0f` dans ce cas — voir `applyFxEffect` case 0) ; un cutoff élevé mais
réel garde le filtre engagé en continu, sans bascule de topologie. Voir aussi la section
FILT ci-dessus (`remapCutoffResToNative`) pour la suite de cette même investigation : même
architecture propre, encore fallait-il que le filtre du patch, à cutoff élevé, converge
en douceur vers le natif plutôt que de plafonner brutalement.

### Seuils exacts / plafonds plats sur une plage de potard → zone morte
Deux bugs distincts, même famille : un test d'égalité/seuil exact près du bord d'une plage
de paramètre (`cutoff >= 18000.0f` ci-dessus) est fragile aux arrondis flottants ; un
plafonnement plat (`fminf(cutoff, nativeCC)`) au-delà d'un seuil peut geler toute une
portion de la course d'un potard sur une valeur identique (zone morte — la moitié+ du pot
Cut ne changeait plus rien de perceptible pour la plupart des patches). Un potard séparé
qui ne fait *rien* d'observable sur une partie de sa course (l'ancien P7 de l'automatisation
FX en dessous de 50%, avant sa fusion avec P5 — voir la section automatisation ci-dessus)
tombe dans la même famille perceptible côté utilisateur : « le potard reste bloqué ».
Pattern de fix qui s'est répété trois fois cette session : remplacer le seuil/plafond dur
par soit une marge/hystérésis, soit — mieux — un remapping continu qui garde toute la
course du potard utile.

### Volume incohérent entre pads `kDrumPads[]` (CLAP/HIHAT2/RIDE plus faibles)
`audioLoadDrumSamples()` charge deux formats source : `int8_t` (mis à l'échelle ×256 au
chargement) et « 16-bit dans un conteneur `int32` » (copié tel quel). Les deux formats
n'avaient jamais été exportés au même niveau — le ×256 des échantillons `int8_t` tombe par
hasard près de la pleine échelle, mais les échantillons « 16-bit en conteneur » jouaient au
niveau arbitraire produit par leur propre outil de conversion, parfois nettement plus bas
(CLAP, HIHAT2, RIDE…). Rapporté via le mode EUCLI (4 pistes jouées côte à côte rendent
l'écart flagrant), mais le bug touche tout consommateur de `kDrumPads[]` : DRUM2, GROOVE,
SYSEQ, et les banks DRUM/SFX de CRUNCH (qui aliasent les mêmes buffers). Fix : normalisation
de crête à deux passes (mesurer le pic, puis reprojeter vers une cible commune ~30000) pour
les deux formats, remplaçant le `×256` en dur.

### Menu : curseur atteignant des cases vides sur une catégorie non multiple de `MENU_COLS`
La navigation joystick de la grille menu limitait le déplacement colonne à `< MENU_COLS`
(largeur fixe de la grille), pas à la largeur réellement peuplée de la **ligne courante** —
sur une catégorie dont le nombre d'items n'est pas un multiple de 3 (SEQNC : 9 items après
l'ajout de CRUNCH, AUTRE : 11), la dernière ligne est incomplète et le curseur pouvait
s'y déplacer sur une case vide (rien n'y est dessiné — le curseur « disparaît »
visuellement). Fix : la largeur de colonne autorisée se recalcule à chaque déplacement de
ligne à partir du nombre réel d'items restants sur la ligne courante.

### EUCLI : lignes clavier inversées haut/bas par rapport à l'affichage
`euSteps[row]` indexait directement par la ligne clavier brute, alors que l'affichage
dessine la piste 0 comme l'anneau le plus intérieur / la barre la plus haute — sur ce
clavier, la ligne physique du haut est `row==3`, pas `row==0` (confirmé indépendamment par
le commentaire de câblage du fork `MothOS_GroovePadBox` de l'utilisateur : « physical row 3
is top »). Fix : `lane = EUCLI_LANES-1-row` pour que la ligne physique du haut pilote la
piste dessinée en haut.

### CRUNCH : colonnes clavier en miroir par rapport à la légende à l'écran
Même famille que le bug EUCLI ci-dessus mais sur l'axe colonne : `col % 4` (au lieu de
`3-(col%4)`) faisait *descendre* la colonne logique de gauche à droite sur le clavier
(rappel : col0 = touche la plus à **droite** dans ce codebase, pas la gauche), alors que la
légende à l'écran dessine ses 4 colonnes en *montant* de gauche à droite — la touche la
plus à gauche du clavier ne correspondait pas à la cellule la plus à gauche de la légende.
