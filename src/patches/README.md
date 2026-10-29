# src/patches/ — Formes d'onde AMY (wavetables)

Chaque fichier = une forme d'onde personnalisée pour la synthèse AMY.  
Format : `const int name[]` où `name[0]` est la longueur en échantillons, suivi des valeurs 16-bit signées.

**Ces patches ne sont pas encore inclus dans le firmware compilé** — ils sont disponibles pour une intégration future avec `amy_load_custom_wave()` ou équivalent.

## Fichiers

### Formes d'onde de base
| Fichier | Samples | Description |
|---------|---------|-------------|
| `pureSin.h` | 480 | Sinusoïde pure — forme d'onde de référence |
| `pureTriSoft.h` | ~480 | Triangle adouci — moins d'harmoniques que le triangle dur |

### Waveforms lead
| Fichier | Samples | Description |
|---------|---------|-------------|
| `jlead1.h` | 499 | Lead 1 — timbre brillant |
| `jlead2.h` | ~500 | Lead 2 |
| `jlead3.h` | ~680 | Lead 3 — plus riche en harmoniques |
| `jlead4.h` | ~500 | Lead 4 |

### Waveforms basse
| Fichier | Samples | Description |
|---------|---------|-------------|
| `jbass1.h` | ~280 | Basse 1 — timbre chaud |
| `jbass2.h` | ~270 | Basse 2 |

### Waveforms pad / synth
| Fichier | Samples | Description |
|---------|---------|-------------|
| `jpad1.h` | ~680 | Pad — doux et atmosphérique |
| `synth2.h` | ~800 | Synthé 2 |
| `synth3.h` | ~800 | Synthé 3 |
| `pad2.h` | ~1000 | Pad 2 |
| `pad3.h` | ~1000 | Pad 3 |

## Utilisation prévue

Ces formes d'onde peuvent être chargées dans AMY comme oscillateurs custom :

```cpp
#include "patches/jlead1.h"
// Charger dans AMY :
amy_event e = amy_default_event();
e.osc   = MY_OSC;
e.wave  = CUSTOM;
e.data  = jlead1;       // tableau de valeurs
e.len   = jlead1[0];    // premier élément = longueur
amy_add_event(&e);
```

## Modifier / créer un patch

1. Créer une forme d'onde mono, 16-bit signé, dans Audacity ou Python (numpy)
2. Exporter en raw 16-bit signed little-endian
3. Convertir en `.h` :
   ```bash
   python3 -c "
   import struct
   data = open('mon_son.raw','rb').read()
   vals = struct.unpack(f'{len(data)//2}h', data)
   print(f'const int mon_patch[] = {{{len(vals)},')
   print(','.join(str(v) for v in vals) + '};')
   " > src/patches/mon_patch.h
   ```
