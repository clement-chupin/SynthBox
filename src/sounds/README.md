# src/sounds/ — Échantillons PCM compilés

Chaque fichier = un échantillon audio mono, encodé directement dans le firmware ESP32.  
Inclus dans `src/audio_engine.cpp` ; enregistrés dans AMY via `pcm_load()` au démarrage.

## Formats

| Format | Type C | Qualité |
|--------|--------|---------|
| `int8_t[]` (8-bit) | `static const int8_t name[]` | Compact, bruit de quantification audible |
| `int[]` (16-bit en 32-bit) | `const int name[]` | Meilleure fidélité, 2× plus lourd |

## Table des pads — `kDrumPads[32]` dans `audio_engine.cpp`

Les 32 pads sont organisés en 4 rangées × 8 colonnes (rangée 0 = bas, colonne 0 = droite).

### Rangée 0 — Batterie standard (int8_t)

| Fichier | Label | Instrument |
|---------|-------|------------|
| `kick1.h` | KK1 | Grosse caisse principale |
| `kick2.h` | KK2 | Grosse caisse alternative |
| `snare1.h` | SN1 | Caisse claire 1 |
| `snare2.h` | SN2 | Caisse claire 2 |
| `snare3.h` | SN3 | Caisse claire 3 |
| `hihat1.h` | HH1 | Charleston fermé |
| `bongo1.h` | BNG | Bongo |
| `snareB3.h` | SNB | Caisse claire brossée 3 |

### Rangée 1 — Percussion étendue (const int)

| Fichier | Label | Instrument |
|---------|-------|------------|
| `kick3.h` | KK3 | Grosse caisse 3 |
| `hihat2.h` | HH2 | Charleston 2 |
| `clap1.h` | CLP | Clap |
| `crash1.h` | CRS | Cymbale crash |
| `ride1.h` | RDE | Cymbale ride |
| `snareB1.h` | SB1 | Caisse claire brossée 1 |
| `snareB2.h` | SB2 | Caisse claire brossée 2 |
| `bass1.h` | BS1 | Basse 1 |

### Rangée 2 — Basse + SFX (const int)

| Fichier | Label | Instrument |
|---------|-------|------------|
| `bass2.h` | BS2 | Basse 2 |
| `sfx1.h` | SX1 | Effet sonore 1 |
| `sfx2.h` | SX2 | Effet sonore 2 |
| `sfx3.h` | SX3 | Effet sonore 3 |
| `sfx4.h` | SX4 | Effet sonore 4 |
| `sfx5.h` | SX5 | Effet sonore 5 |
| `sfx6.h` | SX6 | Effet sonore 6 |
| `sfx7.h` | SX7 | Effet sonore 7 |

### Rangée 3 — SFX + Mélodique (const int)

| Fichier | Label | Instrument |
|---------|-------|------------|
| `sfx8.h` | SX8 | Effet sonore 8 |
| `sfx9.h` | SX9 | Effet sonore 9 |
| `sfx10.h` | S10 | Effet sonore 10 |
| `sfx11.h` | S11 | Effet sonore 11 |
| `sfx12.h` | S12 | Effet sonore 12 |
| `guitar1.h` | GTR | Guitare |
| `synth1.h` | SYN | Synthé |
| `pad1.h` | PAD | Pad |

## Modes qui utilisent ces sons

| Mode | Utilisation |
|------|-------------|
| **DRUM2** | Batterie live — 8 pads sélectionnés via `kDrum2Remap[]` |
| **SYSEQ** | Séquenceur de batterie — mêmes pads, jouées en séquence |
| **SAMPLE** | Mode sampler — les 32 pads accessibles via le clavier |
| **SS2** | Sampler séquenceur — pads mélodiques (BS1, GTR, SYN, PAD…) |

## Modifier un son

1. Convertir le nouveau son en mono, 8-bit ou 16-bit signé, fréquence ~22050 Hz
2. Remplacer les valeurs dans le tableau `.h` correspondant (ou utiliser `xxd -i`)
3. Recompiler — le son est chargé au boot via `audioLoadDrumSamples()`
