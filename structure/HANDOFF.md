# GrvEP — Dossier de passation

> Écrit pour une IA (ou un dev) qui reprend le projet sans historique de conversation. Ordre de
> lecture : `/CLAUDE.md` (court, chargé automatiquement) → **ce fichier** (état, règles de
> collaboration, pièges, travail en attente) → `SOFTWARE.md` / `TEMPLATES.md` / `SIMULATOR.md` /
> `HARDWARE.md` selon la zone touchée. Les identifiants de code sont donnés par **nom de
> symbole** (les numéros de ligne de `main.cpp` bougent à chaque session : faire un `grep`).

---

## 1. Le projet en dix lignes

- **GrvEP** : groovebox/synthé portable sur **ESP32-S3** (clavier 4×8 via TCA8418, OLED 128×128
  SH1107, bande de 36 LEDs SK6812, joystick, **7 potards à encodeur** via mux, carte SD, sortie
  I2S stéréo). 46 modes (`enum AppMode`, `config.h`) : synthés, séquenceurs, samplers, modes
  génératifs, utilitaires visuels.
- Moteur de synthèse : **AMY** (`lib/AMY Synthesizer/src/`, copie **unique** partagée par toutes
  les cibles). Le même code applicatif (`src/`) tourne sur **quatre cibles** : ESP32 (PlatformIO),
  simulateur SDL2 Linux/Windows, APK Android, Web (WASM).
- Code applicatif : `src/main.cpp` (~15 000 lignes, **monolithique** : UI, logique de tous les
  modes, FX, boutons, pots, écran) + `src/audio_engine.cpp/.h` (toute la couche AMY : notes,
  FX, samples) + petits drivers. Il n'y a **pas de suite de tests**.
- Langue de travail avec le propriétaire : **français** (messages, commentaires de commit). Le code
  et `CLAUDE.md` sont en anglais/français mélangés ; suivez le style local.

## 2. Règles de collaboration (déduites de la session, à respecter)

1. **Ne jamais commit/push sans demande explicite** — même après un gros chantier. Rapporter, attendre.
   (Message de commit, quand demandé : se terminer par la ligne `Co-Authored-By:` du modèle.)
2. **« Fait » = compile proprement sur les deux cibles touchées ET vérifié dans le simulateur**
   (mesure, capture d'écran ou log), pas seulement « ça compile ». Fin de tâche standard :
   rebuild propre simulateur (`cd simulator && rm -rf build && ./build.sh`) + ESP32
   (`pio run -e esp32-s3-devkitc-1`), `grep -rn TEMP_DBG` vide, `pgrep -af grvep_sim` vide.
3. **Cause racine, pas rustine.** Le propriétaire signale des *symptômes* (« ça ne fait rien »,
   « ça sonne désagréable ») ; les vraies causes trouvées cette session étaient presque toujours
   ailleurs que l'hypothèse de départ (voir §5). Mesurer avant de corriger. S'il y a 3+ rustines
   sur une même fonctionnalité, revoir la structure (retour explicite du propriétaire).
4. **Refus explicite des « modifications bizarres »** : après un correctif ponctuel (un LPF
   supplémentaire câblé « après ResEcho »), le propriétaire a redemandé le *mécanisme général*
   (FX chaînés dans l'ordre d'activation). Préférer la solution générique et cohérente avec le
   modèle mental de l'utilisateur, même si plus grosse.
5. **Portée** : ne pas ajouter de fonctionnalités/abstractions non demandées ; ne pas créer de
   documents non demandés. Réponses finales courtes : ce qui a changé, ce qui reste.
6. Le propriétaire teste surtout **sur le simulateur** ; le matériel réel n'est pas toujours
   disponible → dire clairement ce qui n'a été vérifié que sur simulateur.
7. Les fichiers « ouverts/sélectionnés dans l'IDE » qui accompagnent les messages
   appartiennent souvent à **d'autres projets** (ex. `damac_train_infer_visu`, `clavier_v2`) :
   bruit, à ignorer sauf si le message s'y réfère.
8. Mémoire persistante de Claude Code (hors dépôt) : `~/.claude/projects/-home-cchupin-projects-GrvEP/memory/`
   (préférences : push seulement sur demande ; simplifier plutôt que patcher).

## 3. État du dépôt à la date de passation

- **Passation du 2026-10-27 : tout est commité et poussé** sur `origin/main`
  (`git@github.com:clement-chupin/SynthBox.git`), y compris FX Punch phase 2 (moteur
  `amy_punch_*()`, entrée AT) et le correctif de sortie du menu FX (entrée AZ). L'arbre de
  travail est propre : un `git status` non vide en reprenant le projet signifie du travail
  nouveau, pas un reliquat. (Les sessions précédentes gardaient volontairement de gros
  chantiers non commités ; ce n'est plus le cas à cette date.)
- Dernières compilations vérifiées (2026-10-27) : simulateur Linux ✔, ESP32
  (`esp32-s3-devkitc-1`) ✔ (80,1 % flash, 38,9 % RAM), firmware flashé et utilisé sur la carte
  du propriétaire (port `/dev/ttyACM1`, adaptateur WCH CH343). **Non revérifiés depuis la
  refonte FX/AMY** : APK Android, exécutable Windows, build Web (voir aussi AY : son réel du
  build Web jamais vérifié dans un navigateur). **Pas de mesure audio sur carte** de la chaîne
  FX de bus ni des effets looper phase 2 : seule l'interaction du bouton FX (AZ) a été
  vérifiée sur matériel.
- Après avoir vidé `.pio/build/…`, le **premier** `pio run` peut échouer avec une erreur SCons
  (`FortranCommon` / `.sconsign….tmp`) : relancer une seconde fois, c'est transitoire.

## 4. Ce qui a changé récemment (chronologie utile)

A. **Une seule copie d'AMY.** L'ESP32 utilisait un clone frère `../other_projects/amy`
   (`lib_deps`) pendant que le simulateur compilait `lib/AMY Synthesizer` — deux arbres qui
   avaient dérivé (l'ESP32 ne mergeait pas le bus 1 dans le bus 0, donc les FX de bus ne
   touchaient pas la TB-303 sur carte). Le clone est supprimé ; `platformio.ini` n'a plus
   d'entrée AMY (PlatformIO lit `lib/`) ; `simulator/hal/AMY-Arduino.h` inclut l'en-tête de `lib/`.
   **À vérifier sur carte** : son du 303 avec FX, CPU, PSRAM.
B. **Chaîne FX de bus dans l'ordre d'activation** (`amy_apply_ordered_fx`, `amy.c`) — FILT
   (LPF/HPF/BPF/LDR), DRIVE, RESECHO, REP/BITCRS, RINGMOD, COMPRESS. Les FX natifs AMY
   (REVERB/CHORUS/FLANGER/DELAY/EQ/RES) gardent un ordre fixe, toujours après. Détails et
   limites : SOFTWARE.md « Effets ».
C. **Refonte des FX** : DRIVE (fusion OVERDRV+DISTORT en un moteur à 4 algos), RESECHO
   réécrit (boucle résonante dédiée, paramètre « Wild » qui apprivoise/libère le même moteur),
   RES (ancien ResEcho conservé), AUTOPAN supprimé, COMPRESS corrigé (S2F), FILT converti en
   effet de bus avec filtre TPT stable et Res mémorisé par type. Interface FX refaite
   (noms en haut, paramètres du dernier FX activé en bas en 2×2, grille toujours complète).
D. **EUCLI** : colonne de réglage par piste (clic = modificateur, double clic = annule+quitte,
   appui long = reset), RANDPITCH centré sur le pitch par défaut, nombre de coups en
   proportion, pots rendus aux proportions à la sortie du menu FX.
E. **Potards FX relatifs** (plus de saut à 0 au premier tour) via `fxPotDeltaApply`, tous modes ; ResEcho re-réglé : Wild 0→1 va de doux à saturé-borné (voir SOFTWARE.md).
   **Divers** : LANIM remappé sur P2/P4/P5/P6 (P1/P3 sont réservés), menu bloqué après clic
   joystick pendant un overlay, curseurs de pots en grille 2×2 (LANIM, NOOB, panneau FX).
F. Docs : `CLAUDE.md` et `structure/` mis à jour (ce dossier).
G. **Nouveau mode MODE_AMBIENT** (synthé drone/pad, inspiré du site "Slide Synth Lab" de
   Casual Machines — moteur DSP du site compilé en WASM, non portable, seul le schéma de
   patch a servi d'inspiration). Réutilise intégralement le moteur de MODE_MODULAR (mêmes
   canaux `MOD3_OSCA_CH`/`MOD3_OSCB_CH`, mêmes `audioModular*()`, zéro nouveau code dans
   `audio_engine.cpp`) avec des défauts différents : reverb+chorus auto-activés à l'entrée
   (nouveau précédent — premier mode à toucher l'état FX global à l'entrée), enveloppe
   ENV_PAD, dérive de wavetable active par défaut (`gModSlots[4]`), détune d'oscillateur B
   plus large, mode drone/latch (double-clic B4 — bascule la note au lieu de note-on/held).
   Au passage : `modSlotApplyDirect()`/`modSlotRestore()` rendues sensibles au mode courant
   (le slot LFO `gModSlots[4]` est maintenant partagé par 2 modes, chacun avec sa propre
   position de base d'oscillateur B) ; et un vrai bug pré-existant corrigé dans la bande
   d'onglets partagée par MODE_MODULAR/MODE_AMBIENT (libellé de l'onglet sélectionné
   illisible, `setDrawColor` manquant). Détails : SOFTWARE.md.
H. **Équilibrage du volume des instruments** — `shapeGain[SHAPE_COUNT]` (`config.h`),
   mesuré par sweep headless (RMS/pic sur les 35 `SynthShape`, tap temporaire dans
   `libminiaudio-audio.c`) et appliqué uniformément dans `audioSetShapeOnSynth()`
   (remplace l'ancienne compensation ponctuelle limitée au seul JUNO_ORGAN). Atténuation
   uniquement (jamais de boost), normalisé sur le niveau de SAW.
I. **RESECHO : "Tone" → "Damp"**, résonance large bande au lieu d'un pic étroit — la
   boucle de feedback réinjecte maintenant la sortie passe-bas du SVF (`low`) au lieu de
   sa sortie passe-bande (`band*q`), compensée par un facteur `×0.22` (mesuré) pour rester
   dans la même plage de niveau qu'avant. Détails/mesures : SOFTWARE.md.
J. **Arpégiateur dans MODE_AMBIENT** (bouton ENV, B2) — étend `OVERLAY_ENV` à 4 colonnes
   uniquement en MODE_AMBIENT (opts 5-8 libres pour Up/Dn/UD/Rnd), corrige au passage un
   bug pré-existant (le sélecteur d'enveloppe ne touchait jamais `MODE_AMBIENT`, seulement
   `MODE_MODULAR`).
K. **MODE_NOOB2 / MODE_OMNI2** — NOOB et OMNI rejouant à travers le moteur ambiant, banque
   de 6 presets (`kAmbientPresets[]`) sélectionnée au P2 (repurposé, plus de sélection de
   `SynthShape`).
L. **INDUSTRIAL toujours trop fort** (rapporté après H) — le sweep de volume original
   sautait l'attaque, ratant le burst résonant qui fait tout le volume perçu du timbre.
   Re-mesuré note complète, `shapeGain[]` corrigé `0.82→0.34`.
M. **Boîte à rythme OMNI/OMNI2** — 5 patterns partagés (`kOmniRhythms[]`), B3 = play/stop,
   les 5 touches libres de la grille d'accords (rangée 0, colonnes 3-7) choisissent le
   pattern à chaud. Patterns re-conçus pour être vraiment distincts (caisse claire/perc/
   swing variables par style, pas juste le kick — rapporté comme "se ressemblent
   énormément"). Détails : SOFTWARE.md.
N. **Écran OMNI/OMNI2 : titre invisible + bande FX illisible** (rapporté : "l'affichage
   rentre en collision") — bug pré-existant de MODE_OMNI (confirmé par test A/B avant
   toute modification, hérité tel quel par OMNI2), corrigé cette fois : `setFont`
   explicite + titre à `y=7` au lieu de `y=0` (baseline sous une police laissée par l'écran
   précédent) ; bande de 14 cases FX (9px chacune — bien trop étroit) remplacée par une
   ligne unique `FX n/14:NOM [ON/off]`.
O. **Le son continuait après avoir quitté OMNI/OMNI2** — `audioAllNotesOff()` (appelée à
   chaque changement de mode) ne coupait pas la plage `DRUM_OSC_BASE` : un coup de la
   boîte à rythme encore en décroissance continuait de sonner dans le mode suivant. Les
   oscillateurs kick/snare/hat + perc sont maintenant coupés explicitement en quittant
   OMNI/OMNI2.
P. **"FX select" d'OMNI/OMNI2 retiré, remplacé par une banque de batterie** — B2 cyclait
   `fxSelected`, redondant avec B1 (`OVERLAY_FX`, la façon standard partout ailleurs).
   Cycle maintenant `omniDrumBank` (`kOmniDrumBanks[]`, 4 kits kick/snare/hat). Détails :
   SOFTWARE.md.
Q. **Certaines banques de batterie sonnaient à contre-temps** — pas un bug de placement
   rythmique (les banques ne touchent que le sample, pas le pattern) mais un vrai silence
   de tête très variable d'un sample PCM à l'autre : KK3 arrivait 165ms en retard (mesuré
   par tap headless), inutilisable comme kick. `kOmniDrumBanks[]` exclut maintenant KK3 et
   HH1 (54ms). Détails/mesures : SOFTWARE.md.
R. **`structure/AGENT.md`** ajouté — même index que README.md mais formulé comme
   instructions pour un agent IA (Claude Code ou autre), avec les non-négociables
   (copie AMY unique, pas de suite de tests automatisée, commit sur demande uniquement)
   répétés en tête.
S. **HOVR (et toute forme jouée en accord) trop fort en MODE_NOOB** — `nooTriggerVoice()`
   envoyait une vélocité fixe (`0.75f*volume`) sans aucune compensation de polyphonie,
   contrairement au clavier de MODE_SYNTH (qui divise par `sqrt(nombre de notes tenues)`).
   Un accord HOVR à 3 notes tapait `peak=32767` (écrêtage dur), confirmé par tap RMS/pic
   headless. Corrigé : `nooTriggerVoice()`/`noo2TriggerVoice()` prennent un `chordSize`
   fourni par l'appelant (pour ne pas sous-atténuer la 1ère note d'un accord simultané,
   comptée avant que ses notes sœurs ne soient ajoutées) et appliquent
   `1/sqrt(max(activeCount, chordSize))`. Même correction appliquée aux deux fonctions
   (MODE_NOOB et MODE_NOOB2 avaient chacune leur propre copie du bug).
T. **Presets ambiants (`kAmbientPresets[]`) mal calibrés entre eux** — certains
   instruments/notes de MODE_OMNI2 grésillaient (écrêtage) tandis que d'autres semblaient
   trop faibles. Cause : un filtre résonant **amplifie** près de la fréquence de coupure au
   lieu de seulement atténuer au-dessus — un preset à coupure basse mais résonance modérée
   (Sub : 500Hz/1.2) sonnait donc plus fort et plus écrêté qu'un preset grand ouvert (Glass :
   5500Hz/1.0), alors qu'intuitivement on s'attendrait à l'inverse. Mesuré par tap headless
   (accord à 3 notes identique rejoué sur les 6 presets, RMS/pic/nombre d'échantillons
   écrêtés) : écart de loudness ×2.2 et 3 presets sur 6 écrêtaient. Corrigé : champ `gain`
   ajouté à `AmbientPresetDef`, calibré sur le preset le plus calme (Glass=1.0, référence),
   atténuation uniquement (`DeepPad=0.52, Drone=0.78, Shimmer=0.77, Sub=0.46, Chorus=0.54`),
   appliqué comme multiplicateur de vélocité à l'accord de MODE_OMNI2 et à
   `noo2TriggerVoice()` (MODE_NOOB2). Re-vérifié après coup : RMS resserré (5220-6957 contre
   5767-12505 avant), `clipSamples=0` sur les 6 presets.
U. **LFO de MODE_OMNI2 remplacé par une enveloppe déclenchée à l'accord** — sur demande
   explicite (pas un bug) : MODE_OMNI2 partageait le LFO continu de dérive de wavetable
   (`gModSlots[4]`, aussi utilisé par MODE_AMBIENT/MODE_NOOB2), jugé trop aléatoire pour ce
   mode. Remplacé, pour MODE_OMNI2 uniquement, par une enveloppe exponentielle décroissante
   (`OMNI_ENV_TAU_MS=550ms`, ~3τ≈1.6s pour se stabiliser) redéclenchée à chaque pression
   d'accord (`omni2ChordPressMs`), qui pousse `MOD3_OSCB_CH` vers
   `ambOscBPos + depth*enveloppe` puis relâche vers la position de base du preset.
   `gModSlots[4]` reste totalement inerte en MODE_OMNI2 (confirmé `active=0` par test
   headless) ; MODE_AMBIENT et MODE_NOOB2 gardent le LFO d'origine, inchangé.
V. **P5/P6/P7 (OMNI/OMNI2) : volume du beat, profondeur d'enveloppe, modulation** — ces 3
   potards étaient inertes en dehors du rôle FX (paramètres 1-3 de l'effet sélectionné).
   Gagnent un second rôle, actif seulement quand aucun FX n'est actif (même convention que
   P4 pour "forme d'onde du strum" vs "paramètre FX 0") : P5 = multiplicateur de vélocité de
   la boîte à rythme (`omniRhythmVol`, 0.5 au centre = inchangé) ; P6 = profondeur de
   l'enveloppe de pression d'accord (`omniEnvDepth` — remplace l'ancienne constante fixe
   `OMNI2_ENV_DEPTH` pour MODE_OMNI2, et pilote un **pitch bend** pour MODE_OMNI via
   `audioSetPitchBend()`, choisi car universel sur les 35 `SynthShape` — contrairement à un
   balayage de filtre, inaudible sur la plupart d'entre elles dont le `filter_type` natif
   est `FILTER_NONE`) ; P7 = modulation continue lente de l'instrument sélectionné
   (`omniModAmt`, off par défaut — volontairement PAS le comportement de l'ancien LFO
   `gModSlots[4]` retiré ci-dessus, rien ne dérive tant que l'utilisateur ne monte pas le
   potard). Resynchronisés à l'entrée du mode (`pots[4].value=0.5`, `pots[5..6].value=0`).
   Vérifié par test headless : RMS du beat quasi doublé entre P5 bas/haut, décroissance de
   l'enveloppe de pitch bend conforme à ±1% à la formule analytique `exp(-t/550ms)`,
   modulation continue confirmée non nulle et variable dans le temps sans accord tenu.
W. **MODE_AMBIENT : notes aiguës beaucoup plus faibles que les graves** — `audioModularSetFilter()`
   (partagée par MODE_MODULAR/MODE_AMBIENT/MODE_NOOB2/MODE_OMNI2) ne posait qu'une coupure
   fixe en Hz, identique pour toutes les notes — sur un preset à coupure basse (Sub=500Hz),
   une note aiguë se fait couper bien plus fort par le LPF24 qu'une note grave. Mesuré : ×27
   d'écart de RMS entre C2 et C7 sur Sub. Fix : `filter_freq_coefs[COEF_NOTE]=1.0f` — AMY
   expose un keyboard tracking de filtre natif (coupure exprimée "à la note A4", qui suit
   ensuite la note jouée en espace log-fréquence) — réduit l'écart à ×1,5 sur Sub. Comme la
   fonction est partagée, les 4 modes en bénéficient d'un coup ; un preset très résonant
   (Drone, reso=2.2) garde un résidu d'irrégularité (résonance qui retombe sur des
   harmoniques différentes de la wavetable selon la hauteur), non lié au symptôme grave/aigu
   rapporté.
X. **"Accent" retiré des 303, B3 repurposé en sélecteur "Patch"** — sur demande explicite
   (pas un bug) : `t303AccentOn` (vélocité 0.85/0.5) supprimé partout (303S PAD/SEQ, 303S2,
   track 303 partagé GEST/DR2/SYSEQ — vélocité fixe 0.5 désormais). Le bouton B3 qui le
   togglait (303S vue PAD, 303S2) cycle maintenant `t303Patch` parmi 4 types de filtre
   (`24dB/12dB/BPF/HPF` via la nouvelle `audioT303SetFilterType()`, touch léger sur
   `filter_type` seul — ne touche jamais `wave`, qui reste au joystick). En vue SEQ de
   303S, B3 togglait NRM→ACC→SLD (3 états) ; devient NRM↔SLD (ACC retiré du cycle). État
   partagé comme `t303Wave`, donc réappliqué explicitement après chaque
   `audioT303Init()`/`audioI303Init()` (qui remet `filter_type` à LPF24) dans les hooks
   d'entrée 303S/303S2/I303/GEST. MODE_GROOVE's propre mécanisme NRM/ACC/SLD
   (`grvAlt[]`/`grvWriteAlt`, piloté par potard, pas bouton) volontairement laissé
   inchangé — hors périmètre de "la touche".
**⚠️ Entrées Y/AA/AB ci-dessous décrivent un mécanisme (split de clavier en rangées
SLIDE/NORMAL, bascule au B4) qui a été explicitement rejeté puis entièrement supprimé et
remplacé dans une session ultérieure — voir entrée AI. Gardées pour l'historique (le
mécanisme de glissando `t303SlideFrom/To/Ms/Active` qu'elles introduisent est, lui, resté
et toujours utilisé), mais ne plus s'y fier pour l'état actuel du code.**

Y. **MODE_303S2 : 2 rangées normales + 2 rangées slide** — sur demande explicite. Rangées
   2-3 = attaque franche (inchangé) ; rangées 0-1 = même hauteur que la rangée normale 2
   rangées plus haut (`noteMap.getMidiNote(row+2,col)`), mais glissent depuis la note
   précédente au lieu de redéclencher (réutilise `t303SlideFrom/To/Ms/Active`, déjà
   existant pour 303S). Nouveau buffer en anneau `s303s2Slide[]` parallèle à `s303s2Seq[]`
   (8 points de copie/sauvegarde GEST étendus pour le transporter). Touche REST
   inchangée. **2 bugs latents corrigés en cours de route** : (1) une touche slide et sa
   touche normale miroir partagent maintenant la même hauteur, donc le relâchement ne peut
   plus se fier à une comparaison de pitch — `s303s2PressedRow`/`Col` ajoutés (même motif
   que `t303PressedRow`/`Col` de 303S) ; (2) le tick d'interpolation du glissando était
   gardé par `currentMode==MODE_303S` uniquement, alors que le tick du séquenceur 303S2
   tourne sans condition de mode (comme DRUM2/SYSEQ/SS2) — un slide déclenché depuis
   303S2 ne se serait jamais interpolé si l'écran affiché n'était pas MODE_303S ; élargi à
   `t303SlideActive` seul.
Z. **Bug pré-existant trouvé en cherchant "303S2" dans le menu : mauvais libellé** —
   rapporté par l'utilisateur (« je ne vois pas le mode 303S2 »). `menuLabels[]`
   (`config.h`) affichait `"303S"` pour l'entrée `MENU_303S2` — copie-collée de l'entrée
   `MENU_303S` juste au-dessus (qui, elle, n'est même câblée dans aucun des 3 onglets du
   menu, donc jamais visible du tout). Résultat : la seule case liée au 303 réellement
   visible dans l'onglet SEQNC affichait "303S" au lieu de "303S2", donc introuvable sous
   ce nom. Fix : libellé corrigé en `"303S2"`. **Patchs 303 itérés** sur retour utilisateur
   juste après le ship initial — HPF retiré (« inutile »), Acid/Soft ajoutés avec un
   multiplicateur reso/envMod (pas seulement un `filter_type`, voir SOFTWARE.md) pour un
   vrai changement de texture.
AA. **Disposition split (slide) de 303S2 rendue optionnelle, bascule au B4** — sur demande
   explicite. La disposition 2 rangées normales + 2 rangées slide (voir entrée Y) devient
   off par défaut (pleine tessiture 31 notes restaurée) ; **B4 appui court** bascule
   `s303s2SplitRows` ; **B4 maintenu ≥2s** efface la séquence (ancien rôle unique de B4,
   maintenant réservé au maintien long — inversé une fois en cours de route sur retour
   utilisateur : la version initiale avait long=switch/court=clear). Implémenté tout en
   haut de `handleButton()`, avant le filtre générique qui bloque les évènements de
   relâchement pour la plupart des modes (même idiome que le long-press de B1 pour le
   menu) — nécessaire car la distinction court/long a besoin de voir appui ET relâchement,
   que le `switch(currentMode)` générique plus bas ne voit jamais pour ce bouton.
AB. **303S2 renommé "303S" à l'affichage** — sur demande explicite, pour finaliser le fait
   que ce mode (305S2, enrichi cette session : patches, split/slide) est désormais LE
   "303S" que l'utilisateur emploie réellement. Changement purement d'affichage
   (`menuLabels[]` + le titre dans `drawScreen()`) — les identifiants internes
   (`MODE_303S2`, `s303s2*`) restent inchangés (renommer ~50+ sites aurait été un risque
   disproportionné pour un changement cosmétique). `MODE_303S`/`MENU_303S` (l'original,
   jamais câblé dans aucun onglet de menu, donc jamais visible) garde aussi le texte
   "303S" dans `menuLabels[]` — intentionnel, documenté en commentaire, sans collision
   visible puisqu'il n'apparaît jamais à l'écran.
AC. **Chantier UI : rangée B1-B4 + popup potards P4-P7 — terminé.** Gros chantier demandé
   explicitement par l'utilisateur, en plusieurs passes. Réutilise `ctrlLabelsFor()`
   (jusque-là debug-only) pour une rangée positionnelle de boutons en haut d'écran
   (`drawBtnRow()`) et un popup générique pour P4-P7 (`potPopupShow()`, modelé sur le
   mécanisme déjà existant de DR2) qui s'efface 500ms après le dernier mouvement.
   **1ère passe** : SYNTH/SYSEQ, 303S, 303S2, I303, DRUM2, DR2, STONE — rangée casée là où
   il y avait de la place (parfois en haut, souvent ailleurs). **2e passe** (demandée
   explicitement par l'utilisateur après coup : il voulait la rangée *systématiquement* en
   haut d'écran, pas "là où ça rentrait", et avait repéré un hint résiduel redondant dans
   SAMPLE, "B2=mapAll", qui en plus était faux — le vrai bouton est B3) : tous les modes
   restants traités (MODULAR, AMBIENT, GROOVE, EUCLI, GEST, MOD2, OMNI/OMNI2, NOOB/NOOB2,
   GRANULAR2, SAMPLE, SS2, LIFE, SWARM, GEN, LANIM, + entrée `ctrlLabelsFor` manquante de
   DJ ajoutée), **et** la 1ère passe elle-même reprise pour remonter sa rangée en haut
   partout où elle ne l'était pas déjà. Deux exceptions délibérées, documentées dans
   SOFTWARE.md plutôt que forcées : **POKEMON** (sprite plein écran, rangée reste en bas)
   et **CRUNCH** (layout custom porté pixel-à-pixel d'un projet externe, déjà à son budget
   de 128px — aucune rangée B1-B4 dessinée du tout). Tout vérifié par capture d'écran
   headless (outils X11 classiques indisponibles sur ce Xvfb en bac à sable ; hook SDL
   temporaire utilisé à la place, retiré après usage — **attention**, le `#define` qui
   active ce hook doit être redéclaré localement dans CHAQUE fichier `.cpp` qui l'utilise,
   `sim_window.cpp` compris, sinon erreur de link). Plusieurs bugs de chevauchement entre
   colonnes trouvés et corrigés en cours de route (étiquette trop large qui déborde sur la
   colonne voisine, ou — cas moins évident — sur SA PROPRE colonne quand elle dépasse
   32px à elle seule et que le clamp anti-bord-écran la repousse vers la colonne voisine) :
   DR2 ("LOOP/LIVE"+"COPY/PASTE"), EUCLI (grille du bas qui chevauchait la rangée),
   SS2 ("ALT/AUTOMAP"), NOOB/NOOB2 ("PLAY/PAUSE"), GROOVE ("FX/SCALE"+"MUTE/SOLO"),
   MODULAR/AMBIENT ("OSCB WAVE", et pour AMBIENT en plus "OCT/DRONE" à elle seule) — détail
   complet et étiquettes finales dans SOFTWARE.md.
AD. **FELU étendu de 4 à 9 moteurs Felucca** — `kFeluAlgos[]`/`FELU_ALGO_COUNT`, browser
   `OVERLAY_FELU_ALGO` élargi en grille 4 colonnes pour les accueillir. Détails (approche
   AMY par moteur) : SOFTWARE.md.
AE. **MODE_303A/MODE_303B (X0X) construits, puis explicitement rejetés et supprimés** —
   sur demande (« tu peux ajouter les 303A,303B,808 et 909 de X0X »), scopé « simple »
   (2 instances T303 indépendantes, séquençage manuel façon 303S2). Construits en entier
   (`T303InstSave/Seq`, helpers `t303Inst*`, câblage complet), un bug de silence trouvé et
   corrigé (`t303InstLoad()` n'appelait jamais `audioT303Init()`, seulement les fonctions
   de réglage léger — un canal T303 jamais initialisé une première fois reste muet même
   avec des réglages fins ensuite ; vérifié par tap RMS/pic). Puis, l'utilisateur ayant
   constaté à l'usage qu'ils ne sonnaient pas différemment de 303S : « on va donc supprimer
   les 303A et 303B ». **Entièrement retirés** de `config.h`/`main.cpp` (confirmé par
   grep). 808/909 (synthèse AMY 8 voix prévue) n'ont, eux, jamais été commencés — toujours
   en attente. Détails : SOFTWARE.md (« Historique abandonné »).
AF. **BREAK : BPM=0 inatteignable + bouton RSD/SYNC sans effet apparent** — même classe de
   bug que les seuils exacts/plafonds documentés au §5 : le mapping potard→BPM n'avait pas
   de marge près de 0 (l'arrondi flottant de la courbe exponentielle potard→valeur ne
   landait jamais exactement sur `0`, le seuil qui active le mode "synchro à la taille du
   sample"), ce qui masquait en même temps l'effet du bouton RESYNC/SYNC (son état n'était
   relu qu'une fois `BPM==0` atteint). Les deux rapports de l'utilisateur étaient donc le
   même bug vu de deux côtés. Détails : SOFTWARE.md.
AG. **Menu : grille 5×3 → 3×5, texte agrandi** — sur demande explicite, en 2 itérations
   (`MENU_COLS` testé à 5 avant d'être corrigé à 3 : « je voulais une disposition en 3
   colonnes de 5 lignes »). Cases élargies (`CW=42,CH=23`), police montée à
   `u8g2_font_5x7_tf` (depuis `4x6_tf`) sur demande de texte plus lisible.
AH. **Refonte du popup potard P4-P7 — jauge demi-cercle, disposition en 4 coins** — suite
   à 3 rounds de retours successifs sur le popup du chantier AC ci-dessus (artefact de
   remplissage, puis « carré noir qui écrase le contenu », puis demande explicite d'une
   disposition en 4 coins + popup toujours affiché même sans paramètre mappé, puis enfin
   retour à un remplissage radial façon demi-cercle plus bas). **Découverte importante** :
   le remplisseur de triangles d'u8g2 (`u8g2_polygon.c`) ignore `setClipWindow()` et clippe
   contre les dimensions physiques de l'écran — contrairement à `drawDisc`/`drawCircle`/
   `drawBox`, qui eux respectent la fenêtre de clip. Tout remplissage à base de triangles
   doit donc être géométriquement autonome. Fix retenu : éventail de triangles dont chaque
   sommet est placé à EXACTEMENT le rayon extérieur (garantie de convexité = débordement
   géométriquement impossible). État final : demi-cercle `Rout=20/Rin=10`, 4 cellules de
   coin (une par potard, via `potGridPos()`, le helper déjà établi pour P4-P7), popup
   affiché pour tout mouvement de potard même sans case dédiée dans le mode courant
   (fallback dans le `default:` du dispatch de pots). Détails/code : SOFTWARE.md (« UI :
   rangée B1-B4 + popup potards », §3).
AI. **303S2 : mécanisme de split de clavier (entrées Y/AA/AB) jugé peu ergonomique,
   supprimé et remplacé par des touches d'altération maintenues** — sur demande explicite
   (« je trouve que le mécanisme de split de clavier... n'est pas ergonomique »).
   `s303s2SplitRows`/le toggle B4 court/long disparaissent ; la grille retrouve sa pleine
   tessiture de 31 notes partout. 3 nouvelles touches dédiées, **alignées verticalement en
   colonne 0** au-dessus de la touche REST (lignes 0-2 = OFFBEAT/ACCENT/SLIDE), maintenues
   PENDANT l'appui d'une note pour lui appliquer l'altération correspondante
   (`s303s2ModHeld`, bitmask `T303ALT_SLIDE/ACCENT/OFFBEAT` — renommé depuis `..._SYNC`,
   jugé incompréhensible par l'utilisateur). B4 redevient un simple CLEAR en appui court.
   **Séquenceur visuel retravaillé en 2 temps** : une 1ère passe qui gardait le nom de note
   en texte + petites marques d'altération a été jugée illisible par l'utilisateur ;
   remplacée entièrement par un point dont la position verticale encode la hauteur (plage
   MIDI mappée sur la hauteur de case) + symboles géométriques pour les altérations. **Bug
   de fond trouvé en creusant le rapport "illisible"** : la vraie cause n'était pas la
   taille du texte mais un mésappariement couleur-contenu/couleur-fond
   (`setDrawColor(cur?0:1)` alors que le fond est rempli dès `cur||!isRest`, une condition
   plus large) — rendait le contenu invisible, pas juste petit, sur la majorité des cases.
   Détails/code complet : SOFTWARE.md.
AJ. **303S2 : formes géométriques du séquenceur agrandies pour occuper toute la case** —
   suite directe de AI. L'utilisateur a d'abord précisé que les formes ne devaient PAS
   encoder la hauteur (contrairement à la position-dot de AI — « je ne cherche pas à
   indiquer la note via cette forme ») : une forme par TYPE (normal=disque, SLIDE=triangle,
   ACCENT=carré, OFFBEAT=losange, REST=anneau creux), priorité fixe SLIDE>ACCENT>OFFBEAT en
   cas de cumul. Puis demande explicite d'agrandir ces formes pour « prendre l'entièreté
   des cases ». **Piège trouvé en agrandissant** : la 1ère version dessinait un fond plein
   PUIS la forme par-dessus en couleur inverse (un "trou" découpé dans une boîte pleine) —
   correct à petite taille, mais à grande taille la forme recouvre presque toute la boîte
   et n'y laisse qu'un mince liseré (vérifié au niveau pixel, pas juste à l'œil — un motif
   boîte+forme-en-creux de cette taille peut tromper une lecture visuelle rapide d'un
   screenshot réduit). Fix structurel : le pas NON courant dessine la forme **pleine** sur
   fond resté noir (ajoute des pixels, n'en retire jamais) ; le pas EN COURS DE LECTURE
   dessine la forme en **contour seulement** (3-4 `drawLine`/`drawFrame`/`drawCircle`) sur
   son fond plein (un trait fin ne souffre pas du même problème d'érosion qu'un
   remplissage). Détails/code : SOFTWARE.md.
AK. **Pot popup : délai d'affichage réduit** (1400ms→700ms, `POT_POPUP_HOLD_MS`) et
   **BREAK : appui long RSD/SYNC réduit** (2000ms→1000ms, `BREAK_B4_LONGPRESS_MS`) — deux
   demandes explicites et ponctuelles, retours utilisateur directs sans investigation
   nécessaire.
AL. **MODE_DRUMS2 / MODE_DRUMS3 — kits batterie synthétisés 808/909** — sur demande
   explicite (« je trouve que les drums sont un peu cheap »). Même gabarit UI que DRUM2
   (8 pads, vues PAD/SEQ) mais synthèse AMY pure (SINE/TRIANGLE/NOISE + filtre + EG0/EG1)
   au lieu de PCM, horloge indépendante (pas raccordée au clock partagé DRUM2/SYSEQ/
   303S2/SS2/GEST). Première utilisation dans ce projet de `freq_coefs[COEF_EG1]` pour un
   pitch-drop de kick (vérifié par capture audio + estimation F0, pas juste déduit du
   code) ; a nécessité de relever `max_oscs` (250→266, AMY) faute d'oscillateurs libres
   sous le plafond par défaut — impact RAM/flash mesuré négligeable sur ESP32 réel. Détails
   complets (voix, synthèse, vérification) : SOFTWARE.md.
AM. **FX Punch (Sloop-style, momentané) — `OVERLAY_FX_PUNCH` — phase 1/2** — sur demande
   explicite (« j'aime bien les FX punch de Sloop... si je maintiens appuyé FX, j'accède à
   un menu qui me permet d'appliquer des effets tant que c'est maintenu »). Sloop
   (`github.com/isod89/sloop-fm1`, une custom firmware M-VAVE FM-1 elle-même bâtie sur
   Felucca — la lignée de MODE_FELU) expose 16 effets punch ; question posée et confirmée :
   **phase 1 = les 8 qui réutilisent un FX `fxList[]` déjà existant (snapshot/restore
   propre, y compris le cas des ressources partagées REP/BITCRS et RES/DELAY), phase 2 =
   les 8 "looper" qui ont besoin d'un nouveau moteur de capture du mix en direct (pas
   commencé)**. Piège découvert en cours de route : le bouton qui ouvre `OVERLAY_FX`
   n'est PAS le même partout (B1 pour SYNTH/OMNI/STONE/etc., B2 pour la majorité, B3 pour
   NOOB/NOOB2) — une 1ère hypothèse "B2 partout" était fausse et aurait cassé des boutons
   qui n'ont rien à voir avec FX dans plusieurs modes (ex. OMNI's B2=cycle de banque de
   batterie) ; remappé en parsant programmatiquement le switch de `handleButton()` plutôt
   qu'en relisant à l'œil. Le hold-detect pour B1 s'intègre dans le mécanisme de long-press
   à 600ms déjà existant (menu) plutôt que de poser un second minuteur concurrent sur le
   même bouton ; pour B2/B3, posé sans `return` sur l'appui pour laisser les logiques
   court-clic/double-clic déjà existantes (GROOVE notamment) tourner normalement — vérifié
   bout-en-bout que le double-clic de GROOVE reste intact. Détails complets (table des 8
   effets phase 1, mécanisme snapshot/restore, vérification) : SOFTWARE.md.
AN. **DRUMS2/DRUMS3 : vue PAD supprimée, mécanisme bank+altérations de DRUM2 réintégré**
   — sur demande explicite (« je veux que pour les drums, on ait accès uniquement au
   séquenceur, la touche de B4 servira alors au reset des notes placées. je veux que
   DRUMS2 et DRUMS3 possèdent le même mécanisme de bank et d'altérations que drums »).
   Un seul écran désormais (l'ancienne vue SEQ) ; B3 libre, B4 efface le pattern en un
   appui (plus de toggle vue). Le système de mod par pas (NRM/50%/RND/DBL, cyclé via la
   ligne 1 du clavier) et la bande de sélection des 8 pads (ligne 0) sont portés mot pour
   mot depuis la vue SEQ de DRUM2 (même rendu de case, mêmes couleurs LED). Détails :
   SOFTWARE.md.
AO. **DRUMS2/DRUMS3 : B4-clear-seul remplacé par B3=REC, B4=bank(court)/clear(long)** —
   suite directe de AN, sur demande explicite (« je veux que pour tous les drums, on ait
   B3: rec, B4: bank / clear en appui court / appui long » — "tous les drums" = DRUMS2+
   DRUMS3, pas DRUM2 qui garde ses 3 vues d'origine). B3 réarmé : la pression ligne 0
   (déjà un live-preview) se quantifie sur le pas le plus proche quand armé+en lecture,
   même calcul que l'ancien enregistrement PAD-view de DRUM2. B4 devient une vraie banque
   de 4 patterns (`drumSPatVel/Mod/Row[2][4][8][16]`, même concept que la "BANK" de
   DR2/GROOVE mais sans presse-papier copier/coller, non demandé) : appui court sauvegarde
   + avance + charge le slot suivant (rien n'est perdu en cyclant), appui long
   (`DRUMS_B4_LONGPRESS_MS`=600ms) efface uniquement le slot actif. Détails/vérification :
   SOFTWARE.md.

AP. **MODE_PRESETS : banque de 45 patchs curatés, posée sur le moteur de FELU** — sur
   demande explicite (« créer un mode avec pleins d'instruments dedans, les projets
   Sloop, X0X et Felucca possèdent une diversité d'instruments et d'algorithmes »), scopé
   par questions à une banque de presets parcourue par catégorie (pas un sélecteur
   algo+macros brut, déjà couvert par FELU), large éventail assumé dès cette passe.
   **Zéro nouveau DSP** : chaque preset est `{nom, catégorie, index d'algo FELU, 4 macros}`
   (`PresetDef`/`kPresets[]`, config.h) appliqué via `presetApply()`, qui règle
   `feluAlgoIdx`/`feluP[]` puis appelle `feluApplyP4P7()` — le pipeline de FELU lui-même.
   8 catégories façon Sloop (BASS/KEYS/ORGAN/PAD/LEAD/PLUCK/STAB/FX), DIGITAL/SAMPLE/GRAIN
   omis (déjà hors scope de FELU). `OVERLAY_PRESETS` (B3) mirror `OVERLAY_PKMN` (browser 2
   colonnes) à un détail près : chaque déplacement joystick applique immédiatement le
   preset survolé (preview live façon Sloop) au lieu d'un confirmer-puis-appliquer.
   Vérifié par test headless : les 8 catégories produisent un signal audio non nul,
   résolution catégorie+item→index plat du browser vérifiée. Détails : SOFTWARE.md.
   D'autres presets/moteurs sont anticipés en itérations suivantes, pas dans cette passe.

AQ. **MODE_PRESETS : joystick pilote octave + instrument (comme 303S), notes jouables en
   même temps** — sur demande explicite (« je veux pouvoir jouer des notes en même temps
   que je suis dans le mode bank de PRSET [...] je veux que comme pour 303S, [le joystick]
   impacte l'octave et l'instrument sélectionné »). JX cycle l'index plat de preset
   (`presetApply()`, debounce 150ms), JY décale l'octave (debounce 300ms), les deux
   tournant directement sur l'écran principal (pas besoin d'ouvrir `OVERLAY_PRESETS`) donc
   les notes restent jouables au clavier pendant qu'on tourne le joystick — mirror exact du
   bloc joystick de MODE_303S. Barre de vélocité JX-driven retirée (JX ne module plus la
   vélocité, l'axe est repris par le changement d'instrument) au profit d'un vélocité
   fixe=1.0. **Bug trouvé en testant** : au relâchement d'une touche, le code recalculait
   la note via `noteMap.getMidiNote()` avec l'octave *courante* plutôt que celle au moment
   de l'appui — si l'octave change (via JY) pendant qu'une note est tenue, le relâchement
   envoyait un `audioNoteOff` sur la MAUVAISE hauteur, laissant la voix originale bloquée
   indéfiniment. Corrigé en stockant la note jouée dans `activeNotes[row][col]` à l'appui
   et en relâchant exactement celle-là, peu importe les changements d'octave entre-temps.
   Vérifié par test headless (3 phases : note seule, changement d'instrument qui coupe
   légitimement la note en cours comme le fait déjà le P2 de FELU, changement d'octave qui
   NE coupe PAS une note tenue) + confirmation que la note se relâche proprement après le
   fix (RMS retombe à 0, plus de voix fantôme).

AR. **DRUM2 unifié avec DRUMS2/DRUMS3 : vues PAD/ANIM supprimées, B4 devient bank "PAT"
   partagé** — sur demande explicite (« pour drums, je veux supprimé les view via B4, que
   ce soit un bouton de bank/clear [...] renomme [bank] par pat pour pattern [...] pour
   DRUM2, DRUMS2 et DRUMS3 »), ceci **inverse une décision explicite antérieure** (entrée AO
   ci-dessus : « tous les drums = DRUMS2+DRUMS3, pas DRUM2 qui garde ses 3 vues
   d'origine »). DRUM2 perd : la vue PAD (couche expressive ghost/soft/normal/accent à 4
   niveaux de vélocité, `kDr2RowVar[]`) et la vue ANIM (visualiseur GIF synchronisé aux
   coups de batterie, `draniBrowse/draniRunning`/overlays par tambour) — son enregistrement
   quantifié "en jouant" (ex-PAD-view) a été **porté** dans la ligne 0 de la vue SEQ
   (désormais permanente), exactement comme DRUMS2/3 l'avaient déjà fait pour leur propre
   ligne 0. Le code de la vue ANIM (`draniLoadFolder`/`convertGifAnimToBvid`/buffers)
   **n'a pas été supprimé** — devenu injoignable (plus aucun point d'entrée n'ouvre
   `drum2View==2`, variable elle-même supprimée), mais laissé en l'état plutôt que de
   l'arracher sans demande explicite. Nouvelle banque 4 slots propre à DRUM2
   (`drum2PatVel/Row/Mod/PitchOff[4][...]`, `drum2ActivePat`, `drum2PatCycle()`/
   `drum2PatClear()`) — même mécanisme que DRUMS2/3 (appui court sauvegarde+avance+charge,
   appui long `DRUM2_B4_LONGPRESS_MS`=600ms efface le slot actif). **Renommage demandé** :
   `drumSBankCycle()`/`drumSBankClear()` → `drumSPatCycle()`/`drumSPatClear()` (les tableaux
   `drumSPatVel/Mod/Row[]` portaient déjà ce nom), et le libellé bouton DRUMS2/3 `"BNK/CLR"`
   → `"PAT/CLR"` ; DRUM2 rejoint directement `ctrlLabelsFor()` des deux autres (un seul
   `case` partagé désormais). Vérifié par test headless : cycle de banque sur 4 slots
   (sauvegarde+chargement confirmés), appui long efface le slot actif seul, build ESP32+sim
   propres. Détails : SOFTWARE.md.

AS. **FX Punch : légende LED à l'activation + fixes colonnes 3-4 (phase 1)** — sur
   rapport utilisateur (« les 2 premières colonnes ne fonctionnent pas [...] les 2 autres
   fonctionnent, mais les effets n'impactent pas trop le son [...] gate, echo et wobble
   fonctionnent bien, mais les autres ne fonctionnent pas [...] je veux qu'à l'activation
   de ce mode, l'affichage LED change »). Trois correctifs distincts trouvés par lecture de
   code (pas de bug runtime caché — les causes étaient visibles statiquement) :
   1. **ALIAS (REP, idx8) quasi inaudible** : seuil de wavefold 0.85 → gain≈1.18x, trop
      proche de 1 (= quasi no-op). Remplacé par 0.08 → gain≈12.5x, dans l'esprit du gain
      délibérément extrême de BITCRUSH.
   2. **LP_SWEEP/HP_SWEEP/PHONE (FILT, idx0) silencieusement écrasés** : `applyFxEffect()`
      mémorise la Res *par Typ* dans des statics locales à la fonction (`lastFiltTyp`/
      `filtResByTyp[]`) pour ne pas surprendre l'utilisateur en changeant de Typ à la main
      — mais ce mécanisme écrasait aussi la Res que `fxPunchApply()` venait juste de poser,
      dès que le Typ différait de celui actif avant le punch. Même souci pour `lpfSmoothCut`
      (lissage anti-zipper du cutoff, 50%/10ms) : non resynchronisé au punch, le cutoff
      réel mettait ~100-200ms à converger au lieu de s'appliquer tout de suite. Fix :
      statics promues en portée fichier (`s_lastFiltTyp`/`s_filtResByTyp[]`), pré-synchro
      explicite dans `fxPunchApply()` *et* `fxPunchRevert()` avant d'appeler
      `applyFxEffect()`. Vérifié par test headless avec préconditions (FILT pré-réglé
      manuellement sur un Typ différent avant le punch — exactement le cas qui déclenchait
      le bug) : Res/cutoff restent bien ceux voulus par le punch, et le revert restaure
      exactement l'état pré-punch (pas la valeur par-Typ générique).
   3. **Légende LED `OVERLAY_FX_PUNCH`** : les 16 touches (colonnes 4-7 physiques) affichent
      désormais une couleur par touche (blanc = tenue, teinte distincte sinon) au lieu du
      rendu LED normal du mode sous-jacent ; colonnes 0-3 éteintes (non utilisées par cet
      overlay). Ajouté tôt dans `updateLedsAndShow()`, avant le `switch(currentMode)`
      normal (désactivé tant que l'overlay est ouvert, sinon il écraserait la légende juste
      après). Détails : SOFTWARE.md.

AT. **FX Punch phase 2 — moteur de capture en ring-buffer pour les 8 effets "looper"
   (LOOP 1/4-1/32, STUTTER, REVERSE, TAPESTOP, HALFSPD)** — jusqu'ici des cases visibles
   mais inertes (`PUNCH_NONE`) dans la grille, documentées depuis l'entrée AM comme
   nécessitant un nouveau moteur de capture (« pas une simple extension »). Implémenté
   entièrement sur demande explicite, confirmée via question (« implémenter maintenant,
   tout »). **Architecture** (`amy.c`/`amy.h`, nouveau bloc juste avant `amy_fill_buffer()`,
   partagé ESP32+simulateur puisque c'est le seul point de rendu commun aux deux cibles) :
   - `amy_punch_cap_buf` : anneau de 2s (`AMY_PUNCH_CAP_SECONDS`) continuellement rempli
     avec le mix final post-FX (= ce qui part littéralement vers le DAC), capturé à chaque
     bloc dans `amy_punch_capture_block()` — jamais interrompu, même pendant qu'un punch
     écrase la sortie, pour que l'historique reste frais et que le relâchement reprenne le
     direct sans accroc.
   - `amy_punch_loop_buf` : copie figée séparée (même taille), utilisée UNIQUEMENT par
     LOOP/STUTTER — une boucle soutenue plus longtemps que la capacité de l'anneau
     principal serait sinon corrompue par l'écriture continue qui finit par repasser sur
     les mêmes échantillons en train d'être rejoués.
   - Mode `FIXED` (LOOP 1/4-1/8-1/16-1/32, STUTTER) : fenêtre figée à l'appui (durée =
     note musicale selon le BPM courant pour les LOOP — même convention que GATE1/16 —, ou
     ~60ms fixes non synchronisés au tempo pour STUTTER), relecture en boucle exacte
     (vitesse 1.0, pas d'interpolation nécessaire).
   - Mode `RATE` (REVERSE=-1.0, HALFSPD=0.5, TAPESTOP=1.0 décroissant exponentiellement
     vers 0 sur ~1.2s comme un moteur de bande qui s'arrête) : lecture à position
     fractionnaire avec interpolation linéaire directement dans l'anneau vivant, avec un
     clamp de sécurité qui empêche le curseur de lecture de tomber à plus de
     `capacité - 1 bloc` derrière (ou se faire rattraper par) le curseur d'écriture — sinon
     une tenue très longue à vitesse lente/négative finirait par lire des échantillons en
     train d'être recyclés.
   - **Allocation** : les deux buffers (~700KB total) sont alloués en PSRAM
     (`MALLOC_CAP_SPIRAM`, via `malloc_caps()` déjà existant dans AMY) **explicitement**,
     pas via `amy_global.config.ram_caps_delay` — ce champ existe mais vaut
     `MALLOC_CAP_DEFAULT` (RAM interne) sur cette carte, pas `MALLOC_CAP_SPIRAM` comme sur
     TULIP/AMYBOARD ; s'y fier aurait tenté d'allouer ~700KB dans les ~124KB de DIRAM libre
     et aurait échoué/planté sur matériel réel.
   - `fxPunchApply()`/`fxPunchRevert()` (main.cpp) : nouveau `punchKindIsLooper()` route les
     8 nouveaux `PunchKind` directement vers `amy_punch_*()`, en contournant entièrement
     `fxList[]`/le mécanisme de snapshot (rien à sauvegarder, aucun FX impliqué). La case
     "pas encore" du rendu OLED/LED a été retirée (toutes les 16 touches sont maintenant
     actives).
   - **Vérifié par test headless** (les 8 effets) : tenir une note, laisser ~800ms
     d'historique se capturer, appliquer le punch PUIS couper la note en direct (le direct
     devient silencieux) — si le punch marche vraiment, le son doit continuer malgré la
     coupure en direct (preuve qu'il rejoue bien l'historique figé, pas juste le direct qui
     passerait à travers). Confirmé pour les 8 : RMS reste non-nul pendant toute la tenue
     malgré la note coupée en direct, TAPESTOP montre une décélération nette vers le quasi-
     silence en fin de tenue (signature de son ralentissement exponentiel), et chaque
     relâchement retombe proprement à ~0 (direct repris, qui est silencieux puisque la note
     avait été coupée). RAM statique ESP32 inchangée (38.8%) — confirmé que les buffers
     vont bien en PSRAM dynamique, pas en tableaux statiques DIRAM. Détails : SOFTWARE.md.

AU. **FX Punch : vraie cause racine de LP/HP SWEEP/PHONE/BITCRUSH (fxOrderList, pas juste
   le clobber Res/cutoff de l'entrée AS) + activation plus rapide avec bascule hold/release
   vers le menu normal** — rapport utilisateur suivant l'entrée AS (« les FX punch
   fonctionnent bien, sauf LP et HP sweep, et phone et bitcrush [...] je veux qu'il s'active
   plus rapidement, que ça switch vers FX punch et vers FX normal si je relâche l'appui, que
   ça reste sur FX punch si je maintiens appuyé »). **Vraie cause** (le fix Res/cutoff de AS
   était réel mais ne suffisait pas) : `amy_apply_ordered_fx()` (amy.c) ne déclenche
   `amy_apply_filt()`/`amy_apply_wavefold_bus0()` que pour les slots présents dans
   `fxOrderList[]` — un tableau séparé de `fxList[idx].active`, rempli uniquement par
   `fxOrderAdd()`, appelé uniquement depuis `overlayFxToggle()` (le menu FX normal).
   `fxPunchApply()` posait `.active=true` + tous les bons params mais n'ajoutait jamais à
   `fxOrderList` → tant que l'utilisateur n'avait pas indépendamment allumé FILT(0) ou
   BITCRS(9) au moins une fois via le menu normal, leur DSP ne tournait jamais sur le
   buffer, silencieusement. ALIAS(8) partage le même wavefold que BITCRS(9) — s'il
   "marchait" c'était par coïncidence de session (REP déjà allumé par ailleurs), pas par
   différence de code. GATE/ECHO/WOBBLE ne passent jamais par `fxOrderList` (tick 10ms
   dédié / position fixe AMY-native), d'où leur immunité totale. **Fix** :
   `fxPunchApply()`/`fxPunchRevert()` font maintenant le `fxOrderAdd()`/`fxOrderRemove()`
   symétrique (seulement si le slot n'était pas déjà actif indépendamment). **Activation
   plus rapide** : `FX_PUNCH_HOLD_MS` 350ms→120ms. **Bascule hold/release** (⚠️ corrigée en
   cours de route, voir juste après) : `fxPunchExit()` (appelé au relâchement du bouton
   déclencheur après un hold réussi) revient à l'écran principal du mode (`OVERLAY_NONE`) ;
   le clic court existant (bascule immédiate du menu FX normal dès l'appui, inchangée)
   reste la seule porte d'entrée vers le menu FX normal. Vérifié par test headless :
   assertions directes sur `fxOrderList[]`/`fxOrderCount` (pas une inférence RMS) pour
   LP_SWEEP/BITCRUSH + le cas "FILT pré-allumé manuellement" (pas de doublon, pas
   d'extinction au relâchement) ; séquence appui→hold→relâchement avec les valeurs exactes
   de `s_overlay` à chaque étape. Détails : SOFTWARE.md.

   **⚠️ Corrigé dans la même session, avant tout commit** : la 1ère itération avait fait
   basculer `fxPunchExit()` vers `OVERLAY_FX` (menu normal) au lieu de `OVERLAY_NONE`, sur
   une mauvaise lecture de « switch vers FX punch et vers FX normal si je relâche l'appui »
   — l'utilisateur a corrigé immédiatement : « je veux qu'après un appui long sur FX, on
   revienne au principal, le seul moyen d'accéder au menu bloquant des FX est un appui
   court sur FX ». Reverti à `OVERLAY_NONE`. Leçon : quand une formulation utilisateur sur
   un comportement hold/release est ambiguë entre "revient au menu normal" et "revient à
   l'écran principal", vérifier plutôt que de choisir par défaut — ici les deux lectures
   étaient plausibles et seule la bonne a fini par être demandée explicitement.

AV. **MODE_303S : P2 (texture par forme d'onde) s'initialise à 0% à l'entrée du mode, peu
   importe la position physique du potard** — demande explicite (« initialise le P2 à 0%
   dans la 303S »). `audioT303Init()` ne touche pas le wavefold/duty — sans ce fix, le son
   gardait la texture laissée par la session précédente jusqu'à ce que l'utilisateur touche
   le potard. Le hook d'entrée de `switchMode()` applique maintenant explicitement 0% à
   travers les mêmes branches par forme d'onde que le dispatch pot de `loop()` (sub-oct
   wave→`audioSW2SetBlend(0)`, PINK→no-op, PULSE→`audioT303Duty(0.5)`, TRI2/SAW2→
   `audioT303WavefoldAsym(0)`, autres→`audioT303Wavefold(0)`), puis fait un **pickup** sur
   `g_lp303swf` (= `pots[1].value` courant) pour que le dispatch pot du tick suivant ne
   détecte pas un "changement" et ne resnap pas instantanément vers la position réelle du
   potard — même idiome pickup que P4-P7 (`lp303[]`) juste au-dessus dans le même bloc,
   mais figeant le son à 0% plutôt qu'en adoptant la valeur du potard. Vérifié par test
   headless : potard à 80% à l'entrée → `amy_wavefold_gain=1.0` (0% de fold, pas les ~20x
   qu'aurait donné 80%) ; reste à 1.0 après plusieurs ticks avec le potard **non touché**
   (pas de resnap) ; tourner le potard à 90% après coup met bien `amy_wavefold_gain≈22.6`
   (32^0.9), confirmant la reprise normale du suivi une fois le potard effectivement
   touché. Scopé à MODE_303S uniquement (pas 303S2/I303), demande explicite.

AW. **FX Punch : plusieurs touches tenues en même temps + appui = punch instantané,
   relâchement rapide = menu normal** — deux demandes dans le même message : « je veux
   pouvoir appuyer sur plusieurs FX punch en même temps [...] je veux que le menu de FX
   normal n'apparaisse que si je relâche rapidement le bouton FX, de sorte à que dès que
   j'appuie sur FX, ce soit le menu FX punch ».
   - **Multi-punch** : `s_fxPunchActiveOpt` (un seul opt) → `s_fxPunchHeld[16]` (bool par
     touche). Les 8 effets phase-1 passent d'un snapshot par-opt à un snapshot
     **par-slot avec comptage de référence** (`FxPunchSlotState{refCount,snapActive,
     snapParams[4]}` × `FX_COUNT`) — plusieurs touches peuvent viser le même slot
     fxList[] (ALIAS/BITCRUSH partagent déjà le gain de wavefold, deux sweeps viseraient
     tous les deux FILT) : snapshot+`fxOrderAdd()` à la 1ère touche sur ce slot
     (`refCount` 0→1), restauration+`fxOrderRemove()` seulement à la dernière relâchée
     (`refCount` →0) ; entre les deux, "dernier écrit gagne" sur les params partagés,
     comportement déjà accepté pour ALIAS/BITCRUSH. Les 8 effets phase-2 looper restent un
     moteur singleton (`amy_punch_*()`) — une 2e touche looper pendant qu'une 1ère est
     tenue est ignorée (pas de sens musical à tenir REVERSE+HALFSPD en même temps).
   - **Appui = punch instantané** : le mécanisme à délai (hold ≥120ms détecté par un poll
     dans `loop()`) est remplacé par un `fxPunchEnter()` synchrone à l'appui, pour
     n'importe quel bouton déclencheur FX de n'importe quel mode. Le relâchement classait
     la durée tenue : court (<120ms) → `OVERLAY_FX` ; tenu → revert + `OVERLAY_NONE`.
     **⚠️ Remplacé par l'entrée AZ** (ce classement empêchait de sortir du menu FX par un
     appui court). Ce nouveau bloc, générique et placé avant le
     `switch(currentMode)` par-mode, `return`-ne immédiatement pour le bouton concerné —
     remplace le `if(btn==N){...s_overlay=OVERLAY_FX;...}` dupliqué dans chaque dispatch
     par mode. **`MODE_GROOVE` B2 exempté** : son simple-clic=FX/double-clic=Scale-Arp se
     décide sur l'appui lui-même (fenêtre 350ms) — un punch instantané et inconditionnel
     aurait cassé ce double-clic sans discussion ; garde son ancien mécanisme à délai
     (poll dans `loop()`, désormais restreint à ce seul mode/bouton).
   - Les deux polls génériques dans `loop()` (B1-group et B2/B3-group) qui appelaient
     `fxPunchEnter()` après le délai sont supprimés (entrée désormais synchrone sur
     l'appui) ; un seul poll GROOVE-only subsiste.
   - Vérifié par test headless : appui→(0 tick)→`OVERLAY_FX_PUNCH` instantané, appui
     court→`OVERLAY_FX`, appui tenu→`OVERLAY_NONE` ; `MODE_GROOVE` confirmé **non**
     affecté (simple clic→FX, double-clic→Scale/Arp, comme avant) ; GATE+WOBBLE (slots
     différents) actifs simultanément sans interférence ; LP_SWEEP+HP_SWEEP (même slot
     FILT) — `refCount` 0→1→2 puis 2→1 (slot toujours actif, l'autre touche le tient
     encore) puis 1→0 (restauration complète). Détails : SOFTWARE.md.

AX. **Trois correctifs audio indépendants (rapport utilisateur en une seule session)** :
   1. **MODE_BREAK — craquements aux changements de portion de sample** : `render_pcm()`
      (`lib/AMY Synthesizer/src/pcm.c`) est le SEUL type d'oscillateur d'AMY qui applique
      un `amp` constant pour tout le bloc (128 échantillons) au lieu d'interpoler
      échantillon par échantillon comme `render_lut()` (sine/saw/triangle/pulse/
      wavetable). Chaque retrigger de BREAK/GRANULAR2 (nouvelle portion sur le MÊME
      oscillateur) remet `phase=0` (`pcm_note_on()`) sans aucun fondu contre la valeur
      précédemment en cours — un saut de valeur brut — et l'attaque d'enveloppe de BREAK
      (1-2ms) est plus courte qu'un seul bloc de rendu (~2.9ms à 44.1kHz), donc même
      l'enveloppe ne peut pas masquer le saut. Fix : `render_pcm()` détecte qu'un bloc
      démarre exactement à `phase==0` (un (re)trigger) et applique un fondu d'amplitude
      dédié sur les 32 premiers échantillons (~0.7ms, indépendant de l'enveloppe
      musicale) — trop court pour s'entendre comme un ramollissement de l'attaque, assez
      long pour casser le saut. S'applique à TOUT PCM (SAMPLE/GRANULAR2/STONE/BREAK), pas
      seulement BREAK. Vérifié par test headless : `blockAmp` confirmé en rampe linéaire
      0.03→0.99 sur les 32 échantillons, aussi bien au premier déclenchement qu'au
      retrigger immédiat du même oscillateur.
   2. **Résidu basse fréquence inaudible mais visible au spectrogramme, avec DELAY+
      REVERB+CHORUS actifs** : aucun étage de DC-blocking n'existe dans le chemin de
      sortie final — un filtre passe-haut un-pole à ~27Hz (`amy_fill_buffer()`,
      `AMY_HPF_OUTPUT`) existe dans le code source d'AMY mais n'était **jamais défini**
      (confirmé par grep sur tout le repo). DELAY et REVERB ont tous les deux un chemin
      de feedback façonné passe-bas par construction (le `filter_coef`/`damping` de
      chacun atténue les aigus à chaque répétition/recirculation) — donc ce qui reste le
      plus longtemps dans une traîne qui s'éteint est, par construction, disproportionné-
      ment basse fréquence, sans qu'aucun filtre en aval ne le retire jamais. Fix :
      `#define AMY_HPF_OUTPUT` activé dans `amy.h` (inconditionnel, pour toutes les
      cibles) — coupure ~27Hz, largement sous toute basse audible, donc sans impact sur
      le son normal. CHORUS confirmé sans feedback à son site d'appel (`feedback_level=0`
      en dur) — jamais une source indépendante de traîne. Piste secondaire non confirmée
      (juste plausible) : la multiplication fixed-point `SMULR6()` utilisée par les DEUX
      boucles de feedback a un arrondi asymétrique autour de 0 qui pourrait créer un
      "limit cycle" (le résidu ne retombe jamais à zéro machine exact) — pas vérifié par
      mesure runtime, le fix HPF suffit déjà à retirer ce qui serait inaudible de toute
      façon.
   3. **MODE_303S2 (pas MODE_303S !) : le fix "P2 init à 0%" de l'entrée AV ne
      s'appliquait jamais** — `MENU_303S` n'est câblé dans AUCUN onglet de menu
      (`kMenuCatInstr/Seq/Aut`), seul `MENU_303S2` l'est (renommé "303S" à l'écran, voir
      note historique dans SOFTWARE.md) ; `MODE_303S` est donc du code mort, jamais
      atteignable depuis le menu. Le fix précédent avait été posé sur le bon MÉCANISME
      mais le MAUVAIS mode. Même fix, porté sur le hook d'entrée `switchMode()` de
      `MODE_303S2` cette fois (branche non-drill-down uniquement — la branche
      `gestDrillDown` préserve délibérément la texture existante, commentaire d'origine :
      "Pot-1 in GEST is not the 303 wavefold knob"). Vérifié par test headless : potard à
      80% à l'entrée → `amy_wavefold_gain=1.0` (au lieu de ~20x), reste à 1.0 après
      plusieurs ticks sans toucher le potard.

AY. **3e cible : simulateur web (WebAssembly/Emscripten), `web/` + `docs/`** — sur demande
   explicite (« les projets felucca/X0X/sloop réussissent à avoir un site web qui permet
   de faire tourner leur simulateur [...] j'aimerais que tu étudies leur système afin
   d'avoir nous aussi un site web qui tourne, via HTML et qu'on peut héberger sur
   github »). Recherche sur `hugelton/Felucca-WebApp` : c'est en fait un éditeur/
   installeur Web MIDI (SysEx) pour du vrai matériel, PAS un simulateur audio dans le
   navigateur — moins avancé que ce que `structure/WEB_SIMULATOR.md` décrivait déjà pour
   GrvEP. Découverte en creusant : cette doc décrit une architecture détaillée mais **qui
   n'existait pas du tout dans l'arbre** (`web/` absent, `docs/` ne contenait que le
   guide utilisateur) — un commit très ancien avait ajouté un `docs/index.html` (792
   lignes) depuis supprimé. Reconstruit entièrement depuis la spec (déléguée à un agent en
   tâche de fond, worktree isolé, puis vérifiée et corrigée dans cette session avant
   fusion dans l'arbre principal). Détails de l'implémentation (web_tasks.cpp/Fibers,
   hal/U8g2lib.h, SD.h, etc.) : voir `structure/WEB_SIMULATOR.md`, substantiellement
   réécrit.

   **Révision architecturale majeure découverte en compilant réellement** (pas une
   supposition — confirmée en lisant le code et en testant les flags) : le plan d'origine
   (single-file, single-threaded, sans pthread/SharedArrayBuffer) était **irréalisable**.
   La copie vendorée de `miniaudio.h` n'a aucun chemin audio non-worklet en
   `__EMSCRIPTEN__` — chaque branche passe par `emscripten_create_wasm_audio_worklet_node()`,
   qui exige de la mémoire partagée entre le thread principal et le thread audio, donc
   `-pthread`, donc `SharedArrayBuffer`, donc des headers COOP/COEP — que GitHub Pages ne
   permet de personnaliser d'aucune façon. Contourné avec `docs/coi-serviceworker.js`
   (vendoré tel quel, `gzuidhof/coi-serviceworker`, MIT) qui injecte ces headers côté
   client via un service worker, sans configuration serveur — un seul rechargement
   silencieux de la page au premier chargement. `-sSINGLE_FILE=1` a dû être abandonné
   (incompatible avec `-sAUDIO_WORKLET`, erreur de build Emscripten explicite) : deux
   fichiers (`grvep.js`+`grvep.wasm`) au lieu d'un seul, sans impact (fichiers statiques
   de même origine, pas de CORS). AMY a elle-même un build web amont qui utilise déjà
   exactement ces flags (`lib/AMY Synthesizer/Makefile`'s `web` target) — précédent qui a
   confirmé la bonne piste une fois la cause identifiée.

   **Vérifié** : build propre (compile+link) dans le worktree ET après fusion dans l'arbre
   principal ; test headless sous Node confirmant que `setup()` tourne jusqu'à `"Ready"`,
   que le clavier/OLED/LEDs/mux fonctionnent tous correctement (mêmes mécanismes que le
   simulateur SDL2, juste alimentés par JS au lieu de SDL). **Non vérifié** : le rendu
   audio réel dans un vrai navigateur — aucun Chrome/Chromium/Puppeteer disponible dans cet
   environnement (tentative d'installation de Puppeteer abandonnée, trop lente/non
   fiable dans ce sandbox) ; Node n'a de toute façon jamais `window.AudioContext` (ce
   n'est pas un navigateur), donc l'échec observé à ce point précis sous Node est attendu
   et ne prouve ni l'échec ni la réussite du chemin AudioContext réel. **Reste à faire
   avant de considérer cette cible terminée** : ouvrir `docs/index.html` dans un vrai
   navigateur, cliquer une touche, vérifier dans la console qu'aucune erreur liée à
   l'AudioContext/cross-origin-isolation n'apparaît et que du son sort effectivement.
   Flashing firmware via navigateur (WebSerial/ESP Web Tools) explicitement hors scope de
   cette passe (demande de l'utilisateur, priorité ultérieure).

AZ. **FX Punch : impossible de sortir du menu FX par un appui court (corrige AW)** —
   rapport sur carte : « pour rentrer dans les FX, un appui court suffit, mais pour en
   sortir, il faut que je fasse un appui long ». Cause : le relâchement court de AW posait
   `OVERLAY_FX` sans regarder l'état à l'appui ; depuis le menu FX, un appui court faisait
   FX → punch → FX, sans sortie possible (sauf en tenant plus de 120 ms, seuil qu'un appui
   « court » humain chevauche souvent). **Nouvelle règle**, indépendante de la durée :
   relâchement **sans** punch joué = bascule (ouvre `OVERLAY_FX` s'il était fermé, ferme
   s'il était ouvert, `OVERLAY_FX_MOD` compté comme ouvert) ; relâchement **avec** punch
   joué = `OVERLAY_NONE` comme avant. Deux booléens : `s_fxBtnFromFx` (posé à l'appui),
   `s_fxBtnPunched` (posé par `fxPunchApply()`). `FX_PUNCH_HOLD_MS` ne sert plus qu'au
   poll de `MODE_GROOVE` B2, non concerné (bascule déjà à l'appui). Vérifié : test headless
   21/21 (3 groupes de boutons × 7 séquences), puis **sur carte** (log série + confirmation
   du propriétaire dans plusieurs modes). Détails : SOFTWARE.md, « Révision — sortie du
   menu FX par appui court ». Note : le 1er flash corrigé a d'abord été signalé « toujours
   bloqué », puis le 2e (identique sur cette logique, + logs) a marché partout — cause de
   ce premier échec non élucidée (aucune trace série de ce test) ; si ça réapparaît,
   capturer le port série **avec pyserial** (`pio device monitor` refuse de tourner sans
   terminal interactif) et regarder la séquence `BTN raw=… DN/UP`.

## 5. Pièges et classes de bugs récurrentes (lire avant de toucher au DSP ou aux modes)

**Audio / AMY**
- `SAMPLE` = **point fixe s8.23 sur toutes les cibles, simulateur inclus** (`AMY_USE_FIXEDPOINT`
  en dur dans `amy.h`). Comparer une valeur de bus à une constante normalisée sans `S2F()` est faux
  (c'était le bug COMPRESS). Lire/écrire avec `S2F()/F2S()`.
- Buffers de bus `fbl[core][bus]` = **planaires** (`buf + ch*AMY_BLOCK_SIZE`). Le commentaire
  « interleaved » de `mix_with_pan()` est faux.
- Un event AMY « reconfigure complet » (num_voices, patch, …) **tue une voix en cours** ; pour
  changer un seul champ en direct, envoyer un event léger (déjà appliqué pour la table
  d'onde du modulaire). Breakpoints d'enveloppe : toujours envoyer l'index 0.
- Les types de filtre par voix d'AMY `FILTER_LPF/HPF/BPF` ont un défaut de calibrage à cutoff
  extrême (chemin « normbits », `filters.c`). Utiliser `FILTER_LPF24` ou un filtre de bus.
- **Toute boucle de feedback doit avoir un gain < 1 ou un limiteur doux, plus une garde
  NaN/inf** : deux bugs de « son cassé/volume infini » cette session venaient d'une
  résonance instable (SVF Chamberlin) et d'un gain de boucle > 1 plafonné à ±4.
- **`fxList[idx].active = true` ne suffit PAS à faire tourner le DSP de FILT(0)/REP(8)/
  BITCRS(9)/DRIVE(1)/RESECHO(7)/RINGMOD(11)/COMPRESS(12)** — `amy_apply_ordered_fx()`
  (amy.c) ne les appelle que pour les slots présents dans `fxOrderList[]`, un tableau
  séparé rempli uniquement par `fxOrderAdd()` (appelé uniquement depuis
  `overlayFxToggle()`, le menu FX normal). Tout nouveau code qui active un de ces slots
  par un autre chemin (vu avec FX Punch, entrée AU) doit appeler `fxOrderAdd()`/
  `fxOrderRemove()` lui-même, sous peine de poser tous les bons paramètres pour rien — le
  bug est silencieux (pas de crash, pas d'erreur), juste aucun changement audible. Les
  autres FX (TREMOLO/DELAY-RES/CHORUS/FLANGER/REVERB/EQ) sont soit sur un tick dédié soit à
  position fixe AMY-native et n'ont pas ce piège.
- Un FX appliqué **par oscillateur/canal** n'atteint que les plages câblées à la main
  (STONE, MODULAR, DJ, CRUNCH, GRANULAR2 avaient chacune leur helper) → préférer un FX de bus.
- Bus 1 (TB-303) → mergé dans le bus 0 avant les FX ; ne jamais appeler `config_reverb/echo`
  sur le bus 1 (saturation PSRAM).
- Indices de FX (`fxList[]`) **codés en dur** à plusieurs endroits : liste dans SOFTWARE.md
  « Effets ». Ajouter/retirer un FX = les mettre tous à jour, dont le `switch` d'`amy.c`.
- **Enveloppe de pitch d'oscillateur** (pas juste de filtre) : `e.freq_coefs[COEF_EG1]`
  (ou `[COEF_EG0]`) est un coefficient linéaire en **octaves**, sommé directement dans le
  log2-fréquence de l'oscillateur avec la valeur courante de l'enveloppe EG1/EG0 (PAS le
  compactage log supplémentaire que subit l'amplitude — ça, c'est `amp_combine_controls()`
  uniquement). Même mécanisme que `filter_freq_coefs[COEF_EG0/EG1]` (déjà utilisé partout
  pour moduler un filtre, p.ex. FELU/MOD2) mais appliqué au PITCH de l'oscillateur —
  première utilisation dans ce projet (voir MODE_DRUMS2's kick pitch-drop, SOFTWARE.md).
  `NOISE` ignore totalement la fréquence (`render_noise()`, oscillators.c) — inutile d'y
  mettre un `freq_coefs`, ça ne fera jamais rien.
- `max_oscs` (AMY, `amy_config_t`) est fixé par défaut à 250 et ce projet n'en utilise QUE
  le défaut — tous les oscillateurs jusqu'à 249 étaient déjà alloués avant la session qui
  a ajouté DRUMS2/DRUMS3 (relevé à 266 à cette occasion, voir `audioInit()`). Vérifier le
  budget avant d'ajouter une nouvelle plage d'oscillateurs fixe.

**UI / modes**
- `handleButton()` : `btn = 3 - rawBtn` ; **B1 (btn 0)** est traité au *relâchement* dans un
  premier `switch(currentMode)` (transport / FX pour les modes « template synthé »), **B2-B4
  (btn 1-3)** dans un second `switch` à l'appui ; `default: break;` → un mode absent d'un
  switch n'a simplement aucune action sur ce bouton. `btn>3` ne se produit jamais.
- **Colonne 0 = touche la plus à droite** (col7 la plus à gauche) ; **ligne 3 = ligne du haut**.
  Deux inversions (EUCLI lignes, CRUNCH colonnes) sont venues de là.
- **P1 et P3 (`pots[0]`, `pots[2]`) sont Volume/BPM pour tous les modes**, appliqués avant le
  switch par mode : ne jamais les relire pour un usage propre. Chaque mode a besoin d'une case
  dans `ctrlLabelsFor()`.
- `drawScreen()` teste `s_overlay` **avant** `menuOpen` : tout code qui ouvre le menu doit remettre
  `s_overlay = OVERLAY_NONE`.
- Menu : ajouter un mode = l'inscrire dans `kMenuCatInstr/Seq/Aut[]` **et** ajuster
  `kMenuCatSizes[]`. Autres tableaux positionnels par `AppMode` : `kIsSeq[MODE_COUNT]`,
  `kVizMode[MODE_COUNT]` (un `bool[]` trop court se complète de zéros en silence ; un
  `const char*[]` trop court plante).
- Édition des paramètres FX au potard : **toujours relative** via `fxPotDeltaApply()` (rawDelta) — ne
  jamais remapper `pots[i].value` (intégrale démarrant à 0) directement sur un paramètre. Un nouveau
  mode qui édite les FX doit boucler `for p<4` sur ce helper puis appeler `applyFxEffect(fxSelected)`.
- Beaucoup d'anciens petits affichages FX par mode appellent encore `fxFiltTypName()` sans
  `fxDriveTypName()` (cosmétique : DRIVE y apparaît comme « DRIVE » + nombre brut).
- **`u8g2_DrawTriangle()`/le remplisseur de polygones d'u8g2 ignore `setClipWindow()`** —
  clippe contre les dimensions PHYSIQUES de l'écran (`u8g2_GetDisplayWidth/Height`),
  jamais contre la fenêtre posée par `setClipWindow()`, contrairement à `drawDisc`/
  `drawCircle`/`drawBox` qui eux la respectent. Tout remplissage à base de triangles doit
  être géométriquement autonome (ex. tous les sommets à une distance fixe d'un point —
  garantie de convexité) plutôt que de compter sur un clip qui n'opère pas sur ce type de
  primitive. Découvert sur la jauge circulaire du popup potards (SOFTWARE.md).
- **Couleur de contenu choisie sur une condition plus étroite que celle qui remplit
  réellement le fond → contenu invisible, pas juste petit.** Ex. : `setDrawColor(cur?0:1)`
  pour un point/texte alors que le fond de la case est rempli dès `cur||!isRest` (une
  condition plus large) — toute case qui satisfait la condition large mais pas l'étroite
  dessine un contenu de la même couleur que son propre fond. Trouvé sur le séquenceur
  303S2 ; probablement la vraie cause derrière plus d'un rapport « c'est illisible » qui
  semble à première vue être un problème de taille de police.

## 6. Travail en attente / pistes

1. **Menu FX dans tous les modes audio où c'est utile** — demandé par le propriétaire (« si
   possible et utile »), analysé, **non implémenté**. L'ouverture de `OVERLAY_FX` est câblée
   mode par mode. Sans accès aujourd'hui : **LIFE, GEN, SWARM, TRACKER** (+ MIDI, EXP/EXP2/EXP3
   qui ont leur propre masque FX local, volontairement). Boutons : LIFE et GEN utilisent B1 (pause)
   et laissent B2-B4 libres ; SWARM n'a aucun cas ; TRACKER utilise les quatre (B1 play, B2 rec,
   B3 octave, B4 clear → il faudrait un double clic/appui long). Les modes purement
   visuels/utilitaires (LIGHT, LIGHTPLAY, BATTERY, SYSINFO, VID, ANIM, LANIM, PCMCLEAN, USB,
   IMPORT) n'ont pas d'audio à traiter. Marche à suivre : ajouter un `if (btn==1) { … OVERLAY_FX … }`
   sur le modèle de DR2/CRUNCH dans le second switch, ajouter la garde FX dans le dispatch de pots
   du mode (copier le bloc DR2/GROOVE), vérifier `notePreviewSafeDuringFxOverlay()` (LIFE/SWARM
   jouent sur toute la grille, or l'overlay réserve les colonnes 4-7), compléter `ctrlLabelsFor()`.
2. **Nettoyage du code de filtrage par oscillateur devenu inutile pour FILT** (`audioSetAllFilters*`
   n'est plus utilisé que par BITCRS ; `s_pcmLPF*`, `remapCutoffResToNative`,
   `audioApplyFilterTo*`, `audioSetFilterFreq`, `audioSetGranular2FilterFreq`) — `grep` les
   appelants avant de supprimer ; commentaires obsolètes (« filter-sharing FX » dans
   `overlayFxToggle`, `noFilterFx`).
3. Vérifier sur **ESP32 réel** (voir §3/§4-A), puis Android/Windows/Web.
4. RESECHO : sous-division fixée à 1/8 (l'ancien `params[3]` interne a été repris par Wild) ;
   pourrait être exposée. RES/DELAY/RESECHO(Wild<1 n'utilise plus `config_echo`) — seuls RES et
   DELAY se disputent encore le bus d'écho AMY : ne pas activer les deux.
5. Réordonner aussi les FX natifs AMY (REVERB…EQ) exigerait de scinder la boucle par bus d'AMY.
6. HPF/BPF « par voix » d'AMY ont le défaut de calibrage évoqué au §5 (non corrigé dans AMY,
   sans objet pour FILT qui n'en dépend plus).
7. Les gros fichiers (`main.cpp`) gagneraient à être découpés, mais aucune demande explicite :
   ne pas le faire sans accord.
8. **MODE_808/MODE_909 (X0X)** — scopés (« synthèse AMY, 8 voix ») en même temps que
   303A/303B, jamais commencés. 303A/303B eux-mêmes ont depuis été construits puis
   **explicitement rejetés et supprimés** (voir entrée AE au §4) — si le besoin de "sons
   X0X dédiés" revient, envisager plutôt d'enrichir 303S/303S2 (c'est ce qui a marché pour
   FELU/BREAK : nouveaux algos dans un mode existant) avant de retenter un mode jumeau.
9. **FX Punch** — phase 2 (les 8 effets "looper") est **faite** (entrée AT, moteur
   `amy_punch_*()` dans `amy.c`). Restent : (a) vérification **à l'oreille sur carte** des 8
   effets looper (CPU/PSRAM du ring buffer de 2 s sur ESP32 non mesurés) ; (b) LIFE/GEN/
   SWARM/TRACKER n'ont pas d'`OVERLAY_FX` (point 1 ci-dessus), donc `fxTriggerBtn()` y
   retourne -1 et aucun punch n'y est possible tant que le point 1 n'est pas fait.

## 7. Comment vérifier (résumé — recettes détaillées dans SIMULATOR.md)

- Mesure audio par tap dans `libminiaudio-audio.c` (RMS/pic entre états), capture OLED par
  `SDL_RenderReadPixels` **avant** `SDL_RenderPresent`, tests injectés dans `setup()` sous
  `#ifdef TEMP_DBG_*`, builds jetables `simulator/build_xxx/`. Tout retirer après.
- L'automatisation `xdotool` de la fenêtre SDL est peu fiable ici : ne pas s'y acharner.
- Un test « long » dans `setup()` bloque `loop()` ; les ticks 10 ms (anti-zipper FILT, LFO,
  `eucliTick`, `crunchTick`) doivent être appelés à la main ou contournés (appeler
  `applyFxEffect()` directement).

## 8. Carte de la documentation

| Fichier | Contenu |
|---------|---------|
| `/CLAUDE.md` | Entrée rapide : cibles de build, copie unique d'AMY, méthodologie de test, classes de bugs, conventions |
| `structure/HANDOFF.md` | Ce fichier |
| `structure/SOFTWARE.md` | Référence par sous-système : modes, canaux/bus AMY, FX et chaîne de bus, modes détaillés, bugs corrigés |
| `structure/TEMPLATES.md` | Conventions UI des modes (SYNTH / SEQUENCER / STANDALONE / MOTHOS) + conventions transverses |
| `structure/SIMULATOR.md`, `WEB_SIMULATOR.md` | Simulateur SDL2 (+ recettes de test) et build WASM |
| `structure/HARDWARE.md` | Brochage et périphériques |
| `structure/prompt_claude.txt` | Journal brut des premières demandes du propriétaire (historique, non curaté) |
| `docs/GUIDE_UTILISATEUR.md` | Manuel utilisateur (français), **pas** de la doc développeur ; ne détaille pas les FX un par un (rien à y corriger pour la refonte) |
| `inspiration/` | Notes sur produits concurrents (idées) |
