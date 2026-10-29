# src/sprites/ — Sprites 1-bit pour l'écran OLED

## pokemon_sprites.h

Auto-généré par `tools/gen_pokemon_sprites.py` — **ne pas éditer à la main**.

Contient 42 sprites Pokémon en format XBM 1-bit pour `u8g2_DrawXBM()`.  
Chaque sprite : 48×48 pixels → 288 octets, stocké en flash ESP32 (`PROGMEM`).

**Utilisé dans** : `src/main.cpp` → mode **POKEMON** (`MODE_POKEMON`)  
**Include** : `#include "sprites/pokemon_sprites.h"` dans `main.cpp`

### Sprites disponibles

```
clefairy    jigglypuff  meowth      eevee       snorlax
charmander  vulpix      ninetales   flareon     magmar
moltres     squirtle    psyduck     gyarados    vaporeon
lapras      bulbasaur   pikachu     magnemite   jolteon
electabuzz  zapdos      articuno    machop      primeape
arbok       nidoking    alakazam    mewtwo      mew
haunter     gengar      dratini     dragonair   dragonite
geodude     onix        aerodactyl  diglett     rhydon
butterfree  scyther
```

## Ajouter / modifier un sprite

1. Placer l'image source (PNG, 48×48 recommandé) dans `tools/drums_img/` ou un dossier dédié
2. Lancer le générateur :
   ```bash
   python3 tools/gen_pokemon_sprites.py
   ```
   Le script régénère `src/sprites/pokemon_sprites.h` avec tous les sprites.

3. Recompiler — les sprites sont chargés en flash au build.

## Format XBM rappel

- 1 bit par pixel, MSB = pixel gauche (compatible `u8g2_DrawXBM`)
- Dimensions : 48 px × 48 px = 288 octets par sprite
- Stocké `PROGMEM` → occupe la flash, pas la RAM
