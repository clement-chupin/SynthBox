# GrvEP — Mode Templates

Un **template** est un ensemble de conventions UX partagées entre plusieurs modes.
Créer un nouveau mode dans un template existant revient à brancher son logique spécifique
sur une structure d'interaction déjà éprouvée (boutons, pots, LED, overlays, navigation).

---

## 1. Template SYNTH

> Modes : `SYNTH`, `POKEMON`, `I303`, `MOD2`

Instrument polyphonique ou monophonique joué sur la grille-clavier.
L'accent est mis sur la sélection d'un **algorithme de synthèse** et le contrôle en temps réel
via les potards et les overlays.

### Pots

| Pot | Rôle |
|-----|------|
| P1 | Volume global (0–1). En MODE_POKEMON : multiplié par `PokemonDef.gain`. |
| P2 | Paramètre principal de l'instrument (onde, texture, algo, dusty…). |
| P3 | BPM (partagé avec le séquenceur interne si activé). |
| P4–P7 | Paramètres de modulation — sémantique fixe (SYNTH/POKEMON/I303) ou algo-spécifique (MOD2). |

### Boutons (B1–B4, appui court sur la grille)

| Bouton | Action |
|--------|--------|
| B1 | Overlay ARP / Scale (`OVERLAY_SCALE_ARP`) — toggle. |
| B2 | Overlay Envelope (`OVERLAY_ENV`) — toggle. |
| B3 | Overlay Browser d'instrument (`OVERLAY_INSTR`, `OVERLAY_I303_WAVE`, `OVERLAY_MOD2_ALGO`) — toggle. |
| B4 | Octave (SYNTH/POKEMON/I303) · Cycle LFO mode (MOD2) · Mode-spécifique selon instrument. |

### Joystick

| Axe | Rôle |
|-----|------|
| JX | Vélocité des notes (dead-zone ±15 %). |
| JY | Pitch-bend (±2 demi-tons, dead-zone ±15 %). |

En présence d'un overlay browser (OVERLAY_INSTR, OVERLAY_I303_WAVE, OVERLAY_PKMN) :
JX/JY naviguent dans le browser ; le pitch-bend est suspendu.

### LEDs

Palette **4 couleurs** par instrument, indexée `note % 4`.

- Touche relâchée : `CHSV(pal[note%4], 200, 16)` (faible luminosité).
- Touche pressée  : `CHSV(pal[note%4], 230, 255)` (pleine luminosité).

Chaque mode déclare sa palette dans sa structure de données
(ex. `PokemonDef.pal[4]` pour POKEMON).

### Affichage OLED (ligne de statut)

Format libre sur 128 px mais doit afficher au minimum :
- Nom de l'instrument / algo courant.
- Octave courante (`Oct:+0`).
- Informations de sélection si applicable (ex. `26/42` pour POKEMON).

### Overlays disponibles

| Overlay | Description |
|---------|-------------|
| `OVERLAY_SCALE_ARP` | Sélection de gamme + mode arpège. |
| `OVERLAY_ENV` | Paramètres d'enveloppe ADSR. |
| `OVERLAY_INSTR` | Browser d'algorithmes de synthèse (SYNTH, MOD2). |
| `OVERLAY_I303_WAVE` | Browser des 12 formes d'onde I303 (TRI, TRI2, SAW, SW3, SQR, SQ2, SIN, NAP, SWF, SQF, SNF, PINK). |
| `OVERLAY_PKMN` | Browser Pokémon (POKEMON uniquement) : JX = type, JY = item. |
| `OVERLAY_FX` | Sélection d'effet actif (modes qui exposent les FX via B1). |

### Créer un nouveau mode SYNTH

1. Définir la structure de données de l'instrument (forme d'onde, enveloppe, filtre, palette LEDs, gain).
2. Implémenter `<mode>Apply()` qui appelle `audioSetShape / audioSetEnvelope / audioSetFilter / audioSetVolume`.
3. Câbler P1–P7 dans le switch du pot-handler.
4. Câbler B1–B4 dans `handleButton()` en suivant le tableau ci-dessus.
5. Ajouter l'entrée dans `kMenuCatInstr[]` et `kMenuCatSizes[]`.
6. Implémenter le bloc LED dans `updateLedsAndShow()`.
7. Implémenter le bloc OLED dans `drawScreen()`.

---

## 2. Template SEQUENCER

> Modes : `DRUM2`, `303S`, `303S2`, `SYSEQ`, `SS2`, `GRANULAR2`

Pattern-sequencer pas-à-pas.
L'accent est mis sur la **programmation de patterns** et la synchronisation BPM.

### Pots

| Pot | Rôle commun |
|-----|-------------|
| P1 | Volume global. |
| P3 | BPM. |
| P4–P7 | Paramètres de l'instrument courant **ou** paramètres FX actifs. |

Le rôle de P2 est mode-spécifique (cutoff, texture, blend…).

### Boutons (B1–B4)

| Bouton | Action générique |
|--------|-----------------|
| B1 | FX / Transport (play/stop selon mode). |
| B2 | Accent ou paramètre binaire mode-spécifique. |
| B3 | Clear / Reset pattern ou sélection de preset. |
| B4 | Navigation octave ou vue séquenceur. |

La grille clavier est utilisée pour **entrer / effacer** des pas ou jouer en live.
Le mode B hold (appui long) peut accéder à des overlays secondaires (`OVERLAY_SEQB2`, `OVERLAY_303`, `OVERLAY_FX`).

### Joystick

| Axe | Rôle |
|-----|------|
| JX | Cycle de forme d'onde (pour les modes à onde variable : 303S, I303, SS2). |
| JY | Octave (haut = +1 oct, bas = -1 oct). |

### LEDs

Affichage de l'état des pas du pattern en cours.

- Pas actif + tête de lecture : pleine luminosité accent.
- Pas actif + hors lecture : luminosité intermédiaire.
- Pas vide : éteint ou très faible.

### Affichage OLED

Grille de pas (typiquement 8 ou 16 pas sur 2 lignes), BPM, octave, slide/accent.

### Overlays disponibles

| Overlay | Description |
|---------|-------------|
| `OVERLAY_FX` | Sélection et paramétrage d'effets. |
| `OVERLAY_303` | Options 303 : wave, slide, accent, octave. |
| `OVERLAY_303_PRESET` | Presets 303 prédéfinis. |
| `OVERLAY_SEQ_OPT` | Options de lecture (loop, 1-shot, cut-note). |
| `OVERLAY_SEQB2` | Grille FX + octave + shape (accès par B hold). |
| `OVERLAY_SYSEQ` | Options spécifiques SYSEQ. |

### Créer un nouveau mode SEQUENCER

1. Définir le buffer de pattern (`step[]`, taille, résolution).
2. Implémenter le tick séquenceur dans le timer ISR ou le task loop.
3. Câbler B1–B4 (transport, accent, clear, navigation).
4. Afficher les pas dans l'OLED et les LEDs.
5. Synchroniser sur `bpmToMs(bpm)` via le timer global.

---

## 3. Template STANDALONE / PERFORMANCE

> Modes : `EXP`, `EXP2`, `EXP3`

Mode autonome de performance ou d'exploration ne nécessitant pas de grille-clavier fixe.
L'interface est entièrement définie par le mode lui-même.

### Pots

Libres. Chaque mode définit la sémantique de P1–P7.

### Boutons

Libres. Pas de convention B1–B4 imposée.

### Joystick

Libre (peut contrôler pitch, filtre, panoramique, position, etc.).

### LEDs

Visualisation libre (animation, vu-mètre, spectre, palette fixe, etc.).

### Affichage OLED

Libre — interface propre au mode.

### Créer un nouveau mode STANDALONE

Aucune convention stricte. Décrire dans un commentaire en tête du bloc la sémantique
de chaque contrôle, puis implémenter librement dans les sections
`handleButton`, `handlePots`, `drawScreen`, `updateLedsAndShow`.

---

## 1b. MOD2 — Synthétiseur Modulaire (variante du template SYNTH)

MOD2 suit le template SYNTH mais introduit le concept d'**algorithme de synthèse** :
au lieu d'un son fixe, chaque algo est une topologie audio complète avec ses 4 paramètres exposés.

### Algorithmes disponibles (P2 = sélection, B3 = browser visuel)

| Algo | Shape AMY | P4 | P5 | P6 | P7 |
|------|-----------|----|----|----|----|
| VCO  | SAW | Cutoff | Resonance | Drive | Decay |
| DUO  | SUPERSAW | Cutoff | Resonance | Saturation | Reverb |
| FM2  | SAW_FM | FM Depth | Cutoff | Resonance | Drive |
| ACID | SAW | Cutoff (80–2kHz) | Resonance (2–16) | Decay | Drive |
| PAD  | SUPERSAW | Cutoff | Chorus | Reverb | Saturation |
| NOIS | NOISE_WHITE | Cutoff | Resonance | Drive | Decay |
| PLCK | PLUCK | Brightness | Resonance | Drive | Decay |
| LEAD | HOOVER | Drive (1–8) | Cutoff | Resonance | FM Depth |

### Mapping des boutons MOD2

| Bouton | Action |
|--------|--------|
| B1 | Overlay FX (`OVERLAY_FX`) — toggle. |
| B2 | Overlay ENV (enveloppe ADSR) — toggle. |
| B3 | Algo browser (`OVERLAY_MOD2_ALGO`) — grille 2×4, 8 algos — toggle. |
| B4 | Cycle play mode (Poly → Mono → Slide → Poly). |

### LFO

Moteur LFO background (10ms tick) indépendant des pots.  
- **Destination** : Pitch ou Filter (selon mode LFO)  
- **Waveforms** : SIN, TRI, SAW, RevSAW, SQR, S&H  
- **Rate** : 2 Hz (fixe)  
- **Depth** : 0.5 st (pitch) / 500 Hz (filter)  
- **B4** = cycle parmi 13 modes (Off + 6 Pitch + 6 Filter)  

### Ajouter un nouvel algo MOD2

1. Ajouter une entrée dans `kMod2Algos[]` (`config.h`) avec shape + 4 `Mod2ParamDef`
2. Ajouter un `case N:` dans `mod2ApplyP4P7()` qui appelle les fonctions audio correspondantes
3. Mettre à jour `MOD2_ALGO_COUNT`
4. (optionnel) Ajouter des LEDs algo-spécifiques dans `updateLedsAndShow()`

---

## Résumé des templates par mode

| Mode | Template | B3 overlay instrument | B4 |
|------|----------|-----------------------|----|
| SYNTH | SYNTH | `OVERLAY_INSTR` | Octave |
| POKEMON | SYNTH | `OVERLAY_PKMN` | Octave |
| I303 | SYNTH | `OVERLAY_I303_WAVE` | Octave |
| MOD2 | SYNTH (variante) | `OVERLAY_MOD2_ALGO` | Cycle play mode (Poly/Mono/Slide) |
| DRUM2 | SEQUENCER | — | Vue |
| 303S | SEQUENCER | — | Octave / JX cycle |
| 303S2 | SEQUENCER | — | Clear |
| SYSEQ | SEQUENCER | — | Octave |
| SS2 | SEQUENCER | — | Mode play |
| GRANULAR2 | SEQUENCER | — | Granular opts |
| EXP | STANDALONE | libre | libre |
| EXP2 | STANDALONE | libre | libre |
| EXP3 | STANDALONE | libre | libre |
