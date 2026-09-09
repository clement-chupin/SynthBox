# GrvEP

- **Exécutable Windows prêt à l'emploi** : [releases/GrvEP-windows.exe](releases/GrvEP-windows.exe) — aucune installation, double-clic pour lancer.
- **Vidéo de présentation** : [Instagram](https://www.instagram.com/p/DdB59hfBGcy/)

![GrvEP](./images/image.png)

GrvEP est une boîte à rythmes / synthétiseur portable basée sur un ESP32-S3 : plus de 30
modes — séquenceurs de drums, synthés (soustractif, FM, **wavetable**, TB-303, granulaire,
échantillonneur...), et une famille de modes expérimentaux/génératifs (automate cellulaire,
essaim de boids, thérémine, physique de balles...) — lecture de samples depuis une carte
SD, écran OLED, clavier de 32 touches (4×8), joystick, 7 potentiomètres et LEDs
adressables (FastLED) pour le retour visuel.

**Nouveau utilisateur ?** Voir [docs/GUIDE_UTILISATEUR.md](docs/GUIDE_UTILISATEUR.md) —
guide d'utilisation complet (contrôles, navigation, panorama des modes), convertible en
PDF (`pandoc docs/GUIDE_UTILISATEUR.md -o guide.pdf --toc --pdf-engine=wkhtmltopdf`, ou
avec un moteur LaTeX comme `xelatex` si installé).

**Développeur, ou envie de contribuer via Claude Code ?** Voir [CLAUDE.md](CLAUDE.md) —
contexte projet condensé (les 4 cibles de build, le piège des deux copies d'AMY à
synchroniser à la main, la méthodologie de test) et [structure/](structure/) pour la
documentation d'architecture détaillée.

Le même code applicatif (`src/`) tourne sur quatre cibles :

- **ESP32-S3** — le matériel réel, via [PlatformIO](https://platformio.org/).
- **Simulateur desktop Linux/Windows** — une reconstruction de la carte (écran, clavier, joystick, LEDs) en SDL2, pour développer/tester sans le matériel.
- **APK Android** — le même simulateur SDL2, compilé nativement en C++ pour Android (pas de wrapper web).
- **Web (WebAssembly)** — le même simulateur compilé via Emscripten (voir [structure/WEB_SIMULATOR.md](structure/WEB_SIMULATOR.md)).

## Nouveautés récentes

- **Automatisation des effets** — double-cliquer un effet actif dans la grille FX ouvre
  un éditeur de modulation (LFO assignable à n'importe quel paramètre de l'effet,
  profondeur/vitesse/forme d'onde/synchro BPM réglables aux pots).
- **Synthé modulaire wavetable** (mode MODULAR, refonte complète) — deux oscillateurs
  qui parcourent des wavetables AMY (5 tables embarquées, 64 formes d'onde chacune),
  position de morphing indépendante par oscillateur, LFO de modulation, filtre partagé.
- **Deux nouveaux modes génératifs** : **LIFE** (Jeu de la vie de Conway sur la grille
  de touches — les naissances/morts de cellules déclenchent des notes) et **SWARM**
  (essaim de boids avec cohésion/séparation/alignement, zone d'attraction pilotée au
  joystick).
- Détails complets dans [structure/SOFTWARE.md](structure/SOFTWARE.md) (architecture) et
  [docs/GUIDE_UTILISATEUR.md](docs/GUIDE_UTILISATEUR.md) (utilisation).

## Build pour l'ESP32

Prérequis : [PlatformIO](https://platformio.org/install/cli) (`pip install platformio` ou l'extension VSCode).

```bash
# Compiler
pio run -e esp32-s3-devkitc-1

# Compiler et flasher (carte branchée en USB)
pio run -e esp32-s3-devkitc-1 -t upload

# Moniteur série
pio run -e esp32-s3-devkitc-1 -t monitor
```

La configuration de build (partitions 16MB, PSRAM, USB MIDI...) est définie dans [platformio.ini](platformio.ini).

## Build et lancement du simulateur

Le simulateur reproduit le firmware sur Linux avec SDL2 (fenêtre, clavier, joystick, LEDs simulées).

Prérequis : `cmake`, un compilateur C++, `libsdl2-dev` (un `.deb` est fourni dans `simulator/` si besoin d'une install hors-ligne).



![GrvEP](./images/simulator.png)



```bash
cd simulator
./build.sh

# Lancer
DISPLAY=:1 ./build/grvep_sim

# or in one line
./simulator/build.sh; DISPLAY=:1 ./simulator/build/grvep_sim

```

Le binaire est produit dans `simulator/build/grvep_sim`.

Deux fenêtres supplémentaires s'ouvrent automatiquement à côté (desktop uniquement,
absentes sur Android), toutes deux basées sur le même flux audio réellement envoyé à
la sortie (donc déjà passé par tous les FX actifs) :

- « GrvEP - Spectrogramme » : waterfall défilant (historique dans le temps).
- « GrvEP - EQ » : courbe unique en temps réel, des graves (gauche) aux aigus
  (droite), comme un analyseur de spectre/EQ classique — plus directe que le
  waterfall pour voir immédiatement l'effet d'un réglage.

Les deux sont pratiques pour régler un filtre/EQ/FX à l'œil, notamment avec les
instruments de bruit (catégorie NOISE : WHT/PINK/BRWN) dont le spectre plein-bande
rend immédiatement visible la réponse en fréquence de l'effet.

## Build d'un .exe Windows (cross-compilation depuis Linux)

Un exécutable Windows tout-en-un (statiquement lié : ni SDL2.dll, ni libstdc++/libwinpthread
à côté) peut être compilé depuis Linux via MinGW-w64 :

```bash
cd simulator
./build_windows.sh
```

Au premier lancement, le script télécharge et extrait (sans root/sudo) le compilateur croisé
et les fichiers de dev SDL2 dans `simulator/.winbuild-toolchain/` (~250 Mo, ignoré par git) ;
les lancements suivants réutilisent ce cache. Le résultat est `simulator/GrvEP.exe` — un seul
fichier à copier sur une machine Windows et à double-cliquer, sans installation.

Un exécutable déjà compilé est disponible dans
[releases/GrvEP-windows.exe](releases/GrvEP-windows.exe).

La « carte SD » simulée pointe vers le vrai dossier Musique de l'utilisateur Windows (résolu
via l'API `SHGetKnownFolderPath`, donc correct même si OneDrive redirige ce dossier ailleurs)
— mêmes fichiers/sous-dossiers qu'on y placerait sur Linux/Android dans `~/Music`.

Une fenêtre console s'ouvre à côté de l'appli et affiche en direct tous les messages de debug
(chargement SD, conversion d'images, erreurs `IMG:`/`WAV:`/`CACHE:`/etc.) — pratique pour
diagnostiquer un souci de lecture de fichier ; fermer cette fenêtre ferme aussi l'appli.

La lecture de fichiers `.mp3` fonctionne aussi sous Windows (décodeur `minimp3` embarqué
directement dans l'exécutable, aucune installation requise) — voir
[simulator/sim_mp3dec.cpp](simulator/sim_mp3dec.cpp).

## Build et installation de l'APK Android

L'APK embarque directement le simulateur (SDL2 + C++, sans WebView) : `libmain.so` contient
le firmware complet (SDL2 + moteur audio AMY + rendu U8g2), affiché en résolution logique
820×560 mise à l'échelle sur l'écran du téléphone. Les touches, le joystick et les potards
sont pilotés au tactile.

Un APK déjà compilé est disponible dans [releases/GrvEP.apk](releases/GrvEP.apk) — il suffit de le
copier sur le téléphone, d'activer "Sources inconnues" dans les paramètres de sécurité, puis
de l'ouvrir pour l'installer (ou `adb install -r releases/GrvEP.apk`).

Pour le (re)compiler soi-même :

```bash
# Première fois seulement : télécharge JDK 21, NDK 26.1 et les sources SDL2
cd android-native
./setup_android.sh
cd ..

# Build (debug, signé avec la clé de debug — installable directement)
./build_native_apk.sh

# Build + installation directe via adb (téléphone branché en USB)
./build_native_apk.sh --install

# Build release (non signé)
./build_native_apk.sh --release
```

L'APK compilé se trouve dans `android-native/app/build/outputs/apk/debug/app-debug.apk`
(ou `release/app-release-unsigned.apk`). Voir [android-native/README.md](android-native/README.md)
pour le détail de l'architecture.
