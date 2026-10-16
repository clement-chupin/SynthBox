# Passation — USB Audio (micro USB pour enregistrer GrvEP sur PC)

**État au moment de la rédaction : abandonné pour cette session, firmware réel restauré et fonctionnel (MIDI+MSC, aucune trace du travail ci-dessous dans le code actuel).** Tout ce qui suit est à reprendre de zéro ou à partir des prototypes isolés encore présents sur disque — rien n'est resté dans le firmware principal.

## Objectif demandé par l'utilisateur

Faire apparaître GrvEP comme un micro USB standard sur le PC, pour pouvoir enregistrer directement
ce qui sort du synthé (au lieu de passer par un enregistreur externe sur la sortie jack), **sans
perdre le MIDI existant** (`MODE_MIDI`) ni le son physique (jack/haut-parleur).

## Ce qui a été établi comme solide (reproductible, à ne pas re-découvrir)

1. **`USBAudioCard` natif d'arduino-esp32 (pas de dépendance tierce) : capture cassée.**
   Bug confirmé, reproductible avec un simple ton synthétique (pas de dépendance matérielle
   I2S), sur ce board précis. Correspond à un ticket GitHub ouvert et non résolu :
   `espressif/arduino-esp32#12518`. Ne pas perdre de temps à re-tester cette voie telle
   quelle — elle ne fonctionnera pas pour la direction capture (device→host).

2. **`pschatzmann/arduino-audio-tools` (bibliothèque tierce) : la bonne voie technique.**
   Son `USBAudioStream` sur ESP32 utilise en interne le **même** mécanisme officiel
   (`tinyusb_enable_interface`) que les classes natives (`USBMIDI`, `USBMSC`) — ce n'est
   PAS une pile USB séparée, contrairement à ce qu'on pourrait croire en lisant son code
   (le `TinyUSBDevice` qu'elle expose est un shim qui appelle `USB.begin()` au final).
   Confirmé fonctionnel : un ton 440Hz généré a été capturé proprement (vérifié
   échantillon par échantillon) à plusieurs reprises tôt dans la session de debug.

3. **MIDI + Audio ne peuvent PAS être déclarés simultanément dans le même boot.**
   Les deux utilisent le même mécanisme d'enregistrement d'interface officiel, et pourtant
   les déclarer ensemble casse systématiquement le flux isochrone Audio — même après avoir
   corrigé une vraie collision d'endpoint (une interface CDC fantôme qui volait un
   endpoint). Cause profonde non isolée ; suspicion de contention de ressource
   FIFO/endpoint au niveau TinyUSB/DWC2 spécifique au mélange isochrone+natif sur cette
   version du core. `arduino-esp32` n'expose pas de `USB.end()` pour reconstruire le
   descripteur composite au runtime — donc pas de vrai "hot swap" possible en cours de
   session.
   → **Architecture retenue** (validée avec l'utilisateur, jamais remise en cause) :
   un seul des deux est enregistré par boot, choisi par un flag en **mémoire RTC**
   (`RTC_DATA_ATTR`, survit à un `ESP.restart()` logiciel mais pas à une vraie coupure
   d'alimentation — donc le firmware se réveille toujours en mode Audio par défaut).
   Entrer/sortir de `MODE_MIDI` bascule le flag et redémarre (quelques secondes,
   acceptées explicitement par l'utilisateur : "même si ça nécessite de rebrancher
   l'ESP32, audio par défaut, midi via le mode, automatiquement au mieux, avec
   rebranchement au pire").

4. **MSC (stockage) coexiste bien avec Audio.** Testé explicitement en isolation (MSC+Audio
   ensemble) : même profil de réussite intermittente qu'Audio seul, PAS l'échec
   systématique vu avec MIDI+Audio. Donc MSC n'a pas besoin du même traitement
   mode-gated — il peut rester déclaré en continu comme aujourd'hui.

## Les 3 bugs réels trouvés et corrigés (dans cet ordre, sur le vrai matériel)

### Bug 1 — blocage total au boot (aucune énumération USB)
**Cause** : `audioInit()` (qui démarre la tâche de rendu AMY, laquelle appelle le hook USB à
chaque bloc, ~350×/s) était appelée **avant** l'initialisation de `USBAudioStream`. La tâche de
rendu démarre sur Core 0 et touche un état TinyUSB partagé pendant que celui-ci est encore en
cours de construction sur l'autre cœur — course suspectée, jamais confirmée avec certitude
(pas de visibilité Serial, voir plus bas).
**Fix validé** : initialiser l'USB (Audio ou MIDI selon le flag RTC) **avant** `audioInit()`
dans `setup()`, pas après. Une fois ce réordonnancement fait, l'énumération a fonctionné à
chaque boot testé.

### Bug 2 — le son physique (jack/haut-parleur) devient muet
**Cause** : `usbAudioFeed()` appelait `sUac->write()` **directement depuis la tâche de rendu
temps réel AMY**, juste avant `amy_i2s_write()` (la sortie physique normale), dans la même
itération de boucle. `write()` peut bloquer plusieurs ms si l'hôte USB ne vide pas assez vite
son tampon interne (`SynchronizedNBufferRTOS`, comportement documenté par la bibliothèque
elle-même) — ce blocage volait exactement le budget de temps dont `amy_i2s_write()` avait
besoin juste après, et le son physique s'est tu.
**Fix validé** : ne jamais appeler `write()` depuis la tâche de rendu. `usbAudioFeed()` doit
se contenter d'un dépôt **non-bloquant garanti** (coût fixe) dans une file FreeRTOS
(`xQueueSend(..., 0)` — délai zéro, perte du bloc si pleine plutôt que d'attendre). Le son
physique a été reconfirmé fonctionnel par l'utilisateur après ce fix.

### Bug 3 — le son physique redevient muet avec une variante différente
Après le fix du bug 2, une première implémentation faisait vider la file côté `loop()`
(`usbAudioDrainTick()`, appelée à chaque itération de `loop()`, qui draine tout ce qu'il y a
d'un coup). L'utilisateur a signalé que le son physique s'était **encore** coupé après le
passage à une tâche dédiée (`usbAudioDrainTask`, qui bloque sur `xQueueReceive(...,
portMAX_DELAY)` plutôt que d'être pollée). **Cause non confirmée** — hypothèse faible
(contention sur le Core 1, où tournent `setup()`/`loop()`) jamais vérifiée faute de
visibilité. C'est le point où la session s'est arrêtée sur l'intégration réelle : **tout a
été annulé avant de comprendre la vraie cause de ce 3e échec.**

## Le vrai mur rencontré : fiabilité de la capture, indépendante du code

En repartant d'un prototype minimal isolé (juste un ton généré en continu, aucun MIDI, aucun
MSC, aucune tâche GrvEP) — donc en éliminant toute variable liée à l'intégration — la capture
est devenue **totalement non fiable (0 succès sur plusieurs séries de 6 tentatives)**, y
compris après :
- débranchement/rebranchement physique du câble USB,
- **redémarrage complet du PC** (confirmé par les numéros de périphérique USB repartis à zéro),
- retrait d'une modification de nom de périphérique (testé comme variable suspecte, sans effet).

Plus tôt dans la même session, ce même genre de prototype minimal avait capturé un son
propre et vérifiable à plusieurs reprises (succès ~50% des tentatives, le reste en erreur
`Input/output error` côté `arecord`). Cette dégradation progressive dans le temps, non
corrigée par un reboot complet, pointe vers quelque chose d'extérieur au code : **très
probablement le hub USB physique dans le chemin** (confirmé via `lsusb -t` :
`Port 013, Class=Hub` entre GrvEP et le PC) — l'audio isochrone plein-débit est connu pour
être sensible aux hubs/câbles/contrôleurs USB d'une manière dont le MIDI (bulk) ne souffre pas.

**Piste non testée, à essayer en premier à la reprise** : brancher GrvEP sur un port USB qui
ne passe PAS par ce hub (port natif de la carte mère/du boîtier PC, pas un hub externe ni un
dock). Si la fiabilité s'améliore nettement, le code ci-dessus (déjà validé en grande partie)
redevient directement exploitable. Si le problème persiste même en direct, il faudra
probablement explorer :
- un câble USB différent (qualité/longueur),
- tester sur un port USB2 natif plutôt qu'un port USB3 (parfois moins capricieux avec de
  l'audio full-speed),
- vérifier si l'hôte (ce PC précis) a des soucis connus avec l'USB Audio Class isochrone sur
  d'autres périphériques, pour savoir si c'est GrvEP ou l'hôte qui est en cause.

## Obstacle méthodologique à garder en tête pour la suite

**Aucune visibilité Serial sur le firmware réel.** `ARDUINO_USB_CDC_ON_BOOT=0` est nécessaire
pour que MIDI/Audio fonctionnent, ce qui fait que `Serial.print` sort sur UART0 (broches
matérielles), invisible sans adaptateur USB-UART dédié câblé sur la carte. Tout le debug des
bugs 1-3 ci-dessus s'est fait à l'aveugle (hypothèses + essais/erreurs + un début de
checkpoints visibles à l'écran OLED, jamais poussé jusqu'au bout). **Avant de retenter une
intégration dans le firmware réel**, la proposition faite à l'utilisateur (acceptée en
principe, jamais réalisée) était de faire un build de diagnostic temporaire avec
`ARDUINO_USB_CDC_ON_BOOT=1` (MIDI/Audio désactivés le temps du diagnostic, mais de vrais logs
Serial visibles via `pio device monitor`) pour comprendre precisément la cause du bug 3 avant
de reconstruire par-dessus des preuves plutôt que des suppositions.

## Fichiers / code de référence (pour repartir vite)

Rien ne subsiste dans le dépôt GrvEP lui-même (tout annulé). Mais le **code source complet de
la dernière version tentée** (celle avec le bug 3, jamais réparée) a été donné en entier dans
la conversation qui a précédé cette passation — à reconstituer depuis l'historique de
conversation si besoin, ou à réécrire à partir des principes ci-dessus (ils sont corrects, seul
le détail du bug 3 doit être ré-exploré avec de vrais logs).

Le prototype isolé minimal (celui utilisé pour le dernier test de fiabilité, 0/6) existe
toujours sur disque, séparé du dépôt GrvEP :
- `~/projects/esp32-usb-uac-test/` — projet PlatformIO autonome, cible `esp32-s3-devkitc-1`,
  dépendance `https://github.com/pschatzmann/arduino-audio-tools.git`. `src/main.cpp`
  actuel : génère un ton 440Hz stéréo en continu via `USBAudioStream`/`StreamCopy`, rien
  d'autre (pas de MIDI, pas de MSC). C'est le point de départ recommandé pour re-tester la
  fiabilité une fois le port USB direct essayé.

## Composants techniques validés individuellement (réutilisables tels quels)

- **`AudioInfo(44100, 2, 16)`** — 44.1kHz stéréo 16-bit, correspond exactement à
  `AMY_SAMPLE_RATE`/`AMY_NCHANS` de GrvEP (128 échantillons/bloc × 2 canaux × 2 octets =
  512 octets/bloc) — pas de ré-échantillonnage nécessaire pour la vraie intégration.
- **Point d'injection côté AMY** : `lib/AMY Synthesizer/src/i2s.c`, fonction
  `esp_fill_audio_buffer_task()`, juste avant l'appel à `amy_i2s_write()` (même buffer
  `block`, mêmes dimensions). Un hook à lien faible (`__attribute__((weak))`) y a été ajouté
  à chaque tentative, pour ne pas coupler la copie vendue d'AMY à une API spécifique à
  GrvEP — c'est le bon pattern, à reproduire.
- **`usbMIDI`/`USBMSC msc` dans GrvEP sont tous les deux des objets globaux construits au
  démarrage statique (avant `setup()`)** — c'est ce qui empêche un simple flag runtime de
  désactiver MIDI : son constructeur appelle déjà `tinyusb_enable_interface()` avant que
  quoi que ce soit dans `setup()` ne s'exécute. La solution qui marche : remplacer l'objet
  global par un pointeur (`USBMIDI *usbMIDI = nullptr;`), construit seulement dans une
  fonction appelée conditionnellement depuis `setup()` (`midiUsbInitIfNeeded()`), avec
  toutes les fonctions `midiNoteOn()`/etc. gardées par un `if (usbMIDI)`.
