# Créer (ou retirer) un mode — checklist de câblage

`TEMPLATES.md` documente les **conventions UX** (quel bouton fait quoi par template).
Ce document est son complément mécanique : la liste exhaustive des endroits de `main.cpp`/
`config.h` qu'un nouveau mode doit toucher, dans l'ordre, plus les pièges qui ont déjà fait
perdre du temps dans ce projet. Lire `TEMPLATES.md` pour choisir le template ; revenir ici
pour ne rien oublier au câblage.

## 0. Avant d'écrire du code

- **Un moteur existant couvre-t-il déjà le besoin DSP ?** Avant d'écrire un nouvel engine
  audio, chercher dans `audio_engine.h` si un moteur déjà présent peut être réutilisé avec
  un slot/oscillateur dédié plutôt que dupliqué. Exemple : `MODE_BREAK` (générateur de
  breakbeat) n'a pas de moteur DSP à lui — il pilote le moteur `GRANULAR2` existant
  (`audioLoadGranular2Source`/`audioComputeGranular2Slices`/`audioPlayGranular2`) sur un
  `sampleIdx`/oscIdx dédié. Beaucoup moins de code, beaucoup moins de bugs qu'un moteur
  neuf.
- **Un mode "test" doit être un vrai `AppMode`**, pas un hack dans un mode existant — voir
  la convention du projet : les features risquées se développent dans un nouveau mode
  validé isolément, fusionné (ou jeté) après coup.
- Décider du template (SYNTH / SEQUENCER / STANDALONE / MOTHOS TRACKER, voir
  `TEMPLATES.md`) et noter en commentaire, en tête du bloc d'état du mode, la sémantique de
  chaque pot/bouton/axe joystick — ce commentaire sert aussi de documentation pour
  `ctrlLabelsFor()` plus bas.

## 1. Points de branchement obligatoires (ajout d'un mode)

Dans l'ordre où ils apparaissent dans `main.cpp`/`config.h` :

1. **`config.h`** :
   - `enum AppMode` — ajouter `MODE_X` **juste avant `MODE_COUNT`** (point d'insertion le
     plus sûr : pas besoin de recompter les entrées existantes).
   - `enum MenuItem` — ajouter `MENU_X` **juste avant `MENU_ITEM_COUNT`**.
   - `menuLabels[]` — ajouter le label court, **même position relative** que dans l'enum.
2. **Bloc d'état du mode** (`main.cpp`) : variables + helpers (RNG, etc.) groupés sous un
   commentaire `// ==================== X STATE ====================`. Déclarer **avant**
   tout code qui les utilise (voir piège « ordre de déclaration » ci-dessous).
3. **`kMenuCatInstr[]` / `kMenuCatSeq[]` / `kMenuCatAut[]`** — ajouter `MENU_X` dans le bon
   tableau de catégorie, **et incrémenter l'entrée correspondante de `kMenuCatSizes[]`**
   (oubli facile : le tableau compile très bien avec une taille fausse, le symptôme est un
   item de menu manquant ou un décalage silencieux).
4. **`dispatchMenuItem()`** — `case MENU_X: switchMode(MODE_X); break;`
5. **`switchMode()` — hook de sortie** (`if (currentMode==MODE_X) { ... }`, en haut de la
   fonction) : couper proprement ce qui tournait (note-off, reset d'une pile de touches
   tenues, etc.). Même sans son à couper, **vider tout état "touche tenue"** — une touche
   relâchée après avoir changé de mode ne repassera jamais par le `case MODE_X` de
   `handleNoteKeyAudio()`.
6. **`switchMode()` — hook d'entrée** (`if (newMode==MODE_X) { ... }`) : pickup des pots
   P4-P7 (`for(int _p=0;_p<4;_p++) lpX[_p]=pots[3+_p].value;` si le mode a ses propres
   trackers statiques), reset de l'état transitoire, **`potPopupReset()` est déjà appelé
   globalement, pas besoin de le refaire ici**.
7. **`kIsSeq[MODE_COUNT]`** (tableau positionnel dans `switchMode()`) — ajouter une entrée
   à la **même position relative** que dans l'enum `AppMode`. Un tableau `bool` trop court
   se complète silencieusement à `false` (pas d'erreur de compilation) — bug difficile à
   repérer.
8. **`kVizMode[MODE_COUNT]`** (tableau positionnel, juste après) — même chose, mais un
   tableau `const char*` trop court plante (`printf("%s", nullptr)`) plutôt que de se
   compléter silencieusement.
9. **`ctrlLabelsFor()`** — un `case MODE_X:` qui retourne les 9 labels (P2, P4-P7, B1-B4).
   **C'est la seule source de vérité pour les labels affichés par `drawBtnRow()` et par le
   popup de potards** — voir piège « texte redondant » ci-dessous.
10. **`handleButton()` — bloc `btn==0` (B1 court)** : `case MODE_X:` pour le toggle
    play/stop si le mode a son propre transport.
11. **`handleButton()` — switch principal B2-B4** : `case MODE_X:` pour B2 (FX en général),
    B3, B4.
12. **`handleNoteKeyAudio()`** : `case MODE_X:` pour l'interprétation de la grille 4×8.
13. **`notePreviewSafeDuringFxOverlay()`** : seulement si le mode doit rester jouable
    pendant qu'un overlay FX est ouvert (cas rare — la plupart des modes n'ont rien à faire
    ici, le `default: return false;` suffit).
14. **Pot-dispatch** (`loop()`, gros switch par mode) : `case MODE_X:` pour P2 et P4-P7 —
    appeler `potPopupShow(label, valeurFormatée)` à chaque changement détecté (le label
    doit correspondre à ce que `ctrlLabelsFor()` renvoie pour ce pot, par convention, même
    si ce n'est pas vérifié au compile).
15. **Joystick** (`loop()`, bloc dédié) : si JX/JY ont un rôle dans ce mode.
16. **`drawScreen()`** : `case MODE_X: { ... }` — en-tête + `drawBtnRow(MODE_X, 17)` +
    contenu spécifique. **Ne jamais réécrire en texte ce que `drawBtnRow()` affiche déjà**
    (voir piège ci-dessous).
17. **`updateLedsAndShow()`** : `case MODE_X: { ... }` pour la grille de LEDs.
18. **Tick du séquenceur** (si le mode a un transport automatique) : un bloc **indépendant**
    dans `loop()`, pas accroché à `drum2Playing`/`drum2Step` sauf besoin explicite de
    partager l'horloge d'un autre mode — voir le commentaire de GROOVE dans `main.cpp` pour
    la justification (éviter qu'un mode tourne en fond et pollue un autre mode par la même
    horloge).

## 2. Retirer un mode proprement (symétrique du point 1)

Un mode non validé par l'utilisateur doit être **retiré intégralement**, pas juste
désactivé dans le menu : `grep -n "MODE_X\|MENU_X"` dans `main.cpp` et `config.h` doit ne
plus rien renvoyer une fois fini. Points **positionnels** à ne pas oublier dans l'autre
sens (ce sont ceux qui cassent silencieusement si on les oublie) :
- `kIsSeq[]` / `kVizMode[]` / `menuLabels[]` perdent une entrée **et l'enum raccourcit
  d'une position** — les entrées placées *après* le mode retiré ne bougent pas de nom mais
  leur index change ; vérifier qu'aucun code ne codait en dur un index numérique à la
  place du nom d'enum.
- `kMenuCatSizes[]` redescend d'une unité dans la bonne catégorie.

## 3. Pièges déjà rencontrés dans ce projet

- **Texte redondant avec `drawBtnRow()`.** `drawBtnRow(MODE_X, 17)` affiche déjà B1-B4 en
  haut de l'écran à partir de `ctrlLabelsFor()`. N'ajouter **aucune** ligne du type
  `"B3=Fichier B4=Reseed"` ailleurs dans le même écran — c'était exactement le genre de
  doublon que le chantier B1-B4 (voir `HANDOFF.md`) a éliminé partout ailleurs ; ne pas le
  réintroduire dans un mode neuf. Si un contrôle n'est pas un bouton (axe joystick, par
  exemple), le documenter en texte est légitime (`"JY=parcourir"` dans MODE_DJ/MODE_BREAK).
- **Tableaux positionnels `kIsSeq`/`kVizMode`** (voir CLAUDE.md, section « Recurring bug
  classes ») — toujours ajouter l'entrée à la bonne position, jamais juste à la fin si le
  nouveau mode n'est pas le dernier de l'enum.
- **Ordre de déclaration en C++.** Un bloc d'état placé tôt dans `main.cpp` (pour être visible
  des helpers définis juste après) ne peut pas appeler des fonctions définies plus loin dans
  le fichier (`sdListDir`, `ctrlLabelsFor`, etc.) — soit le bloc d'état est purement des
  variables/structs (OK partout), soit la logique qui appelle ces fonctions est écrite au
  site d'appel (hook `switchMode`, `handleButton`, etc.) plutôt que dans un helper séparé
  trop tôt dans le fichier.
- **FX partagé qui n'atteint pas un nouveau range d'oscillateurs dédié** (voir CLAUDE.md) —
  si le mode utilise un oscIdx dédié hors de la plage clavier normale, vérifier s'il doit
  aussi être branché dans `audioSetAllFilters()`/`audioSetAllFiltersT()`.
- **Semaphore `s_oledDone` et tests headless.** `switchMode()` appelle déjà `drawScreen()`
  en interne à la fin — un second appel manuel à `drawScreen(false)` juste après dans un
  test peut être silencieusement ignoré (semaphore pas encore libéré par `displayTask`).
  Utiliser `drawScreen(true)` (bloquant, jusqu'à 150ms) pour un screenshot de test fiable.

## 4. Avant de considérer le mode terminé

Voir `CLAUDE.md`, section « Testing methodology » pour la méthode complète (build
simulateur, injection headless dans `setup()`, capture d'écran OLED via
`oled.getBufferPtr()`, tap RMS/peak pour l'audio). En résumé pour un nouveau mode :
1. Build simulateur propre (`cd simulator && ./build.sh`).
2. Vérifier visuellement au moins l'écran principal **et** tout écran secondaire
   (navigateur SD, overlay, etc.) via une capture headless.
3. Grep la diff pour tout `TEMP DBG`/code de test avant de finir.
4. Build ESP32 seulement si le mode touche du matériel/USB sensible — sinon le build
   simulateur + capture suffit pour un mode purement logiciel/audio.
