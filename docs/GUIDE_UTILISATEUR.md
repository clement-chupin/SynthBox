---
title: "GrvEP — Guide d'utilisation"
subtitle: "Boîte à rythmes / synthétiseur portable"
author: "GrvEP"
date: \today
geometry: margin=2.2cm
toc: true
toc-depth: 2
colorlinks: true
---

<!--
  Ce document est écrit pour être lu tel quel en Markdown, mais aussi converti en PDF
  proprement, par exemple avec pandoc :

      pandoc docs/GUIDE_UTILISATEUR.md -o GrvEP-guide.pdf \
             --toc --number-sections -V geometry:margin=2.2cm

  Le bloc YAML ci-dessus fournit déjà titre/sous-titre/page de garde/table des matières
  à pandoc ; les options en ligne de commande ne sont utiles que si vous préférez ne pas
  toucher au fichier.
-->

# Bienvenue

GrvEP est une boîte à rythmes / synthétiseur portable construite autour d'un ESP32-S3 :
séquenceurs, synthés (soustractif, FM, wavetable, TB-303, granulaire…), lecteur de
samples SD, écran OLED, clavier de 32 touches, joystick, 7 potentiomètres et LEDs
adressables pour le retour visuel en temps réel.

Le même firmware tourne sur trois cibles : la carte ESP32-S3 réelle, un simulateur
desktop (Linux/Windows) et une application Android — tout ce que ce guide décrit
s'applique aux trois de la même façon, seule l'interaction physique change (vraies
touches/pots vs. clavier+souris vs. tactile).

Ce guide couvre les contrôles physiques, la navigation dans les menus, les concepts
communs à tous les modes (gammes, enveloppes, effets), un panorama des familles de
modes, puis un zoom détaillé sur les deux ajouts les plus récents et les plus riches :
le **synthé modulaire wavetable** et les **modes expérimentaux génératifs**.

\newpage

# Les contrôles physiques

```
   ┌───┬───┬───┬───┐
   │B1 │B2 │B3 │B4 │        ┌────────────────────────┐
   └───┴───┴───┴───┘        │        Écran OLED       │
   ↑ boutons (haut-gauche)  │        128 × 128        │
                            └────────────────────────┘

   ┌───┬───┬───┬───┬───┬───┬───┬───┐
   │   │   │   │   │   │   │   │   │  ← rangée 3 (aiguës)
   ├───┼───┼───┼───┼───┼───┼───┼───┤
   │   │   │   │   │   │   │   │   │  ← rangée 2
   ├───┼───┼───┼───┼───┼───┼───┼───┤        32 touches
   │   │   │   │   │   │   │   │   │  ← rangée 1        (clavier / séquenceur /
   ├───┼───┼───┼───┼───┼───┼───┼───┤         sélecteur selon le mode)
   │   │   │   │   │   │   │   │   │  ← rangée 0 (graves)
   └───┴───┴───┴───┴───┴───┴───┴───┘

     (P1)              (●)              (P3)   (P4)(P5)(P6)(P7)
    Volume          Joystick            BPM     paramètres du mode
```

| Contrôle | Rôle |
|---|---|
| **Grille 4×8** | 32 touches jouables. Selon le mode : clavier musical, grille de séquenceur, sélecteur de motifs (LIFE), points de spawn (SWARM)… |
| **4 boutons (B1-B4)** | Placés physiquement en haut à gauche, à côté de l'écran OLED (électriquement câblés sur la même matrice de touches, rangée 4 / colonnes 4-7 — transparent pour l'utilisateur). Rôle contextuel selon le mode — le plus souvent B1 = ouvrir/fermer le menu FX. |
| **Joystick + clic** | Contrôle continu (X/Y) contextuel — hauteur/vitesse/direction selon le mode — plus un appui (clic) pour valider/sélectionner. |
| **P1 (Volume)** | Toujours le volume général, quel que soit le mode. |
| **P3 (BPM)** | Toujours le tempo, quel que soit le mode. |
| **P2, P4, P5, P6, P7** | Contextuels — leur rôle change selon le mode actif ; le menu et l'écran OLED en rappellent l'usage courant. |
| **LEDs sous les touches** | Retour visuel synchronisé avec l'état du mode (touche active, séquenceur en cours, hauteur de note par teinte…). |

\newpage

# Naviguer dans les menus

Le menu principal est organisé en **3 onglets** :

- **INSTR** — les instruments/synthés jouables au clavier (SYNTH, STONE, MODULAR, les
  modes expérimentaux…).
- **SEQNC** — les séquenceurs pas-à-pas (DRUM2, DR2, SYSEQ, 303S, SS2, GEST…).
- **AUTRE** — utilitaires et extras (LIGHT, animations, import SD, MIDI…).

Le joystick navigue : axe X pour changer d'onglet (sur la barre d'onglets) ou de colonne
(dans la grille), axe Y pour descendre dans la grille ou remonter à la barre d'onglets ;
un clic sur un élément l'ouvre. Un appui bref sur B1 depuis un mode ouvre en général la
**grille d'effets** de ce mode plutôt que de retourner au menu (voir plus bas) — pour
revenir réellement au menu principal, il y a un raccourci dédié par appui long sur B1
selon les modes, ou en sortant du sous-menu affiché à l'écran.

\newpage

# Concepts communs à (presque) tous les modes

## Gammes

La plupart des modes proposent 4 gammes au choix : **Chromatique**, **Majeure**,
**Mineure**, **Pentatonique** — pratiques pour improviser sans fausse note, en particulier
dans les modes expérimentaux où l'on ne « pense » pas forcément en notes.

## Enveloppes

Plusieurs préréglages d'enveloppe (attaque/chute/maintien/relâchement) sont disponibles
dans la majorité des modes synthé : **Fast** (percussif), **Normal**, **Pad** (lent,
tenu), **Pluck** (attaque immédiate, chute rapide, pas de maintien).

## La grille d'effets (FX)

Un appui bref sur **B1** (depuis SYNTH, POKEMON, I303, STONE, OMNI, MODULAR, GRANULAR2,
DR2 ou GEST) ouvre une grille de **15 effets** (dans une grille 4×4 de 16 cases, la
dernière reste vide) : filtre (LPF/HPF/BPF/LADDER résonant-saturant), distorsion/overdrive,
réverbe, chorus, flanger, delay, tremolo/autopan (modulation), EQ 3 bandes, écho résonant,
wavefold, bitcrush, ring modulator, compresseur.

- Une touche de la grille **active/désactive** l'effet correspondant (appui court).
- Une fois un effet actif, les pots **P4-P7** règlent ses paramètres (le nom de chaque
  paramètre s'affiche à l'écran au-dessus du pot correspondant).
- **Un appui long** sur un effet ouvre un éditeur d'**automatisation** : le joystick
  (axe X) choisit quel paramètre de l'effet moduler, puis les pots règlent profondeur
  (P4), vitesse (P5 — un seul potard continu, des subdivisions lentes synchronisées au
  BPM jusqu'au Hz libre rapide) et forme d'onde (P6 : sinus/triangle/carré/aléatoire
  échantillonné). Un indicateur de phase en direct à l'écran montre la modulation
  tourner pendant qu'on la règle. N'importe quelle touche referme l'éditeur et revient
  à la grille FX normale. Appuyer long sur un effet **inactif** l'active directement et
  ouvre l'éditeur, en un seul geste.

\newpage

# Panorama des familles de modes

## Synthés « classiques »

**SYNTH**, **OMNI**, **I303**, **MOD2**, **POKEMON** — clavier polyphonique classique,
35+ timbres AMY (formes d'onde de base, FM, patches Juno/DX7, presets thématiques…),
gammes/octave/arpégiateur, accès à la grille FX complète.

## Séquenceurs pas-à-pas

**DRUM2** (boîte à rythmes 8 pads), **DR2** (séquenceur batterie hiérarchique 64 pas),
**SYSEQ** (séquenceur synthé polyphonique 16 pas), **303S** (séquenceur TB-303 avec
accent/glissando), **SS2** (séquenceur d'échantillons 16 pas), **GEST** (gestionnaire de
motifs — copier/coller entre séquenceurs, mode LOOP/LIVE).

## Samples & granulaire

**SAMPLE** (lecture directe de fichiers SD sur le clavier), **STONE** (un seul sample SD
réparti/pitché sur tout le clavier), **GRANULAR2** (lecture granulaire multi-échantillon,
avant/arrière, découpage par pad).

## Synthé modulaire wavetable

**MODULAR** — voir le zoom dédié ci-dessous.

## Modes expérimentaux / génératifs

**EXP**, **EXP2**, **EXP3**, **LIFE**, **SWARM** — voir le zoom dédié ci-dessous.

## Utilitaires

**LIGHT**/**LIGHTPLAY**/**LANIM**/**ANIM** (light shows et animations), **VID**
(lecteur MEDIA 1-bit — vidéos `.bvid`, photos, **`.gif` animés** convertis automatiquement,
aperçu audio `.wav`/`.mp3`), **MIDI** (périphérique MIDI USB), **TRACKER** (enregistreur
quantisé), **PCMCLEAN** (nettoyage du cache SD), **IMPORT** (Android : import de
fichiers depuis le téléphone), **BATTERY**/**SYSINFO** (diagnostics).

\newpage

# Zoom : le synthé modulaire wavetable

C'est la refonte la plus profonde du mode **MODULAR**, inspirée d'outils comme Serum :
deux oscillateurs qui parcourent des **wavetables** (des tables de 64 formes d'onde à
travers lesquelles on peut « morpher » en continu) plutôt qu'une simple forme d'onde
fixe — de quoi obtenir des textures qui bougent et évoluent, pas juste un son statique.

## Les 5 wavetables disponibles

`111`, `BRAIDS01`, `PPG_WA00`, `SINE2SAW`, `VIRAL` — chacune propose 64 formes d'onde
différentes à parcourir en continu.

## Les contrôles (P2, P4-P7)

| Pot | Rôle |
|---|---|
| **P2** | Choix de la wavetable, partagée par les deux oscillateurs. |
| **P4** | Position de morphing de l'oscillateur A dans la table (0-100%). |
| **P5** | Position de morphing de l'oscillateur B (indépendante de A — même avec la même table, deux positions différentes créent un effet d'épaisseur/battement). |
| **P6** | Cutoff du filtre partagé. |
| **P7** | Profondeur du LFO qui fait onduler automatiquement la position de l'oscillateur B autour de sa valeur réglée par P5 — c'est ce qui donne ce mouvement « vivant » caractéristique des synthés wavetable. |

L'oscillateur B a un léger désaccord fixe par rapport à A, pour un effet de chœur/
épaisseur discret même sans toucher aux pots.

## Astuces de départ

1. Jouez une note tenue, montez **P7** progressivement — écoutez le timbre onduler
   sans bouger d'autre pot : c'est le LFO qui fait le travail.
2. Réglez **P4** et **P5** à des valeurs bien différentes pour un son plus large/détuné,
   ou proches pour un son plus focalisé.
3. B1 ouvre la grille FX habituelle — un filtre LADDER résonant ou un delay synchronisé
   au BPM se marient particulièrement bien avec les textures wavetable.
4. Essayez chaque wavetable au même réglage de position pour entendre à quel point le
   caractère change — `BRAIDS01`/`VIRAL` sont plus riches/métalliques, `SINE2SAW` est
   plus doux en début de table et se durcit vers la fin.

\newpage

# Zoom : les modes expérimentaux et génératifs

Philosophie commune à cette famille : produire des sons et des rythmes intéressants
**sans connaissance de théorie musicale** — on manipule un système (physique, cellulaire,
comportemental) et on écoute ce qu'il en ressort, plutôt que de « jouer » des notes au
sens classique. Ils s'opposent en cela à des modes comme SYNTH ou 303S, plus proches
d'un instrument traditionnel.

## EXP — Thérémine

Le joystick pilote une position continue : l'axe Y choisit la hauteur (quantifiée à la
gamme choisie), l'axe X la vitesse d'un arpège automatique. Les colonnes de la grille
choisissent forme d'onde, enveloppe, mode d'arpège, gamme, octave et un masque d'effets.

## EXP2 — PolyBounce

Jusqu'à 4 balles rebondissent dans un polygone tournant (3 à 8 côtés) avec une vraie
simulation physique (gravité pilotée au joystick, rebonds élastiques, collisions entre
balles). Chaque rebond sur un bord déclenche une note dont la hauteur dépend du bord
touché.

## EXP3 — Orbital

8 balles (une par colonne du clavier) orbitent autour d'un centre, chacune sur l'un des
4 rayons disponibles, à une vitesse angulaire liée au BPM. Franchir une zone de
déclenchement fixe joue une note — plus l'orbite est « rapide » (rayon intérieur), plus
elle déclenche souvent.

## LIFE — Jeu de la vie de Conway *(nouveau)*

La grille de touches devient un automate cellulaire : chaque case est vivante ou morte,
et évolue selon les règles classiques du Jeu de la vie (une cellule vivante avec 2 ou 3
voisines vivantes survit, une cellule morte avec exactement 3 voisines vivantes naît) —
avec une variante « HighLife » disponible en plus de la règle classique. La grille est
torique : les bords se rebouclent, pas de bordure morte sur un petit plateau 4×8.

- **Appuyer sur une touche** fait naître ou meurt une cellule (ensemencement manuel).
- Chaque **colonne** correspond à un degré de gamme, chaque **ligne** à une octave.
- Seules les **naissances/morts** déclenchent une note (pas chaque cellule vivante à
  chaque cycle), et une seule voix sonne par colonne à la fois — le résultat reste
  musical même quand la grille est très active.
- Pots : variante de règle, vitesse des cycles (calée sur le BPM), réensemencement
  aléatoire à une densité choisie, style d'enveloppe.

*Astuce* : ensemencez une petite zone dense au centre et laissez tourner — la plupart
des configurations aléatoires finissent par se stabiliser ou s'éteindre après quelques
dizaines de cycles ; réensemencez (pot dédié) pour relancer une texture différente.

## SWARM — Essaim de boids *(nouveau)*

Un essaim de jusqu'à 8 « boids » (agents autonomes) se déplace selon les trois règles
classiques du flocking : **cohésion** (se rapprocher du centre du groupe local),
**séparation** (éviter de se percuter), **alignement** (adopter la vitesse moyenne du
voisinage) — un comportement de groupe organique et imprévisible, différent du rebond
rigide d'EXP2.

- Le **joystick** pilote une zone d'attraction qui attire doucement l'essaim et sert de
  zone de déclenchement : chaque boid qui y entre joue une note.
- **Appuyer sur une touche** de colonne active/désactive le boid correspondant.
- Pots : équilibre cohésion/séparation (essaim compact ou dispersé), vitesse maximale,
  force d'attraction du joystick, taille de la zone de déclenchement.

*Astuce* : bougez le joystick lentement en cercle pour « rassembler » l'essaim et
obtenir des déclenchements de notes en rafale groupée, plutôt qu'éparpillés.

\newpage

# Pour aller plus loin

- `structure/SOFTWARE.md` — architecture logicielle détaillée, pour qui veut modifier
  le firmware.
- `README.md` — instructions de build pour les trois cibles (ESP32, simulateur
  desktop, Android).
- Le simulateur desktop ouvre en plus deux fenêtres d'analyse audio (spectrogramme et
  courbe EQ temps réel) très utiles pour régler un filtre ou un effet à l'oreille *et*
  à l'œil — voir le README pour les détails.

Bon jeu !
