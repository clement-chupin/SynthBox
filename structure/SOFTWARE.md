# GrvEP / SynthBox — Software Architecture

> **Lecture recommandée** : `/CLAUDE.md` (racine) puis `structure/HANDOFF.md` (état du projet, pièges, travail en attente) avant ce fichier. Celui-ci est la référence *par sous-système* ; HANDOFF.md est le point d'entrée pour reprendre le projet.

---

## Build

```bash
pio run -e esp32-s3-devkitc-1               # ESP32-S3 (compile seulement)
pio run -e esp32-s3-devkitc-1 -t upload     # compile + flash
cd simulator && ./build.sh                   # simulateur Linux (SDL2), binaire : simulator/build/grvep_sim
```

Plateforme : ESP32-S3, framework Arduino, PlatformIO.
**Il n'existe qu'UNE copie d'AMY** : `lib/AMY Synthesizer/src/`. PlatformIO la trouve seul via `lib/`
(aucune entrée AMY dans `lib_deps`) ; le simulateur/Android/Web compilent les mêmes fichiers
(`simulator/CMakeLists.txt`, `simulator/hal/AMY-Arduino.h`). Toute modification d'AMY se fait là, une fois.
(Ancien montage à deux copies — `../other_projects/amy` — supprimé ; voir CLAUDE.md pour l'historique.)

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

lib/AMY Synthesizer/src/  Moteur de synthèse AMY (copie unique, partagée par toutes les cibles) —
                          amy.c contient aussi la chaîne FX de bus GrvEP (voir « Effets »)
simulator/                Simulateur SDL2 (voir SIMULATOR.md)

structure/                Documentation (voir README.md ; HANDOFF.md = point d'entrée)
```

---

## Modes (46 implémentés)

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
| EUCLI | `MODE_EUCLI` | Séquenceur euclidien 4 pistes : touche = nombre de pas (palette 4-32), P4-P7 = nombre de coups, affichage en anneaux concentriques (voir section dédiée plus bas) |
| CRUNCH | `MODE_CRUNCH` | Tracker 4 pistes façon MothOS, enregistrement live (pas d'édition pas-à-pas) : touches = 4 shifts + 12 notes/commandes, 12 slots instrument (2 banks + 10 mélodiques) (voir section dédiée plus bas) |
| AMBIENT | `MODE_AMBIENT` | Synthé drone/pad inspiré de Casual Machines "Slide Synth Lab" : réutilise le moteur de MODE_MODULAR (mêmes canaux/fonctions) avec des défauts ambiants — reverb+chorus auto, dérive de wavetable active, mode drone/latch (voir section dédiée plus bas) |
| NOOB2 | `MODE_NOOB2` | NOOB (mélodie générative) rejouant à travers le moteur de MODE_AMBIENT au lieu de SYNTH_CH ; P2 choisit un `AmbientPresetDef` au lieu d'un `SynthShape` |
| OMNI2 | `MODE_OMNI2` | OMNI (accords + strum) rejouant à travers le moteur de MODE_AMBIENT, même bascule P2 que NOOB2 |
| BREAK | `MODE_BREAK` | Générateur de breakbeat probabiliste (inspiré X0X/BB-Gen) : un sample SD découpé en 8, un générateur MOVE/STAY le joue, la grille 4×8 est une couche de performance live par-dessus (colonne=slice, ligne=variante normal/reverse/demi-vitesse/stutter) — construit sur le moteur GRANULAR2 (voir section dédiée plus bas) |
| FELU | `MODE_FELU` | Synthé inspiré de Felucca : 9 de ses 13 moteurs (ANALOG/WHEEL/TRIO/PHASE/FM6/LOFI/VOICE/PHYS/NOISE) réimplémentés en `SynthShape` AMY + 4 macros P4-P7 (même schéma que MOD2 — pas un portage DSP littéral, voir section dédiée plus bas) |
| DRUMS2 | `MODE_DRUMS2` | Kit batterie **synthétisé** façon TR-808 (8 pads, même gabarit que DRUM2) : SINE/TRIANGLE/NOISE + filtre + enveloppes AMY, pas de PCM (voir section dédiée plus bas) |
| DRUMS3 | `MODE_DRUMS3` | Même gabarit que DRUMS2, caractère TR-909 (plus percutant/brillant) (voir section dédiée plus bas) |
| PRESETS | `MODE_PRESETS` | Banque de 45 patchs curatés (8 catégories façon Sloop) parcourue par browser 2 colonnes, posée sans nouveau DSP sur le moteur de MODE_FELU (voir section dédiée plus bas) |

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
| 14 | `MOD3_OSCA_CH` | MODE_MODULAR **et** MODE_AMBIENT (mutuellement exclusifs), oscillateur A (wavetable) |
| 15 | `MOD3_OSCB_CH` | MODE_MODULAR **et** MODE_AMBIENT, oscillateur B (wavetable, détune fixe ou réglable) |
| 60 | `PCM_PREVIEW_OSC` | Preview sample SD browser |
| 70–101 | `DRUM_OSC_BASE + 0..31` | Pads batterie (32 sons uniques) |
| 150–181 | `GRANULAR_OSC_BASE + 0..31` | Granular slicer slices |
| 182–213 | `SAMPLE_OSC_BASE + 0..31` | Lecture keys samples (polyphonique) |
| 214–217 | `CRUNCH_OSC_BASE + 0..3` | CRUNCH : 4 pistes, un oscillateur fixe et monophonique par piste (pas de round-robin — une nouvelle note sur une piste déjà sonore retrigger simplement l'oscillateur de cette piste) |
| 240–245 | `STONE_OSC_BASE + 0..5` | STONE : 6 voix round-robin sur un seul sample |

`MODE_MODULAR` (canaux 14-15) n'appartient PAS à `SYNTH_CH` — c'est un des modes (avec
STONE/GRANULAR2/DRUM2/SAMPLE/CRUNCH) à utiliser des canaux/oscillateurs dédiés plutôt que
l'allocateur de voix dynamique d'AMY. Piège classique (« un FX partagé n'atteint que ce qu'on
a câblé ») : tout effet appliqué **par oscillateur/canal** doit être explicitement étendu à
chaque plage dédiée (`audioApplyFilterToStone/Modular/DJ/Crunch()` dans `audio_engine.cpp`).
**Depuis que FILT est un effet de bus, plus aucun FX de la grille n'a ce problème** (tous
travaillent sur le mix du bus 0) — sauf l'option Cut de BITCRS, qui utilise encore
`audioSetAllFiltersT()` par oscillateur. Voir « Effets ».

### Compensation de niveau par timbre (`shapeGain[]`)

Chaque `SynthShape` (35 timbres, `config.h`) passe par `audioNoteOn()` (`audio_engine.cpp`),
qui applique `e.velocity = velocity * s_patchVolumeScale`. Avant cette passe, **une seule**
des 35 formes avait une compensation (`patch==8` → Juno Organ, codé en dur à 0.55) — toutes
les autres (patches AMY Juno/DX7 d'amplitude native très variable, ondes custom empilant
1 à 6 oscillateurs détunés à des résonances de 0.7 à 14) sortaient à un gain identique,
donnant des écarts de volume flagrants en changeant de timbre (rapporté par l'utilisateur).

`config.h` définit maintenant `shapeGain[SHAPE_COUNT]`, appliqué uniformément dans
`audioSetShapeOnSynth()` (remplace les deux anciennes affectations ponctuelles de
`s_patchVolumeScale`). Valeurs mesurées par un sweep headless (tap RMS/pic temporaire dans
`libminiaudio-audio.c`'s `data_callback`, note tenue 750ms par timbre à vélocité 1.0,
fenêtre de mesure en régime établi après l'attaque) : normalisées sur le niveau de SAW
(~9500 RMS), **atténuation uniquement** (jamais >1.0 — un timbre plus calme que SAW n'est
pas amplifié, pour ne pas introduire de bruit de fond ou un nouveau risque de saturation).
Les timbres à base d'ALGO FM personnalisé (FM_DRIFT/FM_BELL/SAT_DRIFT, patch=-2) et
JUNO_ORGAN/JUNO_ORGAN2 saturaient déjà à pleine échelle (pic=32767) à vélocité 1.0 — leur
première estimation de gain (basée sur un RMS déjà écrêté, donc sous-estimé) a nécessité un
second passage de mesure/ajustement pour vraiment sortir de la saturation.

**Piège méthodologique découvert (INDUSTRIAL toujours trop fort après le premier passage,
rapporté par l'utilisateur)** : la fenêtre de mesure « régime établi après l'attaque »
sautait délibérément les ~250 premières ms — exactement la durée du burst de filtre
résonant (EG1, `resonance=12`, `10→350ms`) qui fait le caractère de SHAPE_INDUSTRIAL. Le
sweep original ne mesurait donc jamais son moment le plus fort. Re-mesuré sur la note
COMPLÈTE (900ms, sans saut d'attaque) : RMS réel ~22900 (2,4× SAW) contre ~9580 mesuré en
régime établi — d'où le gain corrigé `0.82→0.34`. Les 34 autres timbres ont été re-vérifiés
avec cette même méthode « note complète » : aucun autre n'a un écart comparable (quelques
timbres avec un pic bref élevé mais un RMS déjà sous la cible, ex. J:STR/D:BRS — un pic
d'attaque court ne se traduit pas forcément en "trop fort" perçu si le RMS moyen reste
contenu). À retenir pour tout futur ajustement de `shapeGain[]` : mesurer la note complète,
pas seulement le régime établi, pour les timbres avec une enveloppe de filtre (EG1) qui
fait un burst court.

---

### Bus AMY

| Bus | Usage |
|-----|-------|
| 0 | Drums, synth principal, tout le reste |
| 1 | T303 exclusivement (`e.bus = 1` dans `audioT303Init`) |

**Le bus 1 est mergé dans le bus 0 avant tout le traitement FX** (`amy_fill_buffer()`, bloc
"Merge T303 bus into bus 0 BEFORE FX"). Reverb/chorus/echo/FX GrvEP traitent donc le signal
T303 sans delay lines supplémentaires (`config_reverb/echo` sur bus 1 = crash PSRAM, voir bugs).

**Buffers de bus (`fbl[core][bus]`) : PLANAIRES** — canal `c` à `buf + c*AMY_BLOCK_SIZE`
(le commentaire « interleaved » de `mix_with_pan()` est faux). `SAMPLE` est du **point fixe
s8.23 sur TOUTES les cibles, simulateur compris** (`#define AMY_USE_FIXEDPOINT` en dur dans
`amy.h`) : tout code DSP de bus doit passer par `S2F()`/`F2S()` pour comparer à une valeur
normalisée (voir le bug COMPRESS).

Ordre de traitement dans `amy_fill_buffer()` (`lib/AMY Synthesizer/src/amy.c`) :
1. mix des deux cœurs → 2. wavefold du bus 1 (303) → 3. merge bus1→bus0 →
4. **`amy_apply_ordered_fx(fbl[0][0])`** (FX GrvEP, dans l'ordre d'activation — voir « Effets ») →
5. boucle par bus des FX **natifs AMY** (EQ → chorus → echo → reverb, ordre fixe) →
6. mix final pondéré par le volume de chaque bus, HPF de sortie optionnel, soft-clip → int16.

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

### "Accent" retiré, B3 (PAD view) repurposé en sélecteur de "Patch" (algo/texture)

Demande utilisateur : « pour les 303, supprime la possibilité d'accent, pour utilisé la
touche pour changer de patch, ça ne change pas la forme d'onde initial défini par le
joystick, mais plutôt l'algorithme/texture de la 303 ». `t303AccentOn` (vélocité 0.85 vs
0.5, toggle global) est retiré entièrement — tous les points de consommation (303S PAD/SEQ
playback, 303S2 live+séquence, le track 303 partagé GEST/DR2/SYSEQ) utilisent maintenant
une vélocité fixe à 0.5. Le bouton B3 qui togglait l'accent en vue PAD de 303S, et le
bouton B3 unique de 303S2, cyclent maintenant `t303Patch` (`t303CyclePatch()`) :

```cpp
#define T303_PATCH_COUNT 5
static const char*   kT303PatchNames[T303_PATCH_COUNT]        = {"24dB", "12dB", "BPF",  "Acid", "Soft"};
static const uint8_t kT303PatchFilterTypes[T303_PATCH_COUNT]  = {FILTER_LPF24, FILTER_LPF, FILTER_BPF, FILTER_LPF24, FILTER_LPF};
static const float   kT303PatchResoMult[T303_PATCH_COUNT]     = {1.0f, 1.0f, 1.0f, 1.4f, 0.6f};
static const float   kT303PatchEnvModMult[T303_PATCH_COUNT]   = {1.0f, 1.0f, 1.0f, 1.35f, 0.55f};
```

**Itération** (rapporté juste après la première version) : « j'aime bien le 24/12/BPF, le
HPF est inutile. j'aimerais également un patch ACID, et un patch soft, afin d'obtenir des
textures de 303 plus variées ». HPF retiré ; Acid/Soft ajoutés — mais un simple `filter_type`
de plus n'aurait rien changé de "plus varié" (il n'existe que 4 types de filtre dans AMY,
déjà tous utilisés par 24dB/12dB/BPF/l'ancien HPF), donc Acid/Soft réutilisent `FILTER_LPF24`/
`FILTER_LPF` mais appliquent en plus un **multiplicateur de résonance et de profondeur
d'enveloppe de filtre** — Acid pousse les deux vers le haut (squelch/sweep plus agressif,
signature acid classique), Soft les réduit (plus doux, sweep atténué). Deux nouvelles
fonctions `t303EffReso()`/`t303EffEnvMod()` calculent `t303Reso * kT303PatchResoMult[...]`/
`t303EnvMod * kT303PatchEnvModMult[...]` — **le potard Reso/EnvMod continue d'afficher et de
stocker la valeur brute de l'utilisateur**, seule la valeur envoyée au moteur passe par le
multiplicateur, à chacun des 9 points d'appel de `audioT303Init()`/`audioI303Init()`/
`audioT303Params()` (303S/303S2/I303/GEST/preset browser), remplaçant `t303Reso, t303EnvMod`
par `t303EffReso(), t303EffEnvMod()`. `t303CyclePatch()` pousse aussi immédiatement
`audioT303Params()` avec les valeurs effectives (pas seulement `filter_type`), pour que
changer de patch s'entende tout de suite même sans toucher les potards Reso/EnvMod.

Nouvelle fonction `audioT303SetFilterType()` (`audio_engine.cpp`) — pose uniquement
`e.filter_type` sur `T303_CH`, sans toucher au `wave` (qui reste piloté par le joystick,
inchangé) ni à aucun autre champ : un "touch léger", pas une reconfiguration complète, donc
sûr en cours de note (même catégorie que `audioT303Wave()`/`audioT303Duty()`). `t303Patch`
est un état **partagé** (comme `t303Wave`/`t303Cutoff`/etc.) entre 303S/303S2/I303/GEST —
`audioT303Init()`/`audioI303Init()` remettent `filter_type` à `FILTER_LPF24` à chaque appel
(comportement d'origine inchangé), donc chaque hook d'entrée de mode (303S/303S2/I303/GEST)
réapplique explicitement `audioT303SetFilterType(kT303PatchFilterTypes[t303Patch])` juste
après son propre `audioT303Init()`/`audioI303Init()`, pour que le patch choisi survive un
changement de mode.

Vérifié par test headless : les 5 multiplicateurs calculent exactement les valeurs
attendues (Acid ×1.40/×1.35, Soft ×0.60/×0.55, les 3 autres inchangés à ×1.0), et un tap
RMS/pic sur la même note confirme un rendu audiblement différent entre les 5 patches
(ex. BPF nettement plus filtré/plus calme que 24dB/12dB, Acid légèrement plus énergique que
24dB malgré un réglage de base identique, Soft nettement moins piqué).

En vue SEQ de 303S, B3 cyclait avant NRM→ACC→SLD (`s303PendingAlt`, 3 états) ; devient un
simple bascule NRM↔SLD (ACC retiré du cycle, 0 et 2 seulement — pas de renumérotation des
tableaux d'affichage existants (`s3alt[]`/`altlbl[]`/`aH[]`/`aS[]`), l'index 1 (ACC) devient
juste mort/jamais atteint). Vérifié par test headless (`handleButton()` simulé) : en vue
PAD, B3 cycle bien les 4 patches sans toucher `s303PendingAlt` ; en vue SEQ, B3 bascule bien
`s303PendingAlt` entre 2 et 0 sans toucher `t303Patch`.

Hors périmètre (scope volontairement limité à "le bouton") : le mécanisme NRM/ACC/SLD de
MODE_GROOVE (`grvAlt[]`/`grvWriteAlt`, piloté par un potard P5, pas un bouton, et partagé
avec sa propre piste SYNTH) est resté inchangé — c'est un mécanisme distinct, pas "la
touche" visée par la demande.

### MODE_303S2 : touches d'altération maintenues (SLIDE/ACCENT/OFFBEAT) — historique révisé

**⚠️ Cette section remplace un mécanisme antérieur de "split de clavier" (2 rangées
normales + 2 rangées slide, bascule au B4) qui a été explicitement rejeté puis
entièrement supprimé par le propriétaire du projet** : « Je trouve que le mécanisme de
split de clavier dans la 303 n'est pas ergonomique, on va supprimer cette fonctionnalité ».
Si une autre doc (`structure/HANDOFF.md`, un historique de commit) décrit encore des
rangées SLIDE/NORMAL ou un `s303s2SplitRows`/B4-toggle, c'est une description de l'ancien
système — **plus aucune trace de ces symboles dans le code actuel** (confirmé par grep).
Les deux approches ont joué la même hauteur sur 2 touches différentes et le propriétaire a
jugé ça peu intuitif ; la bonne occasion d'en retirer des idées réutilisables plutôt que de
jeter tout le travail (le mécanisme `t303SlideFrom/To/Ms/Active` de glissando, lui, est
antérieur et reste utilisé tel quel).

**Nouveau mécanisme — 3 touches modificatrices maintenues, alignées verticalement en
colonne 0** : au lieu de diviser les 31 notes jouables en zones, la grille garde sa pleine
tessiture (`noteMap.getMidiNote(row,col)` partout) et gagne 3 touches dédiées qui, tenues
enfoncées PENDANT qu'on appuie une note, appliquent une altération à cette note :

```cpp
#define T303ALT_SLIDE   0x01
#define T303ALT_ACCENT  0x02
#define T303ALT_OFFBEAT 0x04
```

- **Colonne 0, lignes 0-2** (alignées verticalement juste au-dessus de la touche REST en
  `row3,col0`) : ligne 2 = SLIDE, ligne 1 = ACCENT, ligne 0 = OFFBEAT (la plus éloignée de
  REST). `s303s2ModHeld` (bitmask) suit en temps réel quelles touches modificatrices sont
  actuellement maintenues ; `handleNoteKeyAudio` capture ce bitmask AU MOMENT de l'appui
  d'une note normale et le stocke dans `s303s2Alt[s303s2Head]` (buffer en anneau parallèle
  à `s303s2Seq[]`, renommé depuis l'ancien `s303s2Slide[]` qui ne portait qu'un flag
  booléen). Appuyer une note SANS aucune touche colorée maintenue = note normale,
  inchangé.
- **SLIDE** (prévisualisation en temps réel ET à la lecture) : réutilise
  `t303SlideFrom/To/Ms/Active` déjà existant — si une note sonne déjà, le pitch glisse
  vers la nouvelle au lieu de réattaquer l'enveloppe.
- **ACCENT** (prévisualisation ET lecture) : vélocité fixe plus haute (`0.85` au lieu de
  `0.5`) au lieu de la vélocité normale.
- **OFFBEAT** (lecture uniquement — concept purement temporel, aucune prévisualisation au
  moment de l'appui) : le déclenchement du pas est différé à la MOITIÉ de l'intervalle de
  pas plutôt qu'au début du pas. Implémenté par le même idiome que les autres
  "pending-triggers" différés du projet (cf. `drum2DblPendingAt[]`) :
  `s303s2SyncPendingAt/Note/Slide/Vel` posés au tick de lecture quand `T303ALT_OFFBEAT` est
  actif (`s303s2SyncPendingAt = millis() + waitMs/2`), puis un poll inconditionnel placé
  tout en haut de `loop()` (juste après le poll de `drum2DblPendingAt[]`, avant le tick
  principal du séquenceur) déclenche la note en retard quand `millis()` l'atteint.

**`Gest303Pat`** (sauvegarde des patterns GEST) a son champ renommé `slide[]` → `alt[]`
(même type `uint8_t`, maintenant un bitmask au lieu d'un booléen) ; les 8 points de
copie/sauvegarde/restauration/effacement de `s303s2Seq`/`g_303sPats[].seq` transportent
`s303s2Alt` en parallèle, comme avant pour `s303s2Slide`.

**B4** est redevenu un simple CLEAR en appui court (plus de distinction court/long press,
plus de `s303s2SplitRows`/`S303S2_B4_LONGPRESS_MS`) : efface `s303s2Seq`/`s303s2Alt`,
remet `head`/`count`/`step` à 0, coupe la note en cours s'il y en a une.

**Affichage** (`drawScreen`'s `MODE_303S2` case) :
- Hint du bas : « Left col: REST,SLD,ACC,OFB » (décrit la colonne verticale).
- Popup affiché pendant qu'une touche modificatrice est maintenue : libellés `"SLIDE "`,
  `"ACCENT "`, `"OFFBEAT "` (pas `"SYNC "` — nom jugé incompréhensible par l'utilisateur et
  renommé en OFFBEAT, qui décrit effectivement le comportement de contre-temps).
- LEDs (`updateLedsAndShow`) : les 3 touches modificatrices (colonne 0, lignes 0-2) ont
  chacune leur propre teinte fixe (`kModHue[3] = {220, 40, 130}`, indexé par ligne —
  magenta/OFFBEAT, jaune/ACCENT, cyan/SLIDE), indépendante de l'état de la séquence,
  pour qu'elles restent repérables au premier coup d'œil parmi les 28 autres touches de
  notes normales.

**Séquenceur visuel — remplacement des cases texte par des formes géométriques** :
rapporté par l'utilisateur comme illisible (« les labels sont illisibles ») après une
première tentative qui gardait le nom de note en texte et ajoutait juste des petits traits
pour les altérations. Une 2e tentative a remplacé le texte par un point dont la position
verticale encodait la hauteur de la note (mini piano-roll) — **rejetée à son tour** : « je
ne cherche pas à indiquer la note via cette forme, je voulais plutôt simplement placer des
triangle, carré, rond... en fonction du type de note placé (normal ou les altérations) ».
**Design final** : UNE forme géométrique par pas, choisie par le TYPE de note (pas par la
hauteur), dimensionnée pour occuper la quasi-totalité de la case (demi-étendues 6px/5px
sur une case de 16×13) plutôt qu'une petite icône centrée — demande explicite de
l'utilisateur après la 1ère version (icônes ~6px de diamètre, jugées trop petites) : « je
veux des forme géométrique qui prennent l'entièreté des cases du séquenceur » :
- Note normale (aucune altération) : **disque plein**.
- SLIDE : **triangle plein**.
- ACCENT : **carré plein**.
- OFFBEAT : **losange plein** (2 triangles pleins, moitié haute + moitié basse).
- Pas REST enregistré (silence placé explicitement) : **anneau creux** (cercle non rempli,
  rayon modeste — contrairement aux 4 formes ci-dessus, un grand anneau remplirait tout
  l'espace vide et redeviendrait indiscernable d'« aucune note ») — distingue « un silence
  a été explicitement placé ici » de « rien n'a encore été enregistré » (même rendu "cadre
  vide" sinon).
- Cas de cumul (plusieurs bits d'altération actifs sur le même pas) : priorité fixe
  **SLIDE > ACCENT > OFFBEAT** — arbitraire mais fixée/documentée en commentaire, une
  seule forme ne peut pas représenter une combinaison.

**Piège trouvé en agrandissant les formes — remplissage inversé (fond plein + forme en
creux) qui s'efface lui-même à grande taille.** La version "petite icône" dessinait
d'abord un fond plein (boîte blanche) puis la forme PAR-DESSUS dans la couleur inverse
(un petit trou noir au milieu d'une case blanche) — correct à ~6px, mais à ~12px la forme
recouvre alors presque toute la boîte et n'y laisse qu'un mince liseré, perdant toute
lisibilité (vérifié au niveau pixel, pas juste à l'œil — un rendu réduit/zoomé peut
tromper l'œil sur ce genre de motif). Fix structurel : le remplissage de fond n'est plus
utilisé pour signaler "note enregistrée" (c'est la grande forme elle-même qui le fait
désormais) :
- Pas en cours de lecture normal (pas "cur") : cadre fin seulement (même rendu qu'une case
  vide), et la forme est dessinée **pleine** (couleur1) directement dessus — aucun
  effacement possible puisqu'on AJOUTE des pixels sur un fond resté noir, pas qu'on en
  retire d'un fond déjà plein.
- Pas "cur" (en cours de lecture) : la case entière passe en plein (highlight), et la
  forme est dessinée en **contour seulement** (pas remplie — 3-4 `drawLine` pour le
  triangle/losange, `drawFrame`/`drawCircle` pour le carré/cercle) — un simple TRAIT fin
  en creux ne souffre pas du même problème d'érosion qu'un remplissage plein.

**Bug de fond trouvé et corrigé au passage — mésappariement couleur de contenu / fond
réel** : la couleur du point était choisie selon `cur` seul (« est-ce le pas en cours de
lecture ») alors que le fond de la case est rempli en noir dès que `cur || !isRest` (« y
a-t-il une note enregistrée, jouée ou non ») — une condition strictement plus large. Pour
toute case contenant une note enregistrée mais PAS en cours de lecture (la grande majorité
des cases la plupart du temps), le point était dessiné dans la même couleur que son propre
fond → invisible, pas juste "petit". C'est probablement la VRAIE cause du rapport
"illisible" initial, pas la taille de police. Fix :
```cpp
bool onSolidBg = cur || !isRest;
oled.setDrawColor(onSolidBg ? 0 : 1);   // pas cur?0:1
```
et les marques d'altération sont codées en dur `setDrawColor(0)` (jamais `cur?0:1`) car ce
bloc n'est atteint que quand `!isRest`, donc le fond y est TOUJOURS plein — même piège,
même fix. **Classe de bug à surveiller ailleurs** : toute case où la couleur de contenu est
choisie sur une condition plus étroite que la condition qui remplit réellement le fond.

Vérifié par tests headless (dump OLED → PNG, inspection visuelle à plusieurs niveaux de
zoom) : alignement vertical des 3 touches modificatrices confirmé, popup affiche le bon
libellé pour chaque touche maintenue, le point de hauteur et les marques d'altération sont
bien visibles sur fond plein ET sur fond vide, lecture avec OFFBEAT actif déclenche
effectivement la note à mi-pas (vérifié par instrumentation audio temporaire, retirée
ensuite).

### Bug trouvé (toujours valable) : `menuLabels[MENU_303S2]` affichait "303S" (pas "303S2")

Rapporté par l'utilisateur après le ship de la fonctionnalité slide d'origine : « j'ai
accès à 303S2 mais pas à 303S ». Cause : `menuLabels[]` (`config.h`) avait une entrée
dupliquée copier-collée — `MENU_303S2` affichait littéralement `"303S"`, le même texte que
`MENU_303S` juste au-dessus dans le tableau (qui, lui, n'est câblé dans aucun des 3 onglets
du menu — `kMenuCatInstr`/`kMenuCatSeq`/`kMenuCatAut` — donc jamais visible du tout). Seule
case 303-liée réellement visible dans l'onglet SEQNC : celle mal libellée "303S" au lieu de
"303S2". Fix : libellé corrigé.

### P2 (texture par forme d'onde) s'initialise à 0% à l'entrée du mode

Demande explicite : « initialise le P2 à 0% dans la 303S ». P2 (`pots[1].value`) pilote une
texture différente selon `t303Wave` (duty pour PULSE, largeur pour NAP, fold asymétrique
pour TRI2/SAW2, blend pour les sub-oct waves, fold symétrique sinon — voir le dispatch pot
de `loop()`, même branches). `audioT303Init()` (appelé à l'entrée du mode) ne touche pas
wavefold/duty — sans fix, le son gardait la texture laissée par la session 303S précédente
jusqu'à ce que l'utilisateur touche physiquement le potard, qui peut être n'importe où.

Le hook d'entrée `if (newMode==MODE_303S)` applique maintenant explicitement 0% via les
mêmes branches que le dispatch pot (forcées à 0 plutôt que lues depuis `pots[1].value`),
puis fait un **pickup** sur `g_lp303swf` (= la valeur courante du potard) pour que le
dispatch du tick suivant ne voie pas un "changement" et ne resnap pas instantanément vers
la position réelle du potard — même idiome que le pickup P4-P7 (`lp303[]`) juste au-dessus
dans le même hook, mais figeant le son lui-même à 0% plutôt qu'en adoptant la valeur du
potard. Scopé à MODE_303S uniquement (303S2/I303 ont leurs propres mécanismes P2, non
demandés ici).

Vérifié par test headless : potard à 80% à l'entrée → `amy_wavefold_gain=1.0` (0% de fold,
pas les ~20x qu'aurait donné 80% de fold direct) ; reste à 1.0 après plusieurs ticks avec
le potard **non touché** (pas de resnap) ; tourner le potard à 90% après coup met bien
`amy_wavefold_gain≈22.6` (32^0.9), confirmant la reprise normale du suivi une fois le
potard effectivement touché.

---

## UI : rangée B1-B4 + popup potards P4-P7 (terminé)

Demande utilisateur : afficher partout où c'est pertinent, sans élément redondant ni
interférence avec l'existant, (1) une rangée positionnelle en haut d'écran listant les 4
fonctions B1-B4 (pas de préfixe "B1=", la position suffit — toujours 4 boutons, toujours 4
emplacements) et (2) un popup transitoire affichant la fonction + valeur courante d'un
potard P4-P7 quand on le tourne, qui disparaît automatiquement 500ms après le dernier
mouvement. Présenté explicitement par l'utilisateur comme un gros chantier (beaucoup de
modes à réorganiser) en échange d'un gain réel : moins de texte d'aide à l'écran (les
anciennes lignes "B1=... B2=..." disparaissent), lecture plus rapide des contrôles.

**Suite à une première passe** (rangée placée là où il y avait de la place libre — parfois
en haut, souvent en bas ou au milieu), l'utilisateur a demandé explicitement une 2e passe :
la rangée B1-B4 doit être **systématiquement en haut de l'écran**, juste sous le titre/la
ligne de séparation, dans TOUS les modes — jamais ailleurs, même quand "ailleurs" était plus
simple à caser. Il a aussi signalé un cas concret de texte redondant resté affiché malgré
la rangée (MODE_SAMPLE montrait encore "B2=mapAll" dans une ligne d'aide, en plus — et en
désaccord, voir plus bas — de la rangée du haut). Cette 2e passe a donc eu deux volets :
replacer la rangée de chaque mode en haut, et supprimer tout texte résiduel qui ne faisait
que répéter ce que la rangée affiche déjà.

**Réutilise `struct CtrlLabels` / `ctrlLabelsFor(AppMode m)`** (déjà existant,
`main.cpp`) — jusqu'ici consommé uniquement par `printCtrlLabels()`/
`updateSimCtrlLabels()` (print Serial debug + panneau simulateur), jamais affiché sur
l'OLED réel. ~27 modes y ont déjà une entrée réelle ; le reste tombe sur `default` (tout
`"-"`) et est volontairement ignoré (modes visuels/utilitaires).

### 1. `drawBtnRow(AppMode m, int y)` — rangée B1-B4

```cpp
static void drawBtnRow(AppMode m, int y) {
    CtrlLabels L = ctrlLabelsFor(m);
    const char* b[4] = {L.b1, L.b2, L.b3, L.b4};
    // no-op si les 4 valent "-" ; sinon 4 colonnes de 32px, texte centré par colonne,
    // clampé pour ne jamais dépasser l'écran (128px) — une étiquette plus large que sa
    // colonne (ex. "SPLIT/CLR", "PLAY/BACK") aurait sinon été coupée au bord.
}
```

Appelée **depuis le `drawScreen()` de chaque mode**, pas de point d'insertion générique
possible : la bande partagée `y=0..9` (tension batterie + titre de chaque mode) est déjà
occupée partout (vérifié par Explore agent), donc chaque mode doit faire de la place.
**Règle finale (2e passe) : la rangée va toujours juste sous le titre/hline, à `y≈17`**
(ou légèrement plus bas — `y=21`/`y=28` — pour les quelques modes à en-tête sur 2-3 lignes
comme LIFE/GEN), jamais ailleurs. Techniques utilisées pour lui faire de la place selon
la densité de l'écran :
- **Décalage de tout le contenu suivant de ~8px** vers le bas (le cas le plus courant —
  SYNTH, SAMPLE, STONE, DJ, DRUM2 SEQ/PAD/ANIM, DR2, GROOVE, EUCLI, SYSEQ, 303S PAD/SEQ,
  303S2, SS2 PAD/SEQ, MOD2, GRANULAR2) — marche sans compression tant qu'il reste du mou
  en bas d'écran pour absorber le décalage (souvent le cas : beaucoup d'écrans avaient
  leur contenu groupé en haut avec 30-50px inutilisés en dessous).
- **Fusion de lignes d'en-tête + ajout d'une hline** pour les modes qui n'en avaient pas
  (NOOB/NOOB2 : 2 lignes de titre déjà proches du bord bas de l'écran → fusionnées en une
  seule ligne pour libérer 7px, insuffisant seul donc complété par un resserrement de
  l'espacement du reste de l'écran).
- **Exploitation d'un espace mort déjà présent** sans toucher au reste de la mise en page
  (LANIM, LIFE, GEN : grille/strip de contrôle démarrant juste après l'en-tête mais avec
  30-45px de libre entre elle et le texte de pied de page → la grille est juste décalée,
  rien d'autre ne bouge).
- **DR2** : cas particulier — en-tête suivi immédiatement d'une rangée "PAT" compacte,
  elle-même suivie d'un espace de ~14px avant le texte "Mode:" ; la rangée PAT (seule)
  est décalée de 8px dans cet espace existant, le gros readout beat.step tout en bas
  (déjà proche de la limite 128px) n'est pas touché. A aussi son propre mécanisme de
  popup (`dr2PopupUntil`, voir plus bas), donc aucun `potPopupShow()` générique ajouté.

**Deux exceptions délibérées où la rangée reste hors du haut d'écran**, documentées plutôt
que forcées : **MODE_POKEMON** (sprite plein écran y=0..95, aucun espace disponible en
haut sans rogner l'image — rangée laissée en bas, sous le bandeau nom/infos, comme avant)
et **MODE_CRUNCH** (écran custom type MothOS porté pixel-à-pixel depuis le fork externe de
l'utilisateur, déjà à son budget de 128px — les 4 rangées de pistes débordent même déjà de
2px dans le code d'origine, non lié à cette session ; aucune rangée B1-B4 n'est dessinée
ici du tout, ni en haut ni en bas).

**Pièges trouvés en vérifiant par capture d'écran** (voir méthode plus bas) — chaque fois
une étiquette trop large pour sa colonne de 32px qui déborde sur la colonne voisine (le
clamp anti-débordement ne protège que contre le bord d'écran, pas contre une colonne
adjacente) :
- `MODE_DR2` : "LOOP/LIVE" + "COPY/PASTE" → raccourcies en "LOOP"/"COPY" (1ère passe).
- `MODE_EUCLI` : la grille 2x2 du bas chevauchait carrément la rangée du bas (avant le
  passage en haut) → corrigé en resserrant `cellH` (20→18px) avant de déplacer la rangée.
- `MODE_SS2` : "ALT/AUTOMAP" + "SEQ/PAD" → "ALT/AUTOMAP" raccourci en "ALT/MAP".
- `MODE_NOOB`/`MODE_NOOB2` : "PLAY/PAUSE" (10 car.) débordait sur "OCTAVE" → "PLAY/PAUSE"
  raccourci en "PLAY".
- `MODE_GROOVE` : "FX/SCALE" + "MUTE/SOLO" → raccourcies en "FX/SCL"/"MT/SOLO".
- `MODE_MODULAR`/`MODE_AMBIENT` : "OSCB WAVE" (9 car.) débordait sur "OCTAVE"/"OCT/DRONE"
  → raccourci en "OSCB WV" ; AMBIENT avait un second défaut indépendant, "OCT/DRONE" (9
  car.) trop large pour sa PROPRE colonne (dernière colonne, pas de voisin à droite mais
  le clamp écran la repoussait quand même vers la gauche, dans la colonne 3) → raccourci
  en "OCT/DRN".
- `MODE_SAMPLE` : un hint résiduel "B2=map"/"B2=mapAll" (en fait inexact — le vrai bouton
  est B3, pas B2, cf. `ctrlLabelsFor`) répétait une info déjà juste et déjà affichée par
  la rangée du haut → le hint a perdu toute référence à un bouton, ne garde que
  "Key=asgn"/"Clk=dir"/"Clk=open" (infos non couvertes par la rangée B1-B4).

### 2. `potPopupShow(label, valueStr)` + popup générique P4-P7

```cpp
#define POT_POPUP_HOLD_MS 500
static uint32_t s_potPopupUntilMs = 0;
static char s_potPopupLabel[16], s_potPopupValue[20];
static inline void potPopupShow(const char* label, const char* valueStr) {
    // copie label/valeur, s_potPopupUntilMs = millis() + POT_POPUP_HOLD_MS
}
```

Modelé directement sur le mécanisme déjà existant de MODE_DR2 (`dr2PopupUntil`/
`DR2_POPUP_MS=1200`), généralisé et avec le délai de 500ms demandé. **Un seul point de
rendu**, générique, inséré juste après la fin du grand `switch(currentMode)` de
`drawScreen()` (donc après le dessin propre à chaque mode, sans qu'aucun mode n'ait besoin
de code de dessin dédié) : si `millis() < s_potPopupUntilMs`, dessine une boîte encadrée
(effacée puis recadrée — occulte ce qu'il y avait dessous) avec le label (police 6x10) et
la valeur (police 9x18, même typographie que le popup existant de DR2).

**Point d'appel** : exactement là où chaque mode détecte déjà un changement P4-P7 (le bloc
`if(fabsf(pots[N].value-lpX)>eps){ ...; lpX=... }` de soft-pickup présent dans quasiment
tous les modes) — un seul appel `potPopupShow("CUTOFF", "1200Hz")` ajouté à CHAQUE branche,
avec le label/format déjà connu localement (pas de table de correspondance séparée).
**MODE_STONE dévie** de ce motif (deltas bruts sur `pots[N].rawDelta`, pas de `lpX`
mémorisé — commentaire d'origine : "no baseline/pickup bookkeeping needed") ; l'appel est
alors posé sur ses branches `winChanged=true`. **MODE_DR2 n'a volontairement PAS reçu
d'appel** — son propre `dr2PopupUntil` fait déjà exactement ça ; ajouter les deux aurait
été la redondance que la demande visait justement à éviter.

### Méthode de vérification (capture d'écran headless)

Les outils de capture X11 classiques (`import`, `xwd`) ne fonctionnent pas sur le Xvfb de
ce bac à sable (`unable to read X window image root: Resource temporarily unavailable`).
Solution temporaire (retirée après usage, comme toute instrumentation `TEMP_DBG_*`) : un
hook direct dans `simulator/sim_window.cpp`, lisant les pixels du renderer SDL
(`SDL_RenderReadPixels` sur `s_rend`, juste après `SDL_RenderPresent`) et les sauvant en
BMP via un flag déclenché depuis `main.cpp` (`g_wantScreenshot`/`g_screenshotPath`,
consommé par `simWindowRender()`). Séquence testée en boucle dans `setup()` :
`switchMode()` vers chaque écran cible, laisser tourner `loop()` quelques frames pour que
le rendu se stabilise, armer le flag, attendre qu'il retombe, passer au suivant. A permis
de repérer et corriger les 2 défauts ci-dessus (chevauchement DR2, bord coupé) avant de
considérer la vague 1 terminée.

### État final (rangée B1-B4)

Terminé sur tous les modes ayant de vraies fonctions B1-B4 dans `ctrlLabelsFor` : SYNTH,
SYSEQ, OMNI, OMNI2, MODULAR, AMBIENT, GROOVE, EUCLI, GEST, MOD2, GRANULAR2, SAMPLE, STONE,
I303, 303S (PAD+SEQ), 303S2, SS2 (PAD+SEQ), DRUM2 (PAD+SEQ+ANIM), DR2, LIFE, SWARM, GEN,
NOOB, NOOB2, LANIM, DJ (dont l'entrée `ctrlLabelsFor` manquante — a du vrai code de
pot-dispatch mais pas d'entrée avant cette session — a été ajoutée, tenant compte du fait
que B2/B3 sont inertes en navigation et que B4 bascule Browse/Back selon la vue). La
rangée est systématiquement en haut d'écran, sauf POKEMON et CRUNCH (voir les deux
exceptions documentées plus haut).

### 3. Refonte du popup potard — jauge circulaire + disposition en 4 coins (⚠️ remplace le §2 ci-dessus)

Le popup "boîte rectangulaire label+valeur" du §2 (modelé sur celui de DR2) a été
**entièrement redessiné dans une session ultérieure**, suite à plusieurs rounds de retours
visuels successifs de l'utilisateur, jusqu'à converger vers une jauge en demi-cercle avec
disposition en 4 coins. Les symptômes rapportés, dans l'ordre :
1. « le remplissage de la jauge principal est mieux, mais il y a un artefact bizarre en
   plus » (après une 1ère tentative de jauge circulaire, non détaillée ici).
2. « il y a un carré noir par dessus le reste, et toujours un glitch au niveau de la
   jauge » — le popup effaçait un rectangle plus large que son contenu visible réel, et un
   débordement de pixels subsistait au niveau de l'anneau.
3. Demande de **disposition explicite en 4 coins**, chaque coin affichant le potard P4-P7
   qui lui correspond physiquement, la jauge au centre :
   ```
   | Potard haut-gauche  |   jauge principale   |  Potard haut-droite |
   | Potard bas-gauche   |   jauge principale   |  Potard bas-droite  |
   ```
   plus l'exigence que **le popup s'affiche même si le potard tourné ne pilote aucun
   paramètre dans le mode courant** (jusque-là, un mode sans case `ctrlLabelsFor`/pot-
   dispatch dédiée ne déclenchait jamais `potPopupShow()`, donc aucun retour visuel du
   tout en tournant un potard "mort").
4. Dernier ajustement : « moins haut, et que la jauge soit un demi-cercle qui se remplit
   de manière circulaire, comme avant » — retour à un remplissage radial (balayage
   angulaire) plutôt qu'un remplissage linéaire, tout en gardant la disposition en coins
   et en réduisant la hauteur totale du popup.

**Découverte critique qui a bloqué plusieurs itérations : le remplisseur de triangles/
polygones d'u8g2 ignore totalement `setClipWindow()`.** `u8g2_polygon.c`'s `pg_hline()`
clippe chaque ligne de balayage contre `u8g2_GetDisplayWidth/Height()` directement (les
limites PHYSIQUES de l'écran), jamais contre la fenêtre de clip posée via
`u8g2_SetClipWindow()`/`oled.setClipWindow()`. C'est fondamentalement différent de
`drawDisc()`/`drawCircle()`/`drawBox()`, qui respectent eux correctement la fenêtre de
clip. Conséquence pratique : **tout remplissage à base de triangles doit être
géométriquement autonome (ne jamais compter sur le clipping) pour ne jamais déborder** —
c'est la cause des "artefacts"/"glitch" des rounds 1-2 ci-dessus (des triangles de
remplissage dont un sommet dépassait légèrement le rayon visible de l'anneau, jamais
rattrapés par un clip qui n'opérait pas).

**Fix définitif, par garantie de convexité** : un triangle est l'enveloppe convexe de ses
3 sommets — si TOUS les sommets sont placés à une distance ≤R d'un point de référence,
AUCUN point à l'intérieur du triangle ne peut dépasser cette distance R. Le remplissage
radial est donc construit comme un éventail de triangles dont **chaque sommet est placé à
EXACTEMENT `Rout`** (jamais au-delà), en acceptant une facette légère (sous le seuil de
perception) plutôt qu'un risque de débordement :

```cpp
const int Rout = 20, Rin = 10;
const float step = (float)M_PI / 8.0f;   // ≤ 8 triangles pour couvrir le demi-cercle
auto pointAt = [&](float ang, int &px, int &py) {
    px = cxC + (int)((float)Rout * cosf(ang) + 0.5f);
    py = cyC - (int)((float)Rout * sinf(ang) + 0.5f);
};
// éventail de triangles (cxC,cyC) → pointAt(a) → pointAt(b), a décroissant de PI vers theta
```

**Disposition finale** (`drawPotPopup()`, `main.cpp`) :
- **Demi-cercle** (dôme, bord plat au bas de l'écran, `cyC=127`), `Rout=20`/`Rin=10`,
  hauteur de bande réduite à `popupH=44` (depuis 60px dans une itération intermédiaire).
- **4 cellules de coin** (une par potard P4-P7), placées via `potGridPos()` — le même
  helper déjà établi ailleurs dans le projet pour mapper P4-P7 à leur position physique
  réelle 2×2 autour du joystick (jamais `i%2`/`i/2`, voir `TEMPLATES.md`) — largeur
  `sideW=36` chacune, occupant toute la hauteur de leur moitié (haut/bas), collées aux
  bords gauche/droite de l'écran (pas de marge morte, correction du "carré noir" qui
  écrasait visuellement tout le reste en laissant deviner le contenu du mode dessous sur
  les côtés).
- **Popup toujours affiché**, même sans paramètre mappé : le `default:` du grand
  `switch(currentMode)` de pot-dispatch (jusqu'ici `default: break;`, aucun retour visuel)
  détecte désormais tout mouvement de P4-P7 via `pots[3+p].rawDelta` et appelle
  `potPopupShow("-", "42%")` (label neutre "-", valeur = pourcentage brut de la course du
  potard) — répond à l'exigence #3 ci-dessus sans qu'aucun mode individuel n'ait besoin
  d'un cas dédié.

Vérifié par captures headless à plusieurs valeurs de potard (0.0 à 1.0) avec zooms
pixel-à-pixel confirmant zéro débordement, et par une capture passive de la fenêtre SDL
réelle de l'utilisateur (`xwininfo` pour trouver l'ID de fenêtre, puis
`import -window <id>`, ImageMagick — `xdotool` s'est montré peu fiable pour piloter
l'app dans ce bac à sable, cf. méthodologie de test du `CLAUDE.md` racine) pour confirmer
visuellement le rendu réel sans toucher à la session de l'utilisateur.

**Bug trouvé ensuite : la valeur était coupée en bas de la case, le label trop centré.**
Chaque cellule de coin fait `rowH = popupH/2 = 22px` de haut ; le label était dessiné à
`cy0+12` et la valeur à `cy0+23` — 1px au-delà du bas de SA PROPRE cellule, et carrément
hors écran pour les 2 cellules du bas (`cy0+23` y dépassait `128`, la dernière ligne de
l'écran). Fix : label remonté à `cy0+9` (proche du haut), valeur à `cy0+19` (proche du bas
mais dans les limites des 22px) — les deux lignes de texte restent maintenant pleinement
dans leur cellule sur les 4 coins.

### Historique abandonné : modes dédiés MODE_303A / MODE_303B (X0X)

Suite à « felucca fonctionne bien, tu peux ajouter les 303A,303B,808 et 909 de X0X »,
deux modes `MODE_303A`/`MODE_303B` ont été entièrement construits (2 instances T303
indépendantes, séquençage manuel façon 303S2, pas de générateur façon TB-3PO) —
`T303InstSave`/`T303InstSeq`, helpers `t303InstSaveCurrent/Load/Clear/NoteKey/ApplyP2`,
câblage complet dans `ctrlLabelsFor`/`switchMode`/`handleButton`/`handleNoteKeyAudio`/
`drawScreen`/`updateLedsAndShow`/pot-dispatch/joystick/tick séquenceur — puis vérifiés
silencieux au premier essai (bug : `t303InstLoad()` n'appelait que les fonctions "léger
réglage" `audioT303Params()`/`audioT303SetFilterType()`, jamais `audioT303Init()`, la
SEULE fonction qui configure réellement `num_voices`/`oscs_per_voice`/`wave` sur le canal
T303 — un canal jamais initialisé une première fois reste silencieux même si on lui envoie
ensuite des réglages fins). Fix validé (tap RMS/peak temporaire confirmant un signal réel
sur les deux canaux), modes pleinement fonctionnels — puis **explicitement rejetés et
intégralement supprimés** sur demande directe : « les mode 303A et 303B n'ont pas permis
d'avoir une 303 différente de la 303S. On va donc supprimer les 303A et 303B ». Toute trace
a été retirée de `config.h`/`main.cpp` (confirmé par grep). **Si une future session
envisage de retenter une "303 alternative" en dupliquant le moteur T303 sur un second jeu
de canaux**, c'est une piste déjà essayée et explicitement rejetée par le propriétaire —
chercher plutôt à enrichir 303S/303S2 eux-mêmes (c'est ce qui a été fait pour FELU/BREAK :
nouveaux algos/moteurs dans un mode existant plutôt qu'un mode jumeau). Les 808/909 X0X
(prévus "synthèse AMY 8 voix") n'ont, eux, jamais été commencés — toujours en attente si
le besoin revient.

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

- 3 onglets (`kMenuCatNames` : INSTR / SEQNC / AUTRE), contenu dans `kMenuCatInstr[]` /
  `kMenuCatSeq[]` / `kMenuCatAut[]` (`main.cpp`), tailles dans `kMenuCatSizes[]` = {17, 9, 11}.
  **Ajouter un mode = l'ajouter à une de ces trois listes ET mettre à jour `kMenuCatSizes[]`.**
- Grille de `MENU_COLS`=3 colonnes dans chaque onglet. Navigation : joystick X = onglet (sur la
  barre d'onglets) / colonne, Y = ligne ; bouton court = retour à la barre d'onglets.
- Le curseur colonne est borné à la largeur **réelle** de la ligne courante (dernière ligne
  incomplète) — voir « Bugs corrigés ».
- Piège d'affichage : `drawScreen()` teste les overlays (`s_overlay`) **avant** `menuOpen`.
  Tout chemin de code qui ouvre le menu doit aussi remettre `s_overlay = OVERLAY_NONE`, sinon
  l'écran reste figé sur l'overlay alors que les touches naviguent déjà dans le menu.

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

### Interface

Grille 4×4 dans la moitié haute de l'écran : **uniquement les noms** (`gcw=32`, `gch=17`), les
16 cases toujours bordées (les cases au-delà de `FX_COUNT` sont vides mais dessinées — grille
homogène). FILT et DRIVE affichent le **nom de leur Typ courant** (LPF/HPF/BPF/LDR,
OVRD/DIST/FUZZ/DSTR — `fxFiltTypName()`/`fxDriveTypName()`) au lieu de leur nom. Case active =
fond blanc, texte noir (aucun cadre imbriqué). Moitié basse : `*NOM` + jusqu'à 4 paramètres en
**grille 2×2 de barres étiquetées**, uniquement pour le **dernier FX activé** tant qu'il est
actif (`fxLastActivated`) — aucun indicateur de « dernier sélectionné ».

Deux notions distinctes à ne pas confondre : `fxSelected` = dernière case *pressée* (active
ou non) → c'est elle qui route P4-P7 vers les paramètres ; `fxLastActivated` = dernier FX
passé actif → n'affecte que l'**affichage**.

Sélection au clavier : colonnes 4-7 seulement, `opt = (3-row) + (7-col)*4` (donc 16 slots ;
`FX_COUNT`=14, les 2 derniers ne sont liés à aucune touche — `opt < FX_COUNT` est vérifié à
l'armement de l'appui). Les colonnes 0-3 restent libres pour jouer des notes pendant que
l'overlay est ouvert. Appui court = toggle, appui long (≥500 ms) = éditeur d'automatisation.

Les cellules sont placées par `potGridPos()` (position physique du potard correspondant). Le même widget (`drawFxParamGrid(fxIdx, gridY, cellH, labelDy, barDy, barH)`, `main.cpp`) dessine
aussi les paramètres du dernier FX activé sur l'**écran principal de MODE_SYNTH** (sous la ligne
Vol/Shp/Env ; « FX: aucun actif » sinon) — même look que les barres de piste d'EUCLI. Les
coordonnées `drawStr` sont en **ligne de base** (les glyphes montent depuis `y`) ; une étiquette
posée à `y0` d'une cellule écrêtée disparaît. Pour l'ajouter à un autre mode : l'appeler depuis
son `drawScreen`, en gardant ~113 px de bas d'écran libres du pied de page.

Ouverture de l'overlay : **par mode**, pas centralisée — voir « Disponibilité du menu FX par
mode » dans HANDOFF.md (plusieurs modes audio n'y ont pas accès : travail en attente).

### Liste (`fxList[]`, `main.cpp` — **les indices sont codés en dur, voir plus bas**)

| Idx | Nom | Params (labels) | Où ça s'applique |
|-----|-----|-----------------|------------------|
| 0 | FILT | Cut, Res, –, Typ (0 LPF, 1 HPF, 2 BPF, 3 LDR) | **bus 0**, chaîne ordonnée |
| 1 | DRIVE | Drv, Tone, Lvl, Typ (OVRD/DIST/FUZZ/DSTR) | bus 0, chaîne ordonnée |
| 2 | REVERB | Lvl, Live, Damp, Xover | natif AMY (`config_reverb`) |
| 3 | CHORUS | Lvl, Freq, Dep | natif AMY |
| 4 | FLANGER | Lvl, Freq, Dep, Dly | natif AMY (`config_chorus`, délai court) |
| 5 | DELAY | Lvl (sous-division BPM cachée dans params[1]) | natif AMY (`config_echo`) |
| 6 | EQ | Low, Mid, Hi | natif AMY (`config_eq`) |
| 7 | RESECHO | Lvl, FB, Tone, Wild | bus 0, chaîne ordonnée (moteur dédié) |
| 8 | REP | Seuil | bus 0, chaîne ordonnée (wavefold) |
| 9 | BITCRS | Bits, Cut | wavefold (chaîne) + LPF par oscillateur optionnel |
| 10 | TREMOLO | Rate, Dep | tick 10 ms sur le volume (hors chaîne) |
| 11 | RINGMOD | Freq, Mix | bus 0, chaîne ordonnée |
| 12 | COMPRESS | Thresh, Ratio | bus 0, chaîne ordonnée |
| 13 | RES | Lvl, FB, Tone | natif AMY (`config_echo`, partagé avec DELAY) |

**Indices codés en dur à mettre à jour ensemble si on insère/retire un FX** : le `switch (fx)`
de `applyFxEffect()`, les `fxList[7]/[9]/[10]` épars dans `main.cpp` (RESECHO/BITCRS/TREMOLO),
le `switch` de `amy_apply_ordered_fx()` dans `amy.c` (0,1,7,8,9,11,12), `fxFiltTypName()`
(idx 0) / `fxDriveTypName()` (idx 1), `FX_COUNT`. AUTOPAN et OVERDRIVE/DISTORT séparés ont été
supprimés/fusionnés (d'où la numérotation actuelle) ; l'ancien FX LFO aussi (remplacé par
l'automatisation générique ci-dessous).

### Chaîne de bus dans l'ordre d'activation

Demande de l'utilisateur : « les FX se placent les uns à la suite des autres selon l'ordre
d'activation — si j'active ResEcho puis LPF : synth > ResEcho > LPF > sortie ».
Implémentation : `fxOrderList[]`/`fxOrderCount` (`main.cpp`, mis à jour par `fxOrderAdd/Remove`
à chaque toggle) sont lus **directement par `amy.c`** via `extern uint8_t fxOrderList[]`. À
chaque bloc audio, `amy_apply_ordered_fx()` parcourt la liste et appelle la fonction de bus
de chaque FX actif de la table ci-dessus marqué « chaîne ordonnée » (`amy_apply_filt`,
`_drive`, `_resoecho`, `_wavefold_bus0`, `_ringmod`, `_compressor`). Vérifié à la mesure :
ResEcho→LPF réduit fortement la résonance de ResEcho, LPF→ResEcho la laisse intacte.

**Limites** : REVERB/CHORUS/FLANGER/DELAY/EQ/RES sont des FX *natifs AMY* qui tournent dans
la boucle par bus d'AMY, toujours **après** la chaîne GrvEP et dans un ordre fixe entre eux
(EQ→chorus→echo→reverb) quel que soit l'ordre d'activation. Les réordonner demanderait de
scinder le mixeur d'AMY. TREMOLO n'est pas dans la chaîne (LFO de volume). REP et BITCRS
partagent un seul gain (`amy_wavefold_gain_bus0`).

Le filtrage par oscillateur/canal (`audioSetAllFiltersT`, `audioApplyFilterToX`,
`s_pcmLPF*`, `remapCutoffResToNative`) n'est **plus utilisé par FILT** ; il ne sert plus qu'à
l'option Cut de BITCRS (+ `audioRestoreShapeFilter`). Code hérité candidat au nettoyage —
vérifier les appelants (`grep`) avant de supprimer.

### FILT (idx 0)

- Fonctions : `audioSetFiltBus(cut, res, typ, on)` (LPF/HPF/BPF) et `audioSetLadderFilter()`
  (LDR), appelées depuis `applyFxEffect()` case 0 **et** depuis le tick 10 ms de `loop()`
  (« Smooth FILT cutoff application » : `lpfSmoothCut`, anti-zipper, sauté quand un slot
  d'automatisation pilote le cutoff).
- LPF/HPF/BPF = filtre d'état **TPT/Zavalishin** (`amy_apply_filt`, `amy.c`) :
  inconditionnellement stable. Une première version en forme de Chamberlin divergeait à
  cutoff élevé + forte résonance (sortie collée à pleine échelle, y compris après retour à
  des réglages sains) — n'y revenez pas. Garde NaN/inf intégrée (remet les états à 0).
- Résonance : `e.params[1]` brut 0.5-3.0 reprojeté par `filtResonances()` en Q 0.7-9.7
  (LPF/HPF/BPF, `k=1/Q`, `k≥0.08`) ou 2.25-18 (LDR). **Res est mémorisé par Typ**
  (`filtResByTyp[4]` dans `applyFxEffect`) : sinon une résonance réglée « à l'oreille » pour
  LDR reste telle quelle en repassant sur LPF (Q proche de l'auto-oscillation).
  Activer FILT remet toujours Typ sur LPF (`overlayFxToggle`).
- Piège AMY découvert en mesurant : les types **par voix** `FILTER_LPF`/`HPF`/`BPF` d'AMY
  ont un défaut de calibrage (chemin « normbits » de `filter_process()`, filters.c) — un cutoff
  18000 Hz sonnait *plus faible* qu'un cutoff 100 Hz. `FILTER_LPF24` n'a pas ce défaut.
  Non corrigé dans AMY ; évitez les filtres par voix sur des cutoffs extrêmes.
- Conséquence assumée de la conversion en effet de bus : FILT s'applique à tout le mix et ne
  se mélange plus avec le filtre natif des patches Juno/DX7 (l'ancien `remapCutoffResToNative`).

### DRIVE (idx 1)

Un seul moteur bus 0, 4 courbes (`amy_apply_drive`) partageant Drv (pré-gain), Tone
(one-pole post-saturation, 18 kHz→400 Hz), Lvl (gain de sortie) : 0 Overdrive (tanh), 1
Distort (exp saturante), 2 Fuzz (hard clip), 3 Destrukt (repli/wavefold). Fusion des anciens
OVERDRV et DISTORT — qui n'étaient *que* des balayages de filtre, sans aucun waveshaping
(« ressemble juste à un LPF »).

### RESECHO (idx 7) et RES (idx 13)

- **RES** = l'ancien ResEcho : `audioSetDelay()` → `config_echo` (bus d'écho AMY partagé avec
  DELAY ; ne pas activer RES et DELAY ensemble, ils se disputent la ressource).
- **RESECHO** = moteur dédié (`audioSetResoEcho` → `amy_resoecho_config`, `amy_apply_resoecho`) :
  ligne à retard mono de 700 ms (PSRAM, allouée à la première utilisation) + filtre d'état
  résonant dans la boucle de feedback, sous-division fixe 1/8 synchronisée BPM.
  **Param 4 « Wild » (0→1)** apprivoise/libère *ce même moteur* (pas un mélange de deux
  moteurs — refusé explicitement par l'utilisateur) : `resonance = 1 + wild·(1.5+fb·2.5−1)`,
  `effFeedback = (fb/0.85)·(0.35+1.05·wild)`.
  Le passe-bande a un gain de crête **unitaire** (`band*q`), donc le gain de boucle EST
  `effFeedback` : `<1` décroît, `≈1` (Wild ≈ 0.6 à FB max) entretient, `>1` (Wild → 1, jusqu'à
  1.4) **croît à chaque passage jusqu'au limiteur doux** `2·tanh(x/2)` dans la boucle : un son
  entretenu, saturé, riche en harmoniques, à un plafond **connu** — pas un emballement. Bornage
  volontaire : un LPF en aval atténue alors un signal de taille normale (pas de niveau infini à
  combattre). Historique : à l'origine le gain de boucle valait `fb/q > 1` avec un plafond dur
  ±4 (« volume infini », inatteignable par un filtre en aval) ; le correctif « gain unitaire
  max » a ensuite rendu impossible toute saturation (rapporté) — d'où le mapping actuel.
  Mesuré (Lvl 0.9, FB 0.85, traîne à ~4 s) : Wild 0.3 → silence, 0.6 → RMS ~415 (entretien
  doux), 0.8 → ~8600 (saturé), 1.0 → ~14300 (pic ~62 % de la pleine échelle) ; LPF placé après :
  800 Hz → ~2400, 200 Hz → ~130.

  **Param 3 « Damp » (remplace « Tone »)** : le filtre d'état résonant (Chamberlin SVF)
  calcule 3 sorties chaque échantillon (low/band/high) mais seule `band` (passe-bande)
  était réinjectée dans la boucle de feedback — la résonance se concentrait donc sur UNE
  fréquence précise (`centre_hz`, pilotée par « Tone »), rapporté comme "ça résonne
  spécifiquement sur une fréquence définie, je veux que ça résonne dans la totalité du
  spectre audio". Fix (`amy_apply_resoecho()`, `amy.c`) : la boucle réinjecte maintenant
  `low` (passe-bas) au lieu de `band` — résonance large bande/façon peigne sur tout le
  passe-bas, seulement assombrie progressivement au-dessus de la coupure, au lieu d'un pic
  étroit à un endroit arbitraire. `Wild`/la résonance interne du SVF continuent d'influencer
  `band`, que `low` intègre à chaque échantillon, donc `Wild` garde son rôle
  apprivoise↔sature. « Damp » (0→1, remplace l'ancien « Tone » -0.5→0.9) pilote maintenant
  simplement la coupure : 0=brillant (~8000Hz, presque tout le spectre résonne), 1=sombre
  (~300Hz, les répétitions s'assombrissent vite) — un réglage familier de type
  reverb/écho, plus pertinent qu'un sélecteur de fréquence de résonance.
  Le tap `low` seul étant nettement plus fort que l'ancien `band*q` (déjà normalisé à gain
  de crête unitaire ; `low` ne l'est pas et sature à pleine échelle aux mêmes réglages
  Lvl/FB/Wild) : compensé par un facteur fixe `×0.22` (mesuré/ajusté par sweep headless
  jusqu'à sortir de la saturation). Mesuré (Lvl 0.9, FB 0.85, Wild 1.0, traîne post-note) :
  Damp 0.0→RMS 1858 (brillant), 0.3→1363, 0.6→2119, 0.9→1964, 1.0→579 (sombre, décroît
  vite) — aucun étage ne sature (pic max ~16700/32767). Sweep Wild à Damp=0.3 fixe :
  0.0→RMS 909, 0.3→922, 0.6→949, 1.0→1361 — Wild continue de faire varier l'entretien,
  quoique plus progressivement qu'avec l'ancien tap bandpass (attendu : `low` n'a pas le
  même pic résonant marqué que `band` à fort Q).

### COMPRESS (idx 12)

Compresseur feedforward à enveloppe de crête, liaison stéréo, 5 ms/80 ms fixes. Bug corrigé :
l'enveloppe comparait la valeur brute `SAMPLE` (point fixe s8.23, ~8,4 M par unité ; mesuré ~7 M
sur le simulateur) au seuil normalisé 0-1 → réduction de
gain toujours maximale (« réduit juste fortement le volume »). Toujours `S2F()` avant de
comparer à une grandeur normalisée.

### LADDER — sifflement dans les aigus (fc2 taper)

(Code maintenant dans `amy_apply_filt`, branche `amy_ladder_on`.)
Le pic résonant de LADDER vient de la différence entre deux cascades passe-bas (une
normale, une réglée 1.6× plus brillante) — voir le commentaire "History" dans `amy.c`. À
cutoff élevé, l'ancienne implémentation plafonnait dur la cascade brillante près de Nyquist,
la rendant quasi transparente : la différence devenait du contenu aigu brut non filtré (un
grésillement large bande). Fix : le ratio 1.6× se réduit progressivement vers 1.0× à mesure
que le cutoff approche Nyquist (`headroom = (nyquist-cutoff)/nyquist`). Pas de boucle de
feedback dans cette conception : elle ne peut structurellement ni s'auto-osciller ni
« rester coincée ».

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
qui généralise le pattern déjà utilisé par le FX TREMOLO (accumulateur de
phase + porte de profondeur + restauration propre à la désactivation), sans les modifier :
`gModSlots[0..3]` sont un pool général alloué dynamiquement par cette UI,
`gModSlots[4]` est réservé au LFO du synthé modulaire (voir plus bas).
La cible `MODDEST_FX_PARAM` applique la modulation en écrivant temporairement dans
`fxList[fx].params[param]`, en appelant `applyFxEffect(fx, /*silent=*/true)` (le tick
10ms de cette automatisation appelle `applyFxEffect()` en continu ; le flag `silent`
évite de spammer le terminal à chaque tick — les appels normaux, hors automatisation,
gardent leur log), puis en restaurant la valeur « centre » réglée par l'utilisateur.

### FX Punch (appui long, momentané) — `OVERLAY_FX_PUNCH` — **16/16 effets (phase 1+2)**

Demande utilisateur : « j'aime bien les FX punch de Sloop, j'aimerais la même chose sur ce
synthé, si je maintiens appuyé FX, alors j'accède à un menu qui me permet d'appliquer des
effets sur mon son tant que c'est maintenu appuyé ». **Sloop** (une custom firmware pour le
M-VAVE FM-1, `github.com/isod89/sloop-fm1`) est elle-même construite sur **Felucca** — la
même lignée qui a inspiré MODE_FELU — et expose exactement 16 effets « punch » (voir
`SLOOP.md#punch-in-effects`) : tenir le bouton FX layer puis tenir une des 16 touches
applique l'effet sur le mix entier UNIQUEMENT tant que la touche est tenue, et revient
proprement au relâchement.

**Scopé en 2 phases** (question posée à l'utilisateur, confirmée) : la moitié des 16 effets
Sloop (les 4 fractions de boucle, stutter, reverse, tape stop, half speed) nécessitent un
moteur de **capture du mix en direct** (ring buffer + lecture à vitesse/position variable) —
**phase 2, implémentée dans une session suivante** (voir sa propre sous-section plus bas).
L'autre moitié se branche directement sur des FX déjà existants dans `fxList[]` — **phase
1** :

| Touche Sloop | `PunchKind` | FX réutilisé | Réglage appliqué pendant le hold |
|---|---|---|---|
| low-pass sweep | `PUNCH_LP_SWEEP` | FILT (idx 0, Typ=LPF) | Cutoff balayé bas→haut sur ~2s (tick 10ms dédié, voir plus bas) |
| high-pass sweep | `PUNCH_HP_SWEEP` | FILT (idx 0, Typ=HPF) | Cutoff balayé haut→bas sur ~2s |
| phone | `PUNCH_PHONE` | FILT (idx 0, Typ=BPF) | Bande fixe ~1500Hz (timbre téléphone) |
| bit crush | `PUNCH_BITCRUSH` | BITCRS (idx 9) | Bits=3 (crush fort) |
| alias (sample-rate drop) | `PUNCH_ALIAS` | REP (idx 8) | Seuil de wavefold bas (0.08, gain≈12.5x) — **approximation**, aucun vrai décimateur n'existe encore, c'est le plus proche équivalent déjà dans le moteur. ⚠️ Révision : 0.85 (gain≈1.18x) d'origine était quasi inaudible, voir plus bas |
| gate 1/16 | `PUNCH_GATE` | TREMOLO (idx 10) | Rate=fréquence du 1/16 au BPM courant, Dep=1.0 |
| echo (dotted 1/8) | `PUNCH_ECHO` | RES (idx 13) | Subdivision "D1/8" (`kDelaySubdiv[3]`, déjà existante) |
| tape wobble | `PUNCH_WOBBLE` | FLANGER (idx 4) | Modulation LFO courte — approximation du wobble analogique |

Les 16 cases sont maintenant toutes actives (le repère "pas encore" phase-2 a été retiré
une fois la phase 2 implémentée — voir plus bas).

**Mécanisme** (`fxPunchEnter()`/`fxPunchApply()`/`fxPunchRevert()`/`fxPunchExit()`,
`main.cpp`) : à l'appui sur une des 16 cases, l'état courant (`active`+`params[4]`) du slot
`fxList[]` réutilisé est **snapshotté**, puis écrasé par les réglages du punch ; au
relâchement, l'état snapshotté est restauré **exactement**, qu'il ait été inactif ou déjà
actif indépendamment (ex. puncher BITCRUSH pendant que REP était déjà allumé en permanence
— au relâchement, REP retrouve exactement son propre réglage). **Piège trouvé et corrigé** :
REP(8)/BITCRS(9) pilotent tous les deux le MÊME wavefold global
(`audioSetWavefold()`), et RES(13)/DELAY(5) le MÊME moteur d'écho partagé
(`audioSetDelay()`) — après avoir restauré le slot punché, le sibling actif (s'il y en a
un) est réappliqué en plus, sinon son propre réglage se retrouverait écrasé par le
"retour à off" du slot punché. Vérifié par test headless (REP actif en permanence, punch de
BITCRUSH, relâchement : `rep.active` reste vrai).

**Bouton déclencheur variable selon le mode** (`fxTriggerBtn()`) — piège découvert en
cours de route (une 1ère hypothèse de "B2 partout" était fausse) : le bouton qui ouvre
`OVERLAY_FX` normalement n'est **pas le même partout** (selon quels boutons sont déjà pris
par les 3 autres fonctions du mode) :
- **B1** (SYNTH, POKEMON, I303, STONE, OMNI, OMNI2, MODULAR, FELU, AMBIENT, GRANULAR2)
- **B2** (CRUNCH, DJ, SAMPLE, DRUM2, DRUMS2, DRUMS3, DR2, GROOVE, EUCLI, 303S2, BREAK, GEST,
  303S, SS2, SYSEQ, MOD2)
- **B3** (NOOB, NOOB2)

mappage établi en parsant programmatiquement le second `switch(currentMode)` de
`handleButton()` (pas en relisant à l'œil — trop d'erreurs la 1ère fois, voir
`structure/HANDOFF.md`). Le hold-detect générique est câblé différemment pour le groupe B1
que pour B2/B3 : **B1 a déjà** un mécanisme d'appui long à 600ms (`btn1PressTime`/
`btn1Handled`, bascule le menu principal) — un second minuteur de hold indépendant sur le
MÊME bouton physique se serait fait concurrence ; le hold-punch pour B1 **s'intègre** donc
dans ce mécanisme existant (seuil `FX_PUNCH_HOLD_MS`=350ms, plus court que 600ms, pose
`btn1Handled=true` pour que le poll à 600ms voie son garde `!btn1Handled` déjà retombé).
Pour B2/B3, un minuteur dédié (`s_fxBtnPressMs`/`s_fxBtnLongFired`) est posé **sans**
`return` sur l'appui — laisse tourner normalement la logique court-clic/double-clic propre
à chaque mode (GROOVE a un B2 déjà à double fonction : clic simple=FX, double-clic=Scale/
Arp) — le poll dans `loop()` **écrase** ensuite `s_overlay` par `OVERLAY_FX_PUNCH` une fois
le seuil de hold dépassé, sans jamais empêcher le court-clic/double-clic normal de s'exécuter
d'abord. Vérifié par test headless bout-en-bout (appui+hold+relâchement réel via
`handleButton()`, 4 modes couvrant les 3 groupes de bouton + GROOVE) : ouverture/fermeture
correctes partout, **double-clic GROOVE confirmé intact** (`s_overlay==OVERLAY_SCALE_ARP`
après 2 clics rapides, non avalé par le hold-detect).

Pendant que l'overlay est ouvert, les touches de la grille en colonnes 0-3 ne jouent **pas**
de note (contrairement à `OVERLAY_FX` qui le permet exprès) — comportement de Sloop lui-même
(« Keys pressed while FX is held never play or record notes »), et évite toute confusion
entre "jouer une note" et "tenir une touche punch" avec la même main.

Vérifié (phase 1) : build simulateur propre, 4 des 8 effets testés audio non-silencieux/non
saturés par tap RMS/pic temporaire, restauration post-relâchement vérifiée pour les 8,
capture d'écran de la grille (16 labels identiques à l'ordre Sloop, case tenue en surbrillance).
Build ESP32 propre, RAM/flash quasi inchangés.

#### ⚠️ Révision — fixes colonnes 3-4 (ALIAS trop faible, LP/HP SWEEP/PHONE écrasés) + légende LED

Rapport utilisateur dans une session suivante : « les 2 premières colonnes de FX punch ne
fonctionnent pas [...] les 2 autres fonctionnent, mais les effets n'impactent pas trop le
son (gate, echo et wobble fonctionnent bien, mais les autres ne fonctionnent pas) [...] je
veux qu'à l'activation de ce mode, l'affichage LED change ». Les "2 premières colonnes"
étaient en fait les 8 cases phase-2 (voir leur propre sous-section plus bas, implémentées
dans la foulée) ; les "autres [qui] ne fonctionnent pas" en colonnes 3-4 avaient deux causes
distinctes, trouvées par lecture de code (pas de bug runtime caché) :

1. **ALIAS quasi inaudible** : seuil de wavefold `0.85` → gain≈1.18x, trop proche de 1.0
   pour se lire comme un effet. Remplacé par `0.08` → gain≈12.5x (même esprit que le gain
   délibérément extrême de BITCRUSH).
2. **LP_SWEEP/HP_SWEEP/PHONE silencieusement écrasés** — deux statics locales à
   `applyFxEffect()`'s FILT case (`lastFiltTyp`/`filtResByTyp[]`, mémoire de Res *par Typ*
   pour ne pas surprendre l'utilisateur en changeant de Typ à la main) écrasaient la Res que
   `fxPunchApply()` venait juste de poser, dès que le Typ du punch différait de celui actif
   avant. Même souci pour `lpfSmoothCut` (lissage anti-zipper du cutoff, convergence
   50%/10ms) : non resynchronisé au punch, le cutoff réel mettait ~100-200ms à se mettre en
   place au lieu de s'appliquer tout de suite. **Fix** : les deux statics promues en portée
   fichier (`s_lastFiltTyp`/`s_filtResByTyp[]`), pré-synchronisées explicitement dans
   `fxPunchApply()` *et* `fxPunchRevert()` avant d'appeler `applyFxEffect()`. Vérifié par
   test headless avec précondition reproduisant exactement le bug (FILT pré-réglé
   manuellement sur un Typ différent de celui du punch, comme un utilisateur l'aurait fait
   avant de puncher) : Res/cutoff restent désormais ceux voulus par le punch au lieu de
   retomber à la valeur générique par-Typ, et le revert restaure fidèlement l'état
   pré-punch dans tous les cas.
3. **Légende LED** : `updateLedsAndShow()` affiche désormais, dès l'ouverture de
   `OVERLAY_FX_PUNCH`, une couleur distincte par touche sur les colonnes 4-7 (blanc =
   tenue, teinte par `opt*12` sinon), colonnes 0-3 éteintes (non utilisées par cet overlay)
   — ajouté en early-exit avant le `switch(currentMode)` normal de la fonction, qui est
   désormais gardé par `s_overlay != OVERLAY_FX_PUNCH` pour ne pas écraser la légende juste
   après l'avoir posée.

---

### FX Punch phase 2 — moteur de capture ring-buffer (LOOP 1/4-1/32, STUTTER, REVERSE, TAPESTOP, HALFSPD)

Implémenté sur demande explicite dans une session suivante (confirmée via question : scope
complet d'un coup plutôt qu'un sous-ensemble). Les 8 effets "looper" n'ont aucun FX
`fxList[]` à réutiliser — ils lisent l'historique récent du mix final à la place.

**Architecture** (`lib/AMY Synthesizer/src/amy.c`/`amy.h`, bloc juste avant
`amy_fill_buffer()` — seul point de rendu partagé par l'ESP32 et le simulateur, donc le
moteur marche identiquement sur les deux cibles sans code spécifique à l'un ou l'autre) :

- **`amy_punch_cap_buf`** : anneau de 2s (`AMY_PUNCH_CAP_SECONDS`) continuellement rempli
  avec le mix final post-FX (exactement ce qui part vers le DAC), capturé à chaque bloc dans
  `amy_punch_capture_block()` — jamais interrompu, même pendant qu'un punch écrase la
  sortie, pour que l'historique reste frais et que le relâchement reprenne le direct sans
  accroc (le rendu live — oscillateurs, FX de bus — n'est JAMAIS mis en pause ; seul ce qui
  est copié dans `output_block[]` pour la sortie change).
- **`amy_punch_loop_buf`** : copie figée séparée (même taille), utilisée uniquement par
  LOOP/STUTTER — évite qu'une boucle tenue plus longtemps que les 2s de capacité de
  l'anneau principal ne se fasse corrompre par l'écriture continue qui finirait par
  repasser sur les échantillons en train d'être rejoués.
- **Mode `FIXED`** (LOOP 1/4, 1/8, 1/16, 1/32, STUTTER) : fenêtre figée à l'appui
  (`amy_punch_loop_start(len_frames)`), durée = note musicale au BPM courant pour les LOOP
  (même convention que GATE1/16 : 1/4=60/bpm s, 1/8=30/bpm s, 1/16=15/bpm s, 1/32=7.5/bpm s)
  ou ~60ms fixes non synchronisés au tempo pour STUTTER (glitch volontairement distinct
  d'une subdivision musicale). Relecture en boucle exacte (vitesse 1.0, pas d'interpolation).
- **Mode `RATE`** (REVERSE=-1.0, HALFSPD=0.5, TAPESTOP=1.0 décroissant exponentiellement
  vers 0 sur ~1.2s comme un moteur de bande qui s'arrête — `powf(0.01, 1/frames)` par
  frame) : lecture à position fractionnaire avec interpolation linéaire directement dans
  l'anneau vivant (`amy_punch_reverse_start()`/`amy_punch_halfspeed_start()`/
  `amy_punch_tapestop_start()`). Clamp de sécurité : le curseur de lecture ne peut jamais
  tomber à plus de `capacité - 1 bloc` derrière le curseur d'écriture, sinon une tenue très
  longue à vitesse lente/négative finirait par lire des échantillons en train d'être
  recyclés par l'écriture continue.
- **Allocation PSRAM explicite** : les deux buffers (~700KB total) sont alloués via
  `malloc_caps(..., MALLOC_CAP_SPIRAM)` (sur `ESP_PLATFORM` ; `MALLOC_CAP_DEFAULT` ailleurs)
  — **pas** via `amy_global.config.ram_caps_delay`, qui existe déjà pour les lignes de délai
  d'AMY mais vaut `MALLOC_CAP_DEFAULT` (RAM interne) sur cette carte, pas
  `MALLOC_CAP_SPIRAM` comme sur TULIP/AMYBOARD. S'y fier aurait tenté d'allouer ~700KB dans
  les ~124KB de DIRAM libre et aurait échoué/planté sur matériel réel.
- **Wiring** (`main.cpp`) : `punchKindIsLooper()` route les 8 nouveaux `PunchKind`
  (`PUNCH_LOOP14/18/116/132/STUTTER/REVERSE/TAPESTOP/HALFSPD`) directement vers
  `amy_punch_*()` dans `fxPunchApply()`/`fxPunchRevert()`, en contournant entièrement
  `fxList[]`/le mécanisme de snapshot (rien à sauvegarder, aucun FX impliqué).

**Vérifié par test headless** (les 8 effets) : tenir une note, laisser ~800ms d'historique
se capturer, appliquer le punch PUIS couper la note en direct (le direct devient
silencieux) — si le punch marche vraiment, le son doit continuer malgré la coupure en
direct (preuve qu'il rejoue bien l'historique figé, pas juste le direct qui passerait à
travers). Confirmé pour les 8 : RMS reste non-nul pendant toute la tenue malgré la note
coupée en direct, TAPESTOP montre une décélération nette vers le quasi-silence en fin de
tenue (signature de son ralentissement exponentiel), et chaque relâchement retombe
proprement à ~0 (direct repris, silencieux puisque la note avait été coupée). Build
ESP32+simulateur propres ; RAM statique ESP32 inchangée (38.8%) — confirme que les buffers
vont bien en PSRAM dynamique, pas en tableaux statiques DIRAM qui auraient fait exploser le
rapport de build.

#### ⚠️ Révision — vraie cause racine de LP/HP SWEEP, PHONE, BITCRUSH + activation plus rapide

Rapport utilisateur dans une session suivante : « les FX punch fonctionnent bien, sauf LP
et HP sweep, et phone et bitcrush [...] je veux qu'il s'active plus rapidement, que ça
switch vers FX punch et vers FX normal si je relâche l'appui, que ça reste sur FX punch si
je maintiens appuyé ». Le fix de la révision précédente (pré-synchro Res/cutoff) n'était
qu'une partie du problème — **la vraie cause racine, trouvée par lecture de code** :

`amy_apply_ordered_fx()` (`amy.c`) n'appelle `amy_apply_filt()`/`amy_apply_wavefold_bus0()`
que pour les slots présents dans `fxOrderList[]` — un tableau **séparé** de
`fxList[idx].active`, rempli **uniquement** par `fxOrderAdd()`, qui n'est lui-même appelé
que depuis `overlayFxToggle()` (le menu FX normal). `fxPunchApply()` posait
`fxList[idx].active = true` et tous les bons paramètres, mais **n'appelait jamais
`fxOrderAdd()`** — donc tant que l'utilisateur n'avait pas indépendamment activé FILT(0) ou
BITCRS(9) au moins une fois via le menu normal dans la session, punch LP_SWEEP/HP_SWEEP/
PHONE/BITCRUSH posait tout le bon état mais le DSP ne tournait **jamais** sur le buffer —
silencieusement, sans crash. ALIAS(REP, idx8) partage le même mécanisme de wavefold que
BITCRS, donc si l'utilisateur avait par hasard REP indépendamment actif ce jour-là, ALIAS
"marchait" par coïncidence alors que BITCRUSH restait cassé. GATE(TREMOLO)/ECHO(RES)/
WOBBLE(FLANGER) ne sont jamais passés par `fxOrderList` (tick 10ms dédié / position fixe
AMY-native) — d'où leur immunité totale à ce bug.

**Fix** : `fxPunchApply()` appelle maintenant `fxOrderAdd(idx)` si le slot n'était pas déjà
actif (donc pas déjà dans `fxOrderList`), et `fxPunchRevert()` fait le `fxOrderRemove(idx)`
symétrique — seulement si c'est le punch lui-même qui l'avait ajouté, pour qu'un slot que
l'utilisateur avait réellement allumé via le menu normal reste allumé après le punch.
Vérifié par test headless direct sur l'état global (`fxOrderList[]`/`fxOrderCount`, pas une
inférence via RMS) : LP_SWEEP et BITCRUSH ajoutent puis retirent bien leur slot ; un FILT
pré-allumé manuellement avant le punch n'est ni dupliqué dans la liste ni éteint au
relâchement.

**Activation plus rapide + bascule hold/release** : `FX_PUNCH_HOLD_MS` 350ms → 120ms.

#### ⚠️ Révision finale — appui = punch instantané, relâchement classifie quick/hold

Itérations successives sur l'interaction du bouton déclencheur, chacune sur retour
utilisateur direct :

1. 1ère itération : `fxPunchExit()` (relâchement après un hold) basculait vers `OVERLAY_FX`
   (menu normal) au lieu de fermer l'overlay. Corrigée : « après un appui long sur FX, on
   revient au principal, le seul moyen d'accéder au menu bloquant des FX est un appui
   court » → `fxPunchExit()` va à `OVERLAY_NONE` (écran principal), le clic court (bascule
   immédiate du menu FX normal dès l'appui, inchangée) reste la seule porte d'entrée du
   menu normal.
2. 2e itération (design final) : « dans le cycling des menus de FX, je veux que le menu de
   FX normal n'apparaisse que si je relâche rapidement le bouton FX, de sorte à que dès
   que j'appuie sur FX, ce soit le menu FX punch ». Le clic-court-ouvre-FX-normal-dès-
   l'appui (qui existait dans CHAQUE dispatch par mode, un `if(btn==N){...s_overlay=
   OVERLAY_FX;...}` par mode) est remplacé par un bloc **unique et générique** dans
   `handleButton()`, placé avant le `switch(currentMode)` par-mode et qui `return`
   immédiatement pour le bouton déclencheur FX de ce mode :
   - **Appui** : `fxPunchEnter()` immédiatement (`OVERLAY_FX_PUNCH` sans délai).
   - **Relâchement** : ~~classification par la durée tenue (`< FX_PUNCH_HOLD_MS`)~~ —
     **remplacé**, voir « Révision — sortie du menu FX par appui court » plus bas : la
     décision se fait maintenant sur *ce qui a été joué* pendant l'appui, pas sa durée.
   - **`MODE_GROOVE` B2 exempté** : son dispatch décide déjà single-click=FX vs double-
     click=Scale/Arp sur l'appui lui-même (fenêtre de 350ms) — un punch instantané et
     inconditionnel casserait silencieusement ce double-clic. Garde son ancien mécanisme à
     délai (le poll dans `loop()` qui appelle `fxPunchEnter()` une fois le seuil de hold
     dépassé, désormais restreint à ce seul mode/bouton).

**Plusieurs touches FX Punch tenues en même temps** (« je veux pouvoir appuyer sur
plusieurs FX punch en même temps ») : `s_fxPunchActiveOpt` (un seul opt, `int8_t`) devient
`s_fxPunchHeld[16]` (un `bool` par touche de la grille). Pour les 8 effets phase-1
(`fxList[]`), le snapshot/`fxOrderAdd()` n'est plus par-opt mais **par-slot**, avec
comptage de référence (`FxPunchSlotState{refCount, snapActive, snapParams[4]}` ×
`FX_COUNT`) — plusieurs opts peuvent viser le MÊME slot (ALIAS/BITCRUSH partagent déjà le
gain de wavefold global ; deux sweeps viseraient tous les deux FILT/idx0) : le snapshot et
le `fxOrderAdd()` ne se font qu'à la 1ère touche qui touche ce slot (`refCount` 0→1), et la
restauration/`fxOrderRemove()` qu'à la dernière relâchée (`refCount` →0) — entre les deux,
chaque nouvelle touche sur le même slot écrase simplement les params de la précédente
("dernier écrit gagne", déjà le comportement accepté pour ALIAS/BITCRUSH). Les 8 effets
phase-2 "looper" (`amy_punch_*()`) restent un moteur **singleton** — une 2e touche looper
pendant qu'une 1ère est déjà tenue est simplement ignorée (pas de sens musical à tenir
REVERSE et HALFSPD en même temps de toute façon). Le tick de rampe LP/HP SWEEP (`loop()`)
itère maintenant sur toutes les touches tenues au lieu d'une seule.

Vérifié par test headless : séquence appui→(aucun tick)→vérification instantanée de
`OVERLAY_FX_PUNCH`, appui court→`OVERLAY_FX`, appui tenu→`OVERLAY_NONE`, confirmation que
`MODE_GROOVE` garde son comportement d'origine (simple clic→FX, double-clic→Scale/Arp,
**pas** de punch instantané) ; GATE+WOBBLE (slots différents) actifs simultanément,
relâchement de l'un sans affecter l'autre ; LP_SWEEP+HP_SWEEP (même slot FILT) —
`refCount` 0→1→2 à l'application des deux, puis 2→1 (slot **toujours** actif/dans
`fxOrderList`, l'autre touche le tient encore) au relâchement du premier, puis 1→0
(restauration complète) au relâchement du second.

#### ⚠️ Révision — sortie du menu FX par appui court

Rapport utilisateur sur carte : « pour rentrer dans les FX, un appui court suffit, mais
pour en sortir, il faut que je fasse un appui long ». **Cause** : le relâchement court
(`< FX_PUNCH_HOLD_MS`) posait `OVERLAY_FX` **inconditionnellement** — y compris quand le
menu FX était déjà ouvert à l'appui. Depuis le menu FX, un appui court faisait donc
FX → punch (à l'appui) → FX (au relâchement) : aucune sortie possible sans tenir plus de
120ms, et un appui humain « court » chevauche souvent ce seuil, d'où l'impression d'aléa.

**Règle actuelle** (bloc générique en tête de `handleButton()`) — la durée ne compte plus :
- À l'appui : `s_fxBtnFromFx` mémorise si `OVERLAY_FX` (ou son sous-éditeur
  `OVERLAY_FX_MOD`) était ouvert ; `s_fxBtnPunched` est remis à faux ; `fxPunchEnter()`.
- `fxPunchApply()` pose `s_fxBtnPunched = true` dès qu'une touche punch est jouée.
- Au relâchement : revert de tout ce qui est tenu, puis
  - un punch a été joué → `OVERLAY_NONE` (écran principal, inchangé) ;
  - aucun punch → **bascule** : `OVERLAY_FX` si le menu était fermé, `OVERLAY_NONE` s'il
    était ouvert.

`FX_PUNCH_HOLD_MS` ne sert plus qu'au poll à délai de `MODE_GROOVE` B2 (non concerné :
son dispatch bascule déjà le menu FX dès l'appui). Effet de bord assumé : la grille punch
s'affiche brièvement pendant l'appui qui ferme le menu FX (nécessaire pour pouvoir punch
depuis n'importe quel écran).

Vérifié : test headless au boot (SYNTH/B1, DJ/B2, NOOB/B3 × 7 séquences — appuis 60 ms
et 300 ms, avec/sans punch, depuis l'écran principal et depuis le menu FX : 21/21
conformes), puis **sur carte** par log série (ouverture 114 ms → FX, fermeture 136 ms →
NONE) et confirmation du propriétaire dans plusieurs modes.

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

## MODE_AMBIENT — Synthé drone/pad (inspiré de Casual Machines "Slide Synth Lab")

Nouveau mode, catégorie INSTR. Demande utilisateur suite à la découverte du site
`casualmachines.co/lab` — un éditeur de patch web pour un synthé hardware réel ("Slide
Synth"), dont le moteur DSP est compilé en WebAssembly (non portable/inspectable) ; seul le
schéma de paramètres de patch, lisible dans le JS non-compilé du site, a servi de référence
de design (oscillateurs détunés, position de wavetable qui dérive toute seule, chorus+reverb
intégrés au patch).

**Réutilise intégralement le moteur de MODE_MODULAR, sans aucun changement dans
`audio_engine.cpp`** — mêmes canaux (`MOD3_OSCA_CH`/`MOD3_OSCB_CH`), mêmes fonctions
`audioModular*()`. Les deux modes sont mutuellement exclusifs (un seul `AppMode` actif à la
fois), donc partager les canaux ne coûte rien. Seul un état UI dédié change (`amb*` dans
`main.cpp`, miroir des `mod*` de MODE_MODULAR) avec des réglages par défaut différents :

- **Reverb+Chorus activés automatiquement à l'entrée** (`fxList[2]`/`fxList[3]`, via la même
  séquence que `overlayFxToggle()` — `active=true`, `fxOrderAdd()`, `applyAllFx()`) :
  premier mode du projet à modifier l'état FX global à l'entrée (les FX restent actifs si on
  change ensuite de mode, comme tout autre toggle FX manuel). Vérifié par test headless
  (tap RMS/pic sur `libminiaudio-audio.c`) : queue ~3.8× plus forte avec FX qu'sans, sur le
  même accord.
- **Enveloppe `ENV_PAD`** appliquée à l'entrée (500/100/2000ms, sustain 0.8 — déjà présente
  dans `envTable[]`, pas de nouvelle constante) au lieu de l'enveloppe par défaut plus courte
  de MODE_MODULAR.
- **Dérive de wavetable active par défaut** : `gModSlots[4]` (le même slot LFO réservé que
  MODE_MODULAR, cible `MODDEST_MOD_WTPOS_B`) démarre avec une profondeur non nulle
  (`ambLfoDepth=0.35`) au lieu de 0 — audible immédiatement, pas besoin de tourner P7.
  **Piège évité** : `modSlotApplyDirect()`/`modSlotRestore()` lisaient en dur `modOscBPos`
  (la variable de MODE_MODULAR) — comme ce slot est maintenant partagé par deux modes ayant
  chacun leur propre position de base d'oscillateur B, les deux fonctions ont été rendues
  sensibles au mode courant (`currentMode==MODE_AMBIENT ? ambOscBPos : modOscBPos`) pour lire
  la bonne base.
- **Détune d'oscillateur B plus large** (`ambDetuneSemisB`, défaut 0.25 demi-tons contre
  0.08 pour MODE_MODULAR), réglable au joystick (voir ci-dessous) — pad plus manifestement
  détuné/riche.
- **Mode drone/latch** (double-clic B4, `checkDoubleClick()`) : une pression **bascule** la
  note (elle reste active jusqu'à la pression suivante sur la même touche) au lieu de
  s'éteindre au relâchement physique — permet de superposer des accords qui gonflent sans
  garder les touches enfoncées. Réutilise `activeNotes[][]` (déjà la grille générique "quelle
  touche sonne" du projet) plutôt qu'un nouveau tableau de latch dédié. Nécessite un cas LED
  dédié (`updateLedsAndShow()`) : le cas générique `default:` n'éclaire que les touches
  *physiquement* tenues (`keyState[][]`), ce qui aurait rendu invisibles les notes en drone.
- **Joystick** : JX parcourt 6 éléments (OscA, OscB, Résonance, Enveloppe, LFO, Détune — un
  de plus que MODE_MODULAR, le détune, faute de potard libre — P2/P4-P7 sont déjà tous
  utilisés comme dans MODE_MODULAR), JY modifie l'élément focus. Le même vibrato de hauteur
  piloté par LFO que MODE_MODULAR (`audioModularSetPitchBend`) tourne en parallèle de la
  dérive de wavetable, sur les mêmes `ambLfoRate`/`ambLfoDepth` — gratuit, même mécanisme.

**Bug pré-existant découvert (et corrigé au passage, pas spécifique à ce mode)** : le
libellé de l'onglet actuellement sélectionné dans la bande d'onglets (MODE_MODULAR *et*
MODE_AMBIENT partagent ce widget) s'affichait comme du bruit illisible — `drawBox()` (fond
plein) suivi de `drawStr()` sans inverser `setDrawColor`, donc le texte se dessinait dans la
même couleur que son propre fond. Fix (les deux occurrences) : `setDrawColor(0)` avant le
`drawStr` de l'onglet sélectionné, `setDrawColor(1)` après — même idiome déjà utilisé par
`OVERLAY_INSTR` pour sa liste de catégories.

**Arpégiateur intégré au bouton ENV (B2)** — demande utilisateur : « le bouton de ENV
puisse permettre également de régler un arpègiateur ». Le moteur d'arp existant
(`arpMode`/`arpNotes[]`/`arpNoteCount`/`arpIdx`/`arpCurrent`) n'était câblé que pour
SYNTH/I303/SYSEQ, avec capture de notes dupliquée par mode dans `handleNoteKeyAudio` et un
dispatch dans `loop()` qui supposait `SYNTH_CH` (sauf le cas I303 explicite). Étendu à
AMBIENT sans dupliquer `OVERLAY_SCALE_ARP` (qui aurait aussi importé la sélection de gamme,
non demandée) : `OVERLAY_ENV` passe en grille 4 colonnes **uniquement quand
`currentMode==MODE_AMBIENT`** (tous les autres modes consommateurs d'`OVERLAY_ENV` gardent
leur grille 2 colonnes d'origine), libérant les opts 5-8 pour Up/Dn/UD/Rnd (même convention
clic-sur-la-même-valeur-pour-désactiver qu'`OVERLAY_SCALE_ARP`). `handleNoteKeyAudio` :
quand `arpMode!=0`, les touches nourrissent `arpNotes[]` au lieu de sonner directement ou
de latcher (l'arp est prioritaire sur le mode drone — les notes tenues nourrissent l'arp,
le drone concerne spécifiquement le fait de ne PAS tenir les touches). `loop()` : nouvelle
branche `MODE_AMBIENT` dans le dispatch du tick (`audioModularNoteOn/Off` au lieu de
`audioNoteOn/Off`/`playI303On/Off`).

**Bug corrigé au passage** : le sélecteur d'enveloppe d'`OVERLAY_ENV` n'appelait
`audioModularSetEnvelope()` que pour `MODE_MODULAR` — en `MODE_AMBIENT`, changer de preset
d'enveloppe ne touchait donc jamais le son réel (seul l'appel mort `audioSetEnvelope()`/
`SYNTH_CH` se déclenchait). Étendu aux deux modes.

Hors périmètre (volontairement, pour une première version bornée) : le schéma à 4
oscillateurs librement assignables du site de référence, le "scan" automatique de position
de wavetable à fréquence réglable, le transitoire de bruit et le "pop" de hauteur au
déclenchement — le moteur à 2 oscillateurs fixes de GrvEP capture l'essentiel du caractère
"pad ambiant" sans nouveau canal ni refonte de `MOD3`.

### Notes aiguës beaucoup plus faibles que les graves — filtre sans keyboard tracking

Rapporté : « pour le mode AMBIANT, les notes aigües sont beaucoup plus faibles que les
notes graves ». Cause : `audioModularSetFilter()` (`audio_engine.cpp`, le setter de filtre
partagé par MOD3_OSCA_CH/MOD3_OSCB_CH — donc par MODE_MODULAR, MODE_AMBIENT, MODE_NOOB2 et
MODE_OMNI2, qui passent tous par cette même fonction via `modCutoff`/`ambCutoff`) ne posait
qu'un `filter_freq_coefs[COEF_CONST]` — une coupure fixe en Hz, **identique pour toutes les
notes**. Pour un preset à coupure basse (Sub=500Hz, Drone=800Hz), une note aiguë a son
fondamental/ses harmoniques bien plus proches de (ou au-dessus de) cette coupure fixe qu'une
note grave, donc le LPF24 (24dB/octave) les atténue beaucoup plus fort — sans lien avec le
volume réel de l'oscillateur, uniquement un effet de filtre non compensé.

Mesuré par tap RMS headless (même vélocité, note tenue 500ms, presets Sub/Drone/DeepPad/
Glass, notes C1→C7) : sur Sub, la RMS chute de **11281 (C2) à 414 (C7), soit ×27** entre
grave et aigu. Sur Drone (resonance 2.2, la plus élevée), le motif est irrégulier en plus
d'être descendant. DeepPad (coupure 1200Hz) et Glass (coupure 5500Hz, filtre quasi
transparent sur toute la tessiture testée) ne montraient quasiment pas l'effet — cohérent
avec l'hypothèse filtre.

Fix : AMY expose justement un **keyboard tracking de filtre natif**, `filter_freq_coefs[
COEF_NOTE]` — `combine_controls()` combine les coefficients en espace log-fréquence
(`filter_logfreq = COEF_CONST(Hz, converti en log) + COEF_NOTE × logfreq_for_midi_note(note)`,
où `logfreq_for_midi_note()` vaut 0 à la note MIDI 69/A4/440Hz). Mis à `1.0f` dans
`audioModularSetFilter()` : la coupure (la valeur `cutoffHz` passée, désormais comprise
comme "coupure à A4") suit maintenant la note jouée — elle double par octave montée, se
divise par deux par octave descendue — au lieu de rester fixe. Comme la fonction est
partagée, les 4 modes ci-dessus en bénéficient en un seul endroit.

Re-mesuré après coup : Sub passe de ×27 (11281→414) à un écart ×1,5 (965-1338 sur toute la
tessiture) ; DeepPad et Glass restent plats (déjà bons, légère amélioration). Drone (la
resonance la plus élevée) reste irrégulier — un filtre très résonant qui suit la note peut
toujours tomber sur des harmoniques différentes de la wavetable à différentes hauteurs ; ce
résidu est un effet de résonance+contenu harmonique, pas quelque chose qu'un simple keyboard
tracking peut entièrement aplatir, et n'était pas le symptôme rapporté (grave vs aigu de
façon générale, pas un preset précis).

---

## MODE_NOOB2 / MODE_OMNI2 — NOOB et OMNI sur le moteur ambiant + banque de presets

Demande utilisateur : « fait un noob2 et un omni2... se basant sur le synth ambiant... avec
plusieurs preset pour gérer tous les paramètres du synth ambiant ». Chaque mode duplique
l'intégralité du moteur de son homonyme (NOOB : flux génératif événementiel ; OMNI : grille
d'accords cercle des quintes + bande de strum) mais fait sonner le résultat via le moteur
`MODE_AMBIENT` (`audioModularNoteOn/NoteOff` sur `MOD3_OSCA_CH`/`MOD3_OSCB_CH`) au lieu de
`SYNTH_CH`. Rejoint le groupe de mutuelle-exclusion déjà utilisé par MODULAR/AMBIENT pour
`gModSlots[4]` (le slot LFO de dérive de wavetable) — `modSlotApplyDirect()`/
`modSlotRestore()` ont une nouvelle fonction `modUsesAmbientBase()` regroupant
AMBIENT/NOOB2/OMNI2 (tous les trois partagent le même état `amb*` via `ambApplyPreset()`).

**Banque de presets** (`AmbientPresetDef`/`kAmbientPresets[]`, `config.h`, modelée sur
`kMod2Algos[]` mais à champs nommés puisque les contrôles ambiants sont fixes, pas
assignés par slot P4-P7) : 6 presets curés (Glass/DeepPad/Drone/Shimmer/Sub/Chorus) posant
chaque champ `amb*` (tables d'onde, positions, filtre, LFO de dérive, détune, enveloppe).
`ambApplyPreset(idx)` (`main.cpp`, juste avant `switchMode()`) pousse un preset vers le
moteur — même schéma reset-puis-push que `mod2AlgoApply()`, sûr en cours de note (setters
allégés uniquement, jamais `audioModularOscInit`). **Raison d'être** : NOOB2/OMNI2 ont
leurs propres pots occupés (densité/rythme/forme/variation pour NOOB2 ; accord/strum pour
OMNI2) — aucun potard libre pour ré-exposer individuellement les contrôles du moteur
ambiant, donc P2 (qui choisissait un `SynthShape` dans les originaux) choisit un preset
ambiant à la place.

- **MODE_NOOB2** : duplique intégralement l'état de NOOB (`noo2*`, préfixé, y compris ses
  propres `NooVoice[]`/historique roulant/tables de gamme locales — seules les CONSTANTES
  `NOO_*`/`kNooVariation*` restent partagées, ce sont de pures tables de tuning) —
  `nooTriggerVoice()`/`nooAllNotesOff()` deviennent `noo2TriggerVoice()`/
  `noo2AllNotesOff()` appelant `audioModularNoteOn/NoteOff`. Écran/LED/pavé de gamme au
  clavier identiques à NOOB (piano-roll, sélection de gamme par touche).
- **MODE_OMNI2** : la bande de strum (oscillateurs bruts `AMY_OSC_STRUM+p`) reste
  **partagée** avec OMNI (mécanisme déjà indépendant du mode, pas dupliqué) ; seul l'état
  de sélection d'accord (`omni2Root`/`omni2Type`/`omni2ChordNotes[]`) est dupliqué pour
  éviter toute contamination croisée en changeant de mode.
- **Bug pré-existant corrigé** (rapporté ensuite comme « l'affichage rentre en collision ») :
  l'écran de `MODE_OMNI` (hérité tel quel par `MODE_OMNI2`) avait deux défauts, confirmés
  par test A/B contre le code MODE_OMNI original avant toute modification : (1) le titre
  (`drawStr(0,0,...)`) ne s'affichait jamais — `drawStr` prend un y de **baseline**, et le
  dernier `setFont` laissé actif par l'écran précédemment affiché (jamais réinitialisé en
  entrée de case) pouvait être une police assez haute pour pousser tout le glyphe au-dessus
  de la ligne 0, donc invisible ; (2) la bande FX (`slotW = 128/(int)FX_COUNT`, commentaire
  "=21" resté figé alors que `FX_COUNT` vaut maintenant 14, donc `slotW=9` en vrai) faisait
  se chevaucher les 14 étiquettes en un bloc de texte illisible. Fix : `setFont` explicite
  en tête de case + titre à `y=7` (même convention que MODE_I303 pour ce même 5x7) ; la
  bande de 14 cases remplacée par **une seule ligne** `FX n/14:NOM [ON/off]` — ne dépend
  plus du nombre de FX, ne peut plus se chevaucher. Appliqué identiquement aux deux modes.

### Boîte à rythme (OMNI / OMNI2)

Demande utilisateur : « une boite à rythme derrière les omni et omni2, que je peux play et
changer de type de rythme avec les boutons libre du clavier ». État **partagé** entre
MODE_OMNI et MODE_OMNI2 (`omniRhythm*`, `main.cpp`) — même rythme de fond quel que soit le
moteur de synthèse utilisé pour les accords/strum par-dessus.

- **5 patterns curés** (`kOmniRhythms[]`, Rock/HipHop/House/Latin/Break), 4 pistes par
  pattern (kick/snare/hat fixes + `perc`, dont le PAD varie par style — clap/ride/bongo —
  pour un vrai timbre distinct, pas juste un timing différent), déclenchés via
  `audioPlayDrumPad()` (le même mécanisme déjà partagé par DRUM2/GROOVE/EUCLI/CRUNCH).
  Horloge 16ᵉ de note (`60000/bpm/4`), avec un **swing par pattern** façon MPC (les pas
  impairs arrivent en retard, les pairs en avance, de la même quantité — la moyenne sur une
  paire reste au tempo nominal).
  **Corrigé** (rapporté : « les différentes boite à rythme se ressemble énormément ») — la
  première version partageait la MÊME frappe de caisse claire (4,12) et parfois le même
  charleston sur les 5 styles, ce qui domine la perception d'ensemble : re-conçu pour que
  chaque style varie clairement caisse claire (Break a des ghost notes syncopées au lieu
  d'un backbeat plat), présence/absence de charleston (Latin n'en a pas du tout), la
  piste perc (timbre différent par style) et le swing (HipHop/Latin swingués, les autres
  droits).
- **B3** (libre dans les deux modes — ni OMNI ni OMNI2 ne l'utilisaient) bascule
  play/stop.
- **Les 5 touches libres de la grille d'accords** (rangée 0, colonnes 3-7 — les colonnes
  0-2 de cette rangée sont déjà les accords de B) choisissent le pattern, changement à
  chaud sans réinitialiser le pas courant (comme le bouton « style » d'une vraie boîte à
  rythme). Rendu à l'écran dans la même rangée de cases que les accords B, et l'état
  (nom du pattern + indicateur `>` si en lecture) apparaît sur la ligne d'en-tête.
- Réinitialisé (stoppé) à chaque changement de mode (`switchMode()`, même ligne
  inconditionnelle qui réinitialise déjà `omniRoot`/`omniStrumPos`) — le TYPE choisi
  persiste en revanche (pas remis à zéro), donc rallumer B3 reprend le même style.
- **Bug corrigé** (rapporté : « je veux que quand je quitte les omni, le son s'arrete ») :
  `audioAllNotesOff()` (appelée sans condition à chaque changement de mode) coupe déjà
  SYNTH_CH/le strum/le preview, mais ne touchait PAS la plage `DRUM_OSC_BASE` — un coup de
  batterie encore en train de décroître au moment de quitter OMNI/OMNI2 continuait donc de
  sonner dans le mode suivant. Fix : les oscillateurs kick/snare/hat de la banque courante
  + les 3 pads perc possibles (BNG/CLP/RDE) sont coupés explicitement (`velocity=0`) dans
  le hook de sortie d'OMNI/OMNI2.

### Banque de batterie (OMNI / OMNI2) — remplace le "FX select"

Demande utilisateur : « je n'aime pas le FX select de omni, retire le pour le remplacer
par un bank pour la batterie ». **B2** ne cycle plus `fxSelected` (c'était redondant avec
B1, qui ouvre déjà `OVERLAY_FX` — la façon standard de choisir/activer un FX dans tous les
autres modes) — il cycle maintenant `omniDrumBank` parmi 4 kits (`kOmniDrumBanks[]`,
`main.cpp`), chacun un jeu différent de samples kick/snare/hat parmi ceux déjà chargés
(`kDrumPads[]` : KK1/KK2/KK3, SN1/SN2/SN3/SNB, HH1/HH2). La piste `perc` de la boîte à
rythme (clap/ride/bongo) reste liée au PATTERN, pas à la banque — c'est la couleur du
style, pas le choix de kit. B1/P4-P7 continuent de gérer les FX exactement comme partout
ailleurs (B1 ouvre la grille FX, P4-P7 éditent l'effet qui y est sélectionné) ; seul le
raccourci de cycle au B2 disparaît. La banque choisie persiste elle aussi entre OMNI et
OMNI2 (même logique que le type de rythme).

**Bug corrigé** (rapporté : « selon les bank, certaines drums sont à contre temps ») — les
banques ne changent QUE le sample utilisé, jamais le placement rythmique (`kOmniRhythms[]`
reste indépendant de `omniDrumBank`), donc en théorie aucune banque ne devrait déphaser
quoi que ce soit. En pratique : ce sont des samples PCM bruts avec des durées de silence
de tête très différentes d'un sample à l'autre, donc déclencher `audioPlayDrumPad()` au
même pas programmé produit quand même des instants de départ audibles très différents.
Mesuré par un tap headless dédié (déclenchement depuis le silence, détection du premier
échantillon au-dessus d'un seuil) : **KK3 arrivait 165ms en retard** (quasiment un 16ᵉ de
note entier à un tempo courant — totalement inutilisable comme kick), HH1 ~54ms, SNB
~43ms, SN3 ~16ms, contre <10ms pour KK1/KK2/SN1/SN2/HH2. Fix : `kOmniDrumBanks[]` exclut
maintenant KK3 (le pire, de loin) et HH1 (utilisé comme charleston dans 2 des 4 anciennes
banques) ; Kit4 garde volontairement SNB (~43ms) comme caisse claire "plus lâche/sale" —
un choix de caractère borné et documenté, plus un accident non expliqué.

### INDUSTRIAL toujours trop fort après le premier passage — piège méthodologique

Voir la section `shapeGain[]` plus haut : le sweep headless original sautait l'attaque
(exactement le burst résonant qui fait le caractère d'INDUSTRIAL), sous-estimant son volume
réel de 2,4×. Corrigé (`0.82→0.34`) après re-mesure sur la note complète.

### NOOB/NOOB2 : accords trop forts (HOVR notamment) — polyphonie non compensée

Rapporté : « HOVR trop fort en mode NOOB ». `nooTriggerVoice()` envoyait une vélocité fixe
(`0.75f*volume`) quel que soit le nombre de notes déjà sonnantes, contrairement au clavier
de MODE_SYNTH qui divise la vélocité par `sqrt(nombre de notes tenues)`. Confirmé par tap
RMS/pic headless : un accord HOVR à 3 notes (vélocité plate, sans compensation) tapait
`peak=32767` (écrêtage numérique dur).

Fix : `nooTriggerVoice()`/`noo2TriggerVoice()` prennent un paramètre `chordSize` (fourni par
l'appelant — le nombre de notes de l'accord en cours de déclenchement) en plus d'un
`activeCount` recalculé sur les voix déjà actives dans `nooVoices[]`/`noo2Voices[]`. La
compensation utilise `effCount = max(activeCount, chordSize)`, pas `activeCount` seul : la
1ʳᵉ itération d'un piège en boucle appelant `nooTriggerVoice()` pour chaque note d'un
accord simultané compterait sinon la 1ʳᵉ note comme seule (ses notes sœurs ne sont pas
encore ajoutées à `nooVoices[]`), sous-atténuant exactement cette première note. Vélocité
finale : `0.75f * volume * (1/sqrt(max(1,effCount)))`. Vérifié de bout en bout via la
fonction de production réelle (pas une réimplémentation à la main) : un accord HOVR à 3
notes passe de `rms=20128/peak=32767` (non corrigé) à `rms≈13-17k/peak=32385` (ne tape plus
le plafond d'écrêtage). Même correctif appliqué à l'identique à `noo2TriggerVoice()`
(MODE_NOOB2 avait sa propre copie du même défaut).

### Presets ambiants mal calibrés entre eux — `AmbientPresetDef.gain`

Rapporté (MODE_OMNI2, moteur ambiant) : « certains instruments/notes grésillent un peu, ou
au contraire, sont trop faibles par rapport aux autres ». Les 6 `kAmbientPresets[]`
n'avaient jamais été calibrés en niveau les uns par rapport aux autres. Cause : un filtre
résonant **amplifie** l'énergie près de sa fréquence de coupure au lieu de seulement
atténuer au-dessus — un preset à coupure basse mais résonance modérée (Sub : 500Hz/1.2)
peut donc sonner plus fort, et plus proche de l'écrêtage, qu'un preset grand ouvert
(Glass : 5500Hz/1.0), à l'inverse de l'intuition « coupure basse = plus sourd = plus
calme ».

Mesuré par tap headless (même accord à 3 notes rejoué sur MODE_OMNI2 pour chacun des 6
presets, RMS/pic/nombre d'échantillons écrêtés) :

```
Glass:    rms=5767.2  peak=18417 clipSamples=0   cutoff=5500 reso=1.00
DeepPad:  rms=11019.5 peak=32767 clipSamples=56  cutoff=1200 reso=1.40
Drone:    rms=7437.0  peak=25490 clipSamples=0   cutoff=800  reso=2.20
Shimmer:  rms=7467.2  peak=31726 clipSamples=0   cutoff=6500 reso=1.00
Sub:      rms=12505.0 peak=32767 clipSamples=129 cutoff=500  reso=1.20
Chorus:   rms=10602.5 peak=32767 clipSamples=12  cutoff=3200 reso=1.10
```

Écart de loudness ×2,2 (Glass le plus calme, Sub le plus fort) et 3 presets sur 6
écrêtaient (DeepPad/Sub/Chorus). Fix : champ `gain` ajouté à `AmbientPresetDef` (`config.h`),
calibré sur le preset le plus calme (Glass=1.0, jamais boosté — politique d'atténuation
uniquement, même approche que `shapeGain[]`) : `DeepPad=0.52, Drone=0.78, Shimmer=0.77,
Sub=0.46, Chorus=0.54` (calculé `rms_Glass/rms_mesuré`, arrondi). Appliqué comme
multiplicateur de vélocité aux deux points de consommation d'un preset : l'accord de
MODE_OMNI2 (`chordVel = 0.5f * kAmbientPresets[...].gain`) et `noo2TriggerVoice()`
(MODE_NOOB2). Re-vérifié : RMS resserré à 5220-6957 (contre 5767-12505 avant),
`clipSamples=0` sur les 6 presets.

### MODE_OMNI2 : LFO de dérive remplacé par une enveloppe déclenchée à l'accord

Demande explicite (pas un bug) : « je n'aime pas trop le lfo à une fréquence faible pour ce
mode, je préfère une léger enveloppe qui fait évoluer l'accord quand on le joue, mais pas un
lfo qui produit une évolution un peu aléatoire ». MODE_OMNI2 partageait jusque-là le LFO
continu de dérive de wavetable `gModSlots[4]` avec MODE_AMBIENT/MODE_NOOB2 (via
`modUsesAmbientBase()`).

Fix, **MODE_OMNI2 uniquement** (MODE_AMBIENT et MODE_NOOB2 gardent le LFO d'origine,
inchangé) : `gModSlots[4]` désarmé (`ModSlot{}`, entièrement inerte) dans le hook d'entrée
de MODE_OMNI2. À la place, chaque pression d'accord enregistre `omni2ChordPressMs =
millis()` ; le tick 10ms calcule une enveloppe exponentielle décroissante
`envelope = exp(-(millis()-omni2ChordPressMs)/OMNI_ENV_TAU_MS)` (`OMNI_ENV_TAU_MS=550ms`,
soit ~3τ≈1,6s pour se stabiliser), pousse `MOD3_OSCB_CH` vers
`ambOscBPos + omniEnvDepth*0.8*envelope` via `audioModularSetWtPos()`, puis remet le
minuteur à 0 (sentinelle « rien à faire ») une fois `envelope<0.02`. Résultat : chaque
accord fait évoluer légèrement le timbre puis se stabilise sur la position de base du
preset, sans dérive libre entre les pressions. Vérifié par test headless : `gModSlots[4].
active=0` tout du long en MODE_OMNI2, et `omni2ChordPressMs` retombe bien à 0 après ~2,4s de
ticks suivant une pression d'accord (enveloppe stabilisée). La profondeur (`omniEnvDepth`,
fixée à `0.35` à l'origine) est devenue un potard réglable en direct — voir la section
suivante.

### Potards P5/P6/P7 (OMNI / OMNI2) — volume du beat, profondeur d'enveloppe, modulation

Demande utilisateur : « je veux que pour omni et omni2, les potards permettent également de
régler le volume du beat, l'impact de l'enveloppe sur les accords, et de moduler
l'instrument sélectionné si possible ». P5/P6/P7 (`pots[4..6]`) étaient jusque-là inertes en
dehors du rôle FX (ils ne servaient qu'aux paramètres 1-3 de l'effet sélectionné quand un FX
est actif — voir `fxPotDeltaApply`) : ils gagnent maintenant un second rôle, actif
uniquement **quand aucun FX n'est actif** (même convention déjà utilisée par P4/`pots[3]`
pour alterner entre "forme d'onde du strum" et "paramètre FX 0"), partagé à l'identique par
MODE_OMNI et MODE_OMNI2 :

- **P5 → `omniRhythmVol`** : multiplicateur de vélocité appliqué aux 4 pistes de la boîte à
  rythme (kick/snare/hat/perc) dans le même tick que `kOmniRhythms[]`. Mappé
  `pots[4].value*2.0` (0 = coupé, 1.0 au centre du potard = volume d'origine inchangé, 2.0 en
  butée = doublé), avec un plancher/plafond `constrain(...,0.05,1.3)` par frappe pour éviter
  le silence total ou l'écrêtage.
- **P6 → `omniEnvDepth`** : profondeur de l'enveloppe déclenchée à chaque pression d'accord.
  En MODE_OMNI2, réutilise le mécanisme existant (position de wavetable d'OscB, ci-dessus).
  En MODE_OMNI (moteur `SynthShape`/SYNTH_CH, sans paramètre de "position de timbre"
  générique), la même enveloppe pilote plutôt un **pitch bend** via `audioSetPitchBend()` —
  fonction déjà utilisée pour le pitch bend joystick de MODE_SYNTH, donc un primitive de mise
  à jour continue déjà éprouvée, et surtout **universelle** : contrairement à un balayage de
  cutoff (`audioSetFilterFreq()`), qui n'a aucun effet audible sur la majorité des 35
  `SynthShape` dont le `filter_type` natif est `FILTER_NONE`, le pitch bend fonctionne
  identiquement sur les 35 formes ET sur les patches Juno/DX7. `bendSemis =
  omniEnvDepth*4.0*envelope` (jusqu'à 4 demi-tons de creux à l'attaque, qui se résorbe avec
  la même constante de temps que l'enveloppe OMNI2).
- **P7 → `omniModAmt`** : modulation continue et lente de l'instrument sélectionné,
  **désactivée par défaut** (contrairement à l'ancien LFO `gModSlots[4]` toujours actif que
  le retrait pour OMNI2 visait justement à corriger — ici, rien ne bouge tant que l'utilisateur
  ne monte pas le potard). En MODE_OMNI2, un LFO sinusoïdal lent (~0,15Hz) s'ajoute à la même
  position de wavetable que l'enveloppe (`omniModAmt*0.3*sin(...)`) ; en MODE_OMNI, un
  vibrato de pitch bend (~0,18Hz, `omniModAmt*0.7*sin(...)`) combiné dans le même calcul de
  `bendSemis` que l'enveloppe ci-dessus (un seul appel `audioSetPitchBend()` par tick,
  gardé derrière un seuil de variation pour ne pas spammer d'événements identiques).

À l'entrée de MODE_OMNI/MODE_OMNI2, `pots[4..6]` sont resynchronisés explicitement
(`pots[4].value=0.5` → volume par défaut inchangé, `pots[5].value=pots[6].value=0` →
enveloppe/modulation éteintes tant qu'on n'y touche pas) — même motif déjà utilisé par
GRANULAR2/EXP pour leurs propres P4-P7 (`pots[4].value=0.5f` au besoin). `ctrlLabelsFor()`
affiche ces rôles ("BTVOL"/"ENV"/"MOD") à la place des tirets quand le FX est inactif.

Vérifié par test headless : volume du beat audiblement différent entre P5 bas et haut (RMS
1964→2914 sur le même pattern) ; l'enveloppe de pitch bend décroît en suivant exactement la
formule analytique `exp(-t/550ms)` (mesuré à +600ms et +1800ms, écart <1% avec le calcul) ;
la modulation continue produit un pitch bend non nul et variable dans le temps tant qu'aucun
accord n'est tenu, confirmant que le terme LFO est bien actif indépendamment de l'enveloppe.

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

### Colonne « réglage » (col 0) et mode tune — geste par piste

La touche de la colonne 0 d'une ligne (`col == 0`) ouvre le réglage de la piste
(`euTuneLane`) :
- **clic simple** : cycle le modificateur `euLaneMod[lane]` (`NONE → PROB50 → PITCHRAND →
  DOUBLE`, `EUCLI_MOD_*`) et entre en mode tune ; le mode tune met P4 = pitch (**amplitude
  aléatoire** quand le modificateur est PITCHRAND), P5 = decay, P6 = volume du pad de la
  piste, et le joystick X = phase (`euRotation`, décale le *placement* des coups, jamais les
  têtes de lecture qui restent alignées entre les 4 pistes) ;
- **double clic** (fenêtre `checkDoubleClick`) : **annule** le cycle du premier clic
  (restaure `euModBeforeClick[lane]`) et quitte le mode tune — l'utilisateur ne voulait pas
  que le double clic laisse une altération ;
- **appui long** (`EUCLI_LONGPRESS_MS`=500, sondé dans `loop()` comme la grille FX) : remet le
  modificateur, pitch, decay, volume et amplitude aléatoire de ce pad à `kDrum2Default*`.
- **RANDPITCH** : `pitch = kDrum2DefaultPitch[pad] ± rand(amp)` — centré sur le pitch **par
  défaut fixe** du pad, pas sur `drum2Pitch[pad]` (mutable, réglé en tune) : les pads peuvent
  avoir des pitchs par défaut différents.
- **Nombre de coups = proportion** : P4-P7 pilotent `euPulseProp[lane]` (0-1, accumulé par
  `rawDelta`), `euPulses = round(prop × pas)` recalculé aussi quand le nombre de pas change.
  L'ancien accumulateur d'entier absolu rendait certaines valeurs inatteignables selon le
  nombre de pas (sensibilité du potard dépendante de `euSteps`).
- Contrairement aux autres modes (DR2/GROOVE/CRUNCH, qui gardent P4-P7 sur le FX tant que
  n'importe quel FX est actif — boucle `anyFxActive`), EUCLI ne rend P4-P7 au FX que quand
  `s_overlay == OVERLAY_FX` (menu réellement ouvert) : sinon on ne pouvait plus régler les
  proportions après avoir quitté le menu FX. Exception délibérée, non étendue aux autres modes.

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

## MODE_BREAK — Générateur de breakbeat probabiliste (inspiré X0X/BB-Gen)

Construit **sur le moteur GRANULAR2** (réutilise son pool de voix dédié, pas un nouveau
moteur audio) : un sample SD est découpé en 8 tranches (slices) égales ; un générateur
MOVE/STAY probabiliste décide, pas après pas, s'il avance à la tranche suivante ou reste
sur la courante — la logique centrale qui donne au breakbeat généré son caractère
"glitché"/répétitif plutôt qu'une simple lecture linéaire du sample.

**La grille 4×8** est une **couche de performance live par-dessus** le générateur, pas le
générateur lui-même : colonne = tranche (0-7), ligne = variante de lecture appliquée à
cette tranche quand le générateur la joue (lecture normale / inversée / demi-vitesse /
stutter). Appuyer une touche de la grille n'enregistre donc pas une note classique — ça
arme/désarme une variante pour la tranche correspondante, appliquée la prochaine fois que
le générateur MOVE/STAY la sélectionne.

### Bug BPM=0 / bouton RSD-SYNC sans effet (rapporté et corrigé)

Rapporté par l'utilisateur : « je n'arrive pas à mettre BPM=0 pour synchroniser en
fonction de la taille du sample, de plus, le bouton RSD/SYNC n'a pas l'air d'avoir
d'effet ». Deux symptômes de la même famille de bug que celle documentée dans
`CLAUDE.md` (« Exact-equality thresholds and flat clamps near a parameter's own range
boundary are fragile ») :
- Le mapping potard→BPM n'avait pas de marge/zone de retenue près de 0 — descendre le
  potard au minimum physique ne landait pas fiablement sur l'exactitude `0` à cause de
  l'arrondi flottant de la courbe exponentielle potard→valeur, empêchant de déclencher le
  mode "synchronisé à la taille du sample" qui n'est actif qu'à `BPM==0` exactement.
- Le bouton RESYNC/SYNC pilotait un état qui n'était en réalité consulté par aucun chemin
  de lecture actif à ce moment-là (état mis à jour mais jamais relu par le tick de
  lecture tant que le mode n'était pas déjà dans la bonne configuration de BPM) — corrigé
  en même temps que le fix BPM=0, les deux bugs se masquaient l'un l'autre côté
  utilisateur (« le bouton n'a pas l'air d'avoir d'effet » parce que le BPM=0 qui active
  réellement son effet n'était jamais atteint).

### Visualisation de la jauge de hold/sync — piège UX

Lié au bug du popup potard circulaire (voir « UI : rangée B1-B4 + popup potards ») : un
contrôle physique divisé en deux moitiés logiquement indépendantes (ici, un
toggle BPM-sync dont la moitié "off" ne pilotait pas du tout le taux affiché) se lit pour
l'utilisateur comme « le potard est resté bloqué », pas comme un bug de logique — à
vérifier systématiquement sur tout contrôle dont la plage est divisée en sous-modes.

---

## MODE_FELU — Synthé inspiré de Felucca

Felucca (logiciel de référence externe) expose 13 "moteurs" de synthèse distincts. FELU
en réimplémente **9** en `SynthShape` AMY (pas un portage DSP littéral — chaque moteur est
approché avec les briques de synthèse déjà disponibles dans AMY, pas une reproduction bit
à bit de l'algorithme d'origine) :

| Code | Nom | Approche AMY |
|------|-----|---------------|
| ANLG | Analog | Oscillateurs classiques + filtre, type synthé analogique |
| WHEL | Wheel | — |
| TRIO | Trio | — |
| PHAS | Phase | Modulation de phase |
| FM6  | FM6  | FM multi-opérateurs |
| LOFI | Lo-Fi | Bitcrush/downsample |
| VOIC | Voice | Formants/synthèse vocale |
| PHYS | Physical | Modélisation physique (type Karplus-Strong) |
| NOIS | Noise | Bruit filtré |

(Les 4 moteurs Felucca restants n'ont pas d'équivalent AMY raisonnable et n'ont pas été
portés — pas de piège caché, juste hors scope.)

**Même schéma que MOD2** : chaque algo est un `SynthShape` sélectionné par un patch, plus
**4 macros P4-P7** génériques qui pilotent les paramètres les plus pertinents de l'algo
actif (le mapping macro→paramètre AMY change selon l'algo sélectionné, comme pour MOD2 —
pas de table de macros fixes partagée entre tous les algos).

`kFeluAlgos[]`/`FELU_ALGO_COUNT` ont été étendus de 4 à 9 au cours de cette session (les 5
derniers de la liste ci-dessus ajoutés après une première passe qui n'en couvrait que 4).
Le browser `OVERLAY_FELU_ALGO` (sélection d'algo) a été élargi en grille 4 colonnes
(`is4col`) pour accueillir les 9 entrées sans scroll excessif. Vérifié par test headless :
les 9 algos se chargent et jouent sans crash.

---

## MODE_DRUMS2 / MODE_DRUMS3 — kits batterie synthétisés (808/909)

Demande utilisateur : « je trouve que les drums sont un peu cheap, fait un mode DRUMS2 se
basant sur [...] X0X 808 et un DRUMS3 sur la 909 » — DRUM2 joue des échantillons PCM
courts (son jugé "cheap"/générique) ; ces deux nouveaux modes **synthétisent** chaque coup
à la volée via AMY (SINE/TRIANGLE/NOISE + filtre + enveloppes), aucun PCM. Scopé par
questions : pas de référence externe précise au-delà de "façon 808"/"façon 909", même
gabarit UI que DRUM2 (8 pads, vue PAD + vue SEQ, B1=Play B2=FX B3=Rec B4=Vue) mais
volontairement **trimé** par rapport à DRUM2 : pas de vue ANIM (sync GIF, sans rapport),
pas des modifiers prob/randpitch/double ni des variantes de rangée "character" au-delà du
jeu ghost/soft/normal/accent déjà réutilisé tel quel (`kDr2RowVar[]`), pas de copier/coller
de pattern. **Horloge indépendante** (`drumSTick()`, un par kit) — pas raccordée à
l'horloge partagée DRUM2/SYSEQ/303S2/SS2/GEST (`drum2Playing`/`drum2Step`), même choix
qu'EUCLI/CRUNCH (`eucliTick()`), pour zéro risque sur cette mécanique déjà complexe pour
une fonctionnalité qui n'a jamais demandé à la rejoindre.

**⚠️ Révision ultérieure — mode séquenceur-seul, 2 des 3 trims ci-dessus réintégrés.**
Demande explicite de l'utilisateur dans une session suivante : « je veux que pour les
drums, on ait accès uniquement au séquenceur, la touche de B4 servira alors au reset des
notes placées. je veux que DRUMS2 et DRUMS3 possèdent le même mécanisme de bank et
d'altérations que drums [DRUM2] ». Deux changements :
1. **Vue PAD supprimée** — DRUMS2/DRUMS3 n'ont plus qu'un seul écran (l'ancienne vue SEQ,
   désormais permanente). Plus de `drumSView[]`/toggle B4 ; B3 (qui n'arme plus rien,
   faute de surface de jeu live à enregistrer) est **libre** (`"-"` dans
   `ctrlLabelsFor()`), B4 **efface le pattern** (`drumSSeqVel/Mod/Row[kit]` à zéro,
   `drumSStep[kit]=0`) — plus de distinction court/long, un simple appui suffit.
2. **Mécanisme "bank" (bande des 8 pads) + "altérations" (mod par pas) réintégrés** —
   porté **mot pour mot** depuis la vue SEQ de DRUM2 (voir sa propre section plus haut) :
   `drumSSeqMod[2][8][16]` (0=NRM 1=50% 2=RND(pitch aléatoire 36-96) 3=DBL),
   `drumSPadAltMode[2][8]` (cyclé 1→2→3→1 via la ligne 1 du clavier), `drumSSelFromAlt[2]`
   (quel mod s'applique au prochain pas posé), `drumSSeqRow[2][8][16]`/`drumSDblPendingAt/
   Pitch[2][8]` (toujours "normal"=2 — pas de vue PAD donc pas d'enregistrement live
   ghost/soft/accent à l'origine des autres valeurs de `kDr2RowVar[]`, mais le tableau est
   réutilisé tel quel pour que `drumSTick()` applique le même calcul de lecture que DRUM2).
   Rendu (grille + LEDs) identique aux 4 styles de case de DRUM2 (plein=NRM, moitié haut
   plein+bas cadre=50%, trou creusé=RND, deux moitiés verticales=DBL) et à ses couleurs de
   ligne 1 (vert/ambre/magenta/cyan par mod). Vérifié par test headless : cycle d'altération
   (3 appuis ligne 1 → DBL), pose d'un pas sous le mod sélectionné, pose d'un second pas
   après re-sélection "normale" (ligne 0, mod revient à NRM), programmation + déclenchement
   effectif d'un double-hit différé, et effacement complet par B4 — capture d'écran du
   nouvel écran unique confirmant les 4 visuels de mod bien distincts même à cette taille.

**⚠️ 2e révision — B3 redevient REC, B4 devient double-usage (bank court / clear long).**
Demande explicite, juste après la révision ci-dessus (B4 à "clear" seul était donc une
étape transitoire) : « je veux que pour tous les drums, on ait B3: rec, B4: bank / clear
en appui court / appui long » — "tous les drums" = DRUMS2+DRUMS3 (DRUM2 lui-même, mode
distinct et déjà établi avec ses 3 vues PAD/SEQ/ANIM, n'est pas concerné). Deux
mécanismes ajoutés :
- **B3 = REC** (`drumSRecArmed[2]`, remis en service) : sans vue PAD à frapper en live, la
  pression ligne 0 (déjà vivante — elle prévisualise ET sélectionne le pad, voir plus
  haut) sert maintenant aussi de surface d'entrée live : armé + en lecture, une pression
  ligne 0 est quantifiée sur le pas le plus proche (même calcul `stepMs`/`elapsed`/
  `nearStep` que l'ancien enregistrement en vue PAD de DRUM2, juste déclenché depuis
  l'action déjà existante de la vue SEQ plutôt que depuis une vue dédiée).
- **B4 = banque de patterns, 4 slots** (`drumSPatVel/Mod/Row[2][4][8][16]`,
  `drumSPatFilled[2][4]`, `drumSActivePat[2]`) — même concept que la banque de DR2/GROOVE
  (`dr2Pats[]`/`grvOnPats[]`, déjà nommée "BANK" dans leur `ctrlLabelsFor()`) mais
  **simplifié** : pas de presse-papier copier/coller au double-clic (non demandé ici).
  **Appui court** (`drumSBankCycle()`) : sauvegarde le pattern courant dans son slot,
  avance au slot suivant (modulo 4), charge son contenu (ou repart vide si jamais utilisé)
  — rien n'est jamais perdu en cyclant. **Appui long** (`DRUMS_B4_LONGPRESS_MS`=600ms,
  `drumSBankClear()`) : efface uniquement le slot **actif** (buffers live + sa copie
  stockée) — même idiome bouton double-usage que B4 de BREAK/l'ancien 303S2
  (`drumSB4PressMs/LongFired[2]`, détection en haut de `handleButton()` + poll dans
  `loop()`). En-tête d'écran affiche désormais `P%d` (numéro de slot, 1-indexé) ; la ligne
  "Alt:.../Pt:..." s'inverse entièrement quand REC est armé (même convention que
  l'indicateur REC ARM d'origine de DRUM2 en vue PAD). Vérifié par test headless : cycle
  complet sur les 4 slots confirmant sauvegarde+restauration correctes, effacement par
  appui long isolé au seul slot actif (les 3 autres restent intacts), capture d'écran
  confirmant que tous les libellés B1-B4 ("PLAY"/"FX"/"REC"/"BNK/CLR") tiennent dans leurs
  colonnes de 32px sans déborder.

**⚠️ 3e révision — DRUM2 rejoint le même schéma (vues PAD/ANIM supprimées), "BNK/CLR" →
"PAT/CLR".** Demande explicite dans une session suivante : « pour drums, je veux supprimé
les view via B4, que ce soit un bouton de bank/clear [...] renomme là par pat pour pattern
[...] pour DRUMS, DRUMS et DRUMS [= DRUM2, DRUMS2, DRUMS3] » — **inverse la 2e révision
ci-dessus**, qui avait explicitement exclu DRUM2 ("qui garde ses 3 vues d'origine"). Deux
changements :
- **DRUM2 perd ses vues PAD et ANIM** : `drum2View` (0=pad/1=seq/2=anim) supprimée, la vue
  SEQ devient permanente (même collapse que DRUMS2/3 avaient déjà subi dans la 1ère
  révision). **Perdu** : la couche expressive ghost/soft/normal/accent à 4 niveaux de
  vélocité de l'ex-vue PAD (`kDr2RowVar[]` reste utilisé par le tableau SEQ pour la lecture,
  mais plus pour l'enregistrement live) ; le visualiseur GIF-synchronisé-aux-coups de l'ex-
  vue ANIM (`draniBrowse`/`draniRunning`, overlays par tambour) — devenu **injoignable**
  (plus aucun point d'entrée n'ouvre ce mode), code non supprimé (pas demandé), juste mort.
  L'enregistrement quantifié "en jouant" de l'ex-vue PAD a été **porté** dans la ligne 0 de
  la vue SEQ, exactement comme DRUMS2/3 l'avaient fait pour la leur (vélocité fixe=100,
  row-variation fixe="normal", pas de ghost/soft/accent enregistrables — même renoncement
  déjà accepté pour DRUMS2/3 dans la 1ère révision).
- **Nouvelle banque 4 slots pour DRUM2** (`drum2PatVel/Row/Mod/PitchOff[4][8][16]`,
  `drum2ActivePat`, `drum2PatCycle()`/`drum2PatClear()`, `DRUM2_B4_LONGPRESS_MS`=600ms) —
  même mécanisme que DRUMS2/3, avec un 4e tableau (`PitchOff`) que DRUMS2/3 n'ont pas
  (DRUM2 a un concept de décalage de pitch par pas que DRUMS2/3 n'ont jamais eu).
- **Renommage "bank"→"pat"** (plus fidèle à la fonction) : `drumSBankCycle()`/
  `drumSBankClear()` → `drumSPatCycle()`/`drumSPatClear()` (les tableaux `drumSPatVel/
  Mod/Row[]` portaient déjà ce nom, seules les fonctions traînaient l'ancien) ; libellé
  bouton `"BNK/CLR"` → `"PAT/CLR"` pour DRUM2+DRUMS2+DRUMS3, qui partagent maintenant un
  seul `case` dans `ctrlLabelsFor()`.

Vérifié par test headless : cycle complet sur les 4 slots de DRUM2 (sauvegarde+restauration
confirmées), effacement par appui long isolé au seul slot actif, build ESP32+simulateur
propres (RAM ESP32 légèrement *en baisse*, 38.8% contre ~44.5% avant — code mort PAD/ANIM
retiré du chemin de rendu/LED, les buffers `drani*` restant alloués mais inutilisés).

### 8 voix communes aux deux kits

`DRUMS_BD/SD/LT/HT/RS/CP/CH/OH` (config.h) — kick/snare/tom grave/tom aigu/rimshot/
clap/charley fermé/charley ouvert, CH choke OH (même mécanisme que DRUM2). `kit` 0=DRUMS2
(808), 1=DRUMS3 (909) ; mêmes 8 voix, réglages par défaut différents
(`kDrumSDefaultPitch/Decay/Volume[2][8]`) — 909 généralement plus aigu/plus court/plus
percutant que 808, conformément au caractère des machines d'origine.

### Synthèse (`audioDrumSynthHit()`, audio_engine.cpp) — un seul oscillateur par voix

- **BD/LT/HT (toniques)** : `TRIANGLE` + enveloppe d'amplitude EG0 (decay) + une SECONDE
  enveloppe EG1 qui fait chuter le pitch juste après l'attaque — le classique "pitch drop"
  analogique. Mécanisme AMY : `freq_coefs[COEF_EG1]` est un coefficient linéaire (en
  **octaves**) multiplié par la valeur courante de l'enveloppe EG1 et sommé directement
  dans le log2-fréquence de l'oscillateur (`combine_controls()` dans amy.c — somme linéaire
  simple, PAS le compactage log supplémentaire que subit l'amplitude dans
  `amp_combine_controls()`). Donc `freq_coefs[COEF_EG1] = 2.2` + une enveloppe EG1 qui
  descend de 1.0 à 0.0 en ~50ms donne "2.2 octaves au-dessus de la note de base à l'attaque,
  revenant à la note de base en ~50ms". **Aucun précédent dans ce projet** n'utilisait
  `freq_coefs[COEF_EG1]` (seul `filter_freq_coefs[COEF_EG0/EG1]`, pour moduler un filtre,
  l'était déjà, p.ex. FELU/MOD2) — vérifié expérimentalement plutôt que déduit en pur
  théorie : capture brute d'un coup de BD (tap temporaire dans
  `libminiaudio-audio.c`, retiré ensuite) + estimation de F0 par autocorrélation glissante
  en Python, confirmant une chute nette de ~160Hz à ~85-90Hz en ~15-20ms, cohérent avec un
  pitch-drop de kick classique (valeur exacte non calée au Hz près sur un vrai 808 — la
  valeur réelle reste ajustable au potard PITCH).
- **SD/RS/CP/CH/OH (bruit)** : `NOISE`, qui **ignore totalement la fréquence**
  (`render_noise()` dans oscillators.c ne lit jamais le log-fréquence de l'oscillateur) —
  le potard "PITCH" serait donc une commande morte pour ces 5 voix si on le laissait tel
  quel (piège documenté dans `CLAUDE.md`, « half the control does nothing »). Réaffecté à
  la coupure d'un filtre (`FILTER_BPF`/`FILTER_HPF` selon la voix) — un "tone" knob, comme
  sur les vraies 808/909. CP (clap) approxime le "flam" multi-rafale avec un SEUL
  oscillateur en donnant à son enveloppe EG0 plusieurs segments montée/descente rapides
  avant la vraie queue (EG0 accepte jusqu'à 24 points de breakpoint — largement assez) —
  moins cher qu'empiler plusieurs oscillateurs pour un résultat alternatif correct.

### Plage d'oscillateurs dédiée — a nécessité de relever `max_oscs`

Chaque kit a besoin de 8 oscillateurs dédiés fixes (pas de PCM donc pas de slot preset à
réserver, juste des oscillateurs). Tous les oscillateurs jusqu'à 249 étaient déjà
attribués (`STONE`=240-245, `DJ_OSC`=246, `SS_OSC`=247, `DJ_GRAIN`=248-249 — "les 2 seuls
oscillateurs libres sous le plafond de 250" d'après les commentaires existants de
`config.h`). `max_oscs` (configuré dans `audioInit()`, `audio_engine.cpp`) relevé de 250
(défaut AMY) à 266 pour faire de la place à `DRUMS2_OSC_BASE=250`/`DRUMS3_OSC_BASE=258`
(8 chacun). Changement global (affecte tous les targets) mais à faible risque : impact RAM
mesuré négligeable sur la cible ESP32 réelle (43.3% utilisé après le changement, contre
~43% déjà documenté avant — voir `structure/HANDOFF.md`), flash inchangé (80.0%).

### Vérification

Simulateur : build propre, les 16 voix (2 kits × 8) produisent un signal non nul et non
saturé (RMS/pic mesurés par tap temporaire), captures d'écran des vues PAD/SEQ vérifiées
(un vrai bug trouvé au passage : le pourcentage de volume débordait de l'écran — texte
`"100%"` posé à `x=117` avec une police 5x7, largement hors des 128px — corrigé en
resserrant la barre et en reculant le texte à `x=103`). ESP32 : build propre
(`pio run -e esp32-s3-devkitc-1`), RAM/flash quasi inchangés (voir ci-dessus). **Non
testé sur carte réelle.**

---

## MODE_PRESETS — banque de patchs curatée (inspirée du bank Sloop)

Demande utilisateur : « créer un mode avec pleins d'instruments dedans, les projets
Sloop, X0X et Felucca possèdent une diversité d'instruments et d'algorithmes, j'aimerais
en tirer profit ! » — scopé par questions explicites à **une banque de presets à
parcourir par catégorie** (pas un sélecteur algo+macros brut, FELU le fait déjà), avec un
**large éventail** assumé (45 presets dès cette passe, d'autres possibles en itérations
suivantes).

**Zéro nouveau DSP** — MODE_PRESETS est une couche de curation posée sur le moteur
existant de MODE_FELU (`feluAlgoIdx`/`feluP[4]`/`feluApplyP4P7()`, voir sa propre section
plus haut) : chaque preset est juste `{nom, catégorie, index d'algo FELU, 4 macros
normalisées}` (`PresetDef`, `kPresets[]`, `config.h`). `presetApply(idx)` charge un
preset en réglant `feluAlgoIdx`/`feluP[]` puis en appelant `feluApplyP4P7()` — le même
pipeline que FELU lui-même utilise pour ses propres macros.

**45 presets, 8 catégories** (`PresetCategory`/`kPresetCatNames[]`) inspirées du
classement "Kind" du bank Sloop (BASS/KEYS/ORGAN/PAD/LEAD/PLUCK/STAB/FX) : BASS(6)
KEYS(4) ORGAN(5) PAD(9) LEAD(6) PLUCK(6) STAB(5) FX(4). Couvre les 9 moteurs FELU
existants ; les presets Sloop tagués DIGITAL/SAMPLE/GRAIN (sans équivalent FELU — déjà
hors scope de FELU lui-même, cf. sa propre section) sont omis, pas approximés. Instruments
melodiques uniquement — la diversité "batterie" est déjà couverte par DRUM2/DRUMS2/
DRUMS3, pas dupliquée ici.

**UI** : B1=FX, B2=Env, B3 ouvre `OVERLAY_PRESETS`, B4=Octave (identique à FELU, B3
remplace juste son sélecteur d'algo `OVERLAY_FELU_ALGO`). `OVERLAY_PRESETS` est un
browser 2 colonnes (catégories à gauche / presets de la catégorie à droite) qui **mirror**
`OVERLAY_PKMN` à l'identique (même géométrie, même séparateur vertical x=39) — seule
différence : contrairement à PKMN (confirmer puis appliquer), chaque déplacement
joystick dans `OVERLAY_PRESETS` **applique immédiatement** le preset survolé
(prévisualisation live façon Sloop), une pression de touche ne fait que fermer l'overlay
(`presetApply()` a déjà tourné pendant le scroll). P2 balaie l'index plat 0..44 ; P4-P7
réutilisent tel quel le bloc FELU (`feluPCache[]`/`feluPotNeedsSync`/`potPopupShow()`).

**Vérifié par test headless** : build simulateur + ESP32 propres ; injection au boot
appliquant un preset par catégorie (8 au total) + note jouée + tap RMS confirmant un
signal non nul pour chacune (ex. WOW BASS/VOIC, ACCORDION/TRIO, ATMOS PAD/TRIO, PWM
LEAD/ANLG, METAL NOISE/NOIS...) ; résolution catégorie+index→index plat du browser
vérifiée indépendamment (PAD, item 2 → "ATMOS PAD", idx 17) ; noms de presets confirmés
≤11 caractères (budget d'affichage 12 car. respecté). Instrumentation temporaire retirée
après vérification.

**⚠️ Révision — joystick pilote octave + instrument directement sur l'écran principal,
comme MODE_303S.** Demande explicite dans une session suivante : « je veux pouvoir jouer
des notes en même temps que je suis dans le mode bank de PRSET [...] je veux que comme
pour 303S, [le joystick] impacte l'octave et l'instrument sélectionné ». Avant cette
révision, le joystick ne faisait que moduler la vélocité (JX, repris de FELU) — sans
impact perceptible hors du moment précis d'un appui de note. Nouveau bloc joystick dédié
dans `loop()` (gated `s_overlay==OVERLAY_NONE`, mirror exact du bloc de MODE_303S) : JX
balaie l'index plat de preset (`presetApply()`, debounce 150ms), JY décale l'octave
(`noteMap.setOctave()`, debounce 300ms) — tourne directement sur l'écran principal, donc
jouable au clavier en même temps (contrairement à `OVERLAY_PRESETS`/B3, qui bloque
toujours les notes pendant qu'il est ouvert, comme tous les browsers 2-colonnes de ce
genre). Barre de vélocité JX retirée (vélocité fixe=1.0 désormais) puisque JX est repris
par le changement d'instrument. **Bug trouvé en testant, corrigé** : relâcher une touche
recalculait sa hauteur via `noteMap.getMidiNote()` à l'octave *courante* plutôt que celle
au moment de l'appui — si JY change l'octave pendant qu'une note est tenue (exactement le
scénario visé par cette révision), le relâchement éteignait la MAUVAISE hauteur, laissant
la voix d'origine bloquée indéfiniment. Fix : la note jouée est stockée dans
`activeNotes[row][col]` à l'appui et c'est exactement celle-là qui est éteinte au
relâchement, peu importe les changements d'octave entre-temps. Changer d'instrument (JX)
coupe toujours une note en cours (`presetApply()` appelle `audioAllNotesOff()`, même
comportement que le P2 de FELU — inévitable, changer de `SynthShape` ne peut pas préserver
une voix en cours) ; changer d'octave (JY) ne coupe PAS une note tenue. Vérifié par test
headless en 3 phases (note seule sans joystick, changement d'instrument qui coupe
légitimement puis nouvelle note qui sonne normalement, changement d'octave qui préserve
une note tenue) + confirmation que la note se relâche proprement après le fix.

**Itérations suivantes anticipées** (pas dans cette passe, per décision explicite de
l'utilisateur "large éventail, quitte à itérer en plusieurs passes") : plus de presets
dans les catégories existantes, voire de nouveaux moteurs si une approximation AMY
raisonnable de DIGITAL/SAMPLE/GRAIN apparaît.

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

**Édition des paramètres FX = relative** : `fxPotDeltaApply(fx, p)` (`main.cpp`) ajoute
`pots[3+p].rawDelta` (un tour d'encodeur = pleine plage) à la position normalisée *courante*
du paramètre (Cut de FILT garde sa courbe exponentielle). L'ancien mappage absolu
(`params = mn+(mx-mn)*pots[i].value`, l'intégrale de l'encodeur démarrant à 0) faisait
« sauter » le paramètre à ~0 au premier tour. Tous les blocs de dispatch de pots des modes
(SYNTH, POKEMON, DJ, OMNI, SAMPLE, 303S, SS2, DRUM2, DR2, GROOVE, EUCLI, CRUNCH, STONE,
GRANULAR2) passent par ce helper ; il n'y a plus de suivi de position (`lp_fx[]`,
`fxPotNeedSync`) à resynchroniser, et n'importe quel changement — FILT compris — appelle
`applyFxEffect(fxSelected)`. Les paramètres de type (Typ) restent des flottants 0..3 arrondis
à la consommation.

**P1 (`pots[0]`) et P3 (`pots[2]`) sont réservés à tous les modes sans exception** : Volume et
BPM, appliqués inconditionnellement dans `loop()` *avant* le `switch(currentMode)` des pots.
Un mode ne peut donc pas les relire pour un usage propre — seuls P2 (`pots[1]`) et P4-P7
(`pots[3..6]`) sont libres (la struct `CtrlLabels` n'a d'ailleurs pas de champ p1/p3). Bug
corrigé : `MODE_LANIM` lisait `pots[0..3]` pour Spd/Dens/Hue/Bri et se retrouvait collé sur
Volume/BPM — il utilise maintenant `pots[1],[3],[4],[5]` (P2/P4/P5/P6), avec une entrée dédiée
dans `ctrlLabelsFor()`. Tout nouveau mode doit aussi avoir sa case dans `ctrlLabelsFor()`
(sinon la légende console affiche des tirets).

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

### FILT bypass au cutoff max → « s'active » aléatoirement *(historique — FILT est désormais un effet de bus, voir « Effets »)*
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

### COMPRESS : « réduit juste fortement le volume »
Enveloppe calculée sur la valeur brute `SAMPLE` (point fixe) comparée à un seuil normalisé ;
manquait `S2F()`. Voir « Effets ». Règle générale : dans `amy.c`, toute comparaison à une
constante « humaine » passe par `S2F()`.

### Boucle canaux/buffers : planaire, pas entrelacé
Les blocs bus-0 historiques du simulateur (ladder, ringmod, compresseur) indexaient
`buf[i*NCHANS+ch]` (entrelacé) sur des buffers planaires : les canaux étaient mélangés sans
que ça se remarque sur du contenu quasi mono. Réécrits en planaire dans `amy_apply_*`. Un
panoramique bus (feu AUTOPAN) a échoué tant qu'il indexait entrelacé — c'est ce qui a révélé
la vraie disposition.

### FILT/ResEcho « cassent » le son (divergence)
Filtre d'état de forme Chamberlin instable à haut cutoff + haute résonance ; boucle ResEcho de
gain > 1 plafonnée à ±4. Voir « Effets » (TPT stable, gain de boucle unitaire, limiteur doux,
garde NaN).

### FX partagés : 4 bugs de la même famille « atteint seulement ce qui a été câblé »
AUTOPAN (pan par oscillateur SYNTH_CH seulement → aucun effet sur batterie/samples),
DISTORT/OVERDRIVE et FILT (filtres par oscillateur, plages dédiées à câbler à la main). Remède
retenu : effets de bus (`amy_apply_*`), qui atteignent tout ce qui est mixé.

### Menu FX / joystick : écran figé sur l'overlay
Cliquer le joystick (ou B1 long) pendant un overlay ouvrait le menu sans remettre `s_overlay`
à `OVERLAY_NONE` ; `drawScreen()` teste les overlays avant `menuOpen` → écran bloqué. Corrigé
aux deux endroits qui ouvrent le menu.

### u8g2 : `drawTriangle()` ignore `setClipWindow()` — contrairement à `drawDisc`/`drawCircle`/`drawBox`
`u8g2_polygon.c`'s `pg_hline()` clippe chaque ligne de balayage contre les dimensions
PHYSIQUES de l'écran (`u8g2_GetDisplayWidth/Height`), jamais contre la fenêtre posée par
`setClipWindow()`. Tout remplissage à base de triangles doit donc être géométriquement
autonome (sommets placés pour ne jamais pouvoir dépasser, via la garantie de convexité —
voir « jauge circulaire » dans la section UI potards) plutôt que de compter sur un clip qui
n'opère pas sur ce type de primitive. Découvert en traquant un débordement de pixels sur
la jauge circulaire des popups potards P4-P7.

### Couleur de contenu choisie sur une condition plus étroite que celle qui remplit le fond → contenu invisible
Classe de bug trouvée dans le rendu du séquenceur 303S2 : `setDrawColor(cur?0:1)` pour le
contenu, alors que le fond de la case est rempli dès `cur || !isRest` — une condition plus
large. Toute case satisfaisant la condition large mais pas l'étroite dessine un contenu de
la même couleur que son propre fond (invisible, pas juste petit/difficile à lire). À
vérifier chaque fois qu'une case a sa couleur de contenu et sa couleur de fond décidées par
deux conditions différentes plutôt que la même variable booléenne.
