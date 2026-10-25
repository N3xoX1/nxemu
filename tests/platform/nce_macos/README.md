> Mise à jour de cette copie : le patcher Darwin de production est maintenant compilé directement, avec sa clé TSD et son champ `pstate` uniques. A3.22 teste le scanner rtld 36 bits. Le test T4.16 reprend les interruptions sans motif comme le runtime de production. Les verdicts et explications ci-dessous sont le relevé historique antérieur à l'intégration . « Aucun échec bloquant » dans cette suite ne valide pas à lui seul un jeu 36 bits.

# Validation du NCE sur macOS Apple Silicon

Tests reproductibles qui décident si le NCE de NXEmu (`src/nxemu-cpu/nce/`) peut tourner sur macOS arm64, avant toute intégration. Rien dans `src/` n'est modifié : la suite compile le **vrai** `nce/patcher.cpp`, avec une seule substitution mécanique (voir « Ce que la suite exécute »).

```bash
tests/platform/nce_macos/run_all.sh
```

Durée : environ 15 s de tests après la compilation. Le script compile dans `build/nce_macos_validation/`, lance les étapes 2, 3 et 4, et écrit `results/<date>_<machine>.txt` et `.tsv`. On peut lancer une seule étape : `run_all.sh memory|arm64|transitions`.

Résultat de référence : `results/20261020-124455_Apple-M4-Pro_macOS-26.6.2.txt` (M4 Pro, 12 cœurs, macOS 26.6.2), 64 résultats. L'exécutable de validation déclare le SDK 12.3 (`tests/platform/CMakeLists.txt`), comme NxEmu devra le faire ; `nce_macos_validate_current_sdk` est le même programme avec le SDK installé, utilisé comme témoin.

## Verdicts

| Étape | Verdict | Résumé |
|---|---|---|
| 1. Audit | fait | 12 dépendances Linux, toutes avec un équivalent Darwin testé (tableau ci-dessous) |
| 2. Mémoire (bloquant) | **GO sous conditions** | Mapping identité possible sous 512 Gio, mais pas sur [63 Gio, 448 Gio). 4 Kio sur 16 Kio exact grâce au plan + émulation. Le support shm actuel n'est pas exécutable : il faut une entrée mémoire Mach |
| 1 bis. Vrais modules | fait | 32 jeux, 1,7 Gio de code : 0,13 % de pages 16 Kio scindées (5 à 19 par jeu), 1,5 M d'instructions x18, aucun CNTFRQ/CNTVCT, 94 CTR_EL0 |
| 3. ARM64 | **GO sous conditions** | x18 conservé par XNU si l'exécutable déclare un SDK < 13 : plus de réécriture x18. Le différentiel original/réécrit passe (93 sites x0-x30, CNTPCT, exclusifs, SVC). TPIDR_EL0 est écrasé par le noyau : le patcher doit le prendre en compte |
| 4. Transitions | **GO sous conditions** | Entrées/sorties exactes, 20 M de SVC sous interruptions sans perte, coexistence avec les ports Mach. Deux défauts existants (FPCR hôte, NZCV) |

Classes de vérification : **bloquant** (l'échec n'a pas de contournement exact : NO-GO), **changement** (l'échec montre que le NCE ne marche pas tel quel ; la correction est validée par un autre test), **info** (mesure).

## Étape 1 — Audit des dépendances Linux

| Dépendance | Où | Sur Darwin | Test |
|---|---|---|---|
| En-têtes `asm-generic/signal.h`, `unistd.h`, `__NR_tkill` | `arm_nce_asm_definitions.h:8` | `<signal.h>` ; `__pthread_kill` (328) | build (shim) |
| Directives ELF (`.section … %progbits`, `.type`), symboles sans `_` | `arm_nce.s` | Mach-O : `src/nce_darwin.S` | build |
| Entrée par `tkill` sur soi-même via `svc #0` avec x8 | `arm_nce.s:64` | `svc #0x80` avec x16 = 328 | T4.1 |
| TPIDR_EL0 comme pointeur de thread hôte, échangé à chaque transition ; magic TLS pour séparer invité et hôte | `arm_nce.s` (8 accès), `patcher.cpp` (9 lectures, 1 écriture) | Le noyau écrit TPIDR_EL0 (numéro de CPU) : slot TSD de TPIDRRO_EL0, et décision invité/hôte par le PC | A3.3, A3.4, T4.15 |
| `mcontext_t` Linux, `fpsimd_context`, `FPSIMD_MAGIC` | `arm_nce.cpp:56`, `interpreter_visitor.h:100` | `__darwin_mcontext64` (`__ss`, `__ns`, `__es`) | T4.1 |
| `gettid()`, `syscall(SYS_tkill)` | `arm_nce.cpp:265,367` | `pthread_kill` / port Mach du thread | T4.13 |
| SIGSEGV = accès, SIGBUS = alignement | `arm_nce.cpp:288-314` | Faute de protection et page PROT_NONE → SIGBUS ; non mappé → SIGSEGV. Classer par ESR | M2.19 |
| `InvalidateNCE` à 4 Kio, `DeferredMapSeparateHeap` réservé à Linux | `arm_nce.cpp:68,159`, `memory.cpp:1205` | Plan 4K→16K | M2.21-M2.25 |
| NCE compilé seulement pour Android/Linux | `nxemu-cpu/CMakeLists.txt:499` | nouvelle branche Apple | — |
| `NceEnabled` exige le fastmem | `cpu_settings.cpp:136` | Fastmem désactivé sur 16 Kio (`host_memory.cpp:531`) : le NCE a besoin d'un mapping direct à lui, indépendant de Dynarmic | M2.6 |
| Horloge native NCE réservée à Linux | `core.cpp:128` | CNTVCT lisible en EL0 | A3.7 |
| Espace 39 bits ramené à 38 bits pour le NCE | `k_address_space_info.cpp:29` | Disposition à adapter aux fenêtres Darwin | M2.5, M2.7 |

## Étape 1 bis — Mesure sur de vrais modules Switch

`nso_survey/survey.py` lit les jeux déchiffrés (formats NXEmu `DNCA`/`DXCI`, NSP et XCI), décompresse rtld, main, subsdk* et sdk, les dispose comme le chargeur de NXEmu (modules à la suite, alignés sur 4 Kio), puis classe chaque page hôte de 16 Kio et chaque instruction du `.text`.

```bash
python3 -m venv /tmp/venv && /tmp/venv/bin/pip install lz4 capstone numpy
/tmp/venv/bin/python -I tests/platform/nce_macos/nso_survey/survey.py --json survey.json <jeux...>
```

Résultats sur 32 programmes (dernière version installée de chaque jeu, 429 M de mots de code, 2 min 48 s) :

- **Pages scindées** : 282 pages de 16 Kio sur 218 683 (0,13 %), de 5 à 19 par jeu, toutes aux frontières : fin de `.text`/début de `.rodata` (RX|R), fin de `.rodata`/début de `.data` (R|RW), et fin du `.bss` d'un module/début du `.text` du suivant (RW|RX). Aligner la base des modules sur 16 Kio ne réduit pas leur nombre (289) : il remplace les frontières RW|RX par des queues de `.bss` (RW|--). Ces dernières disparaissent si le chargeur arrondit l'image du module à 16 Kio (pages invitées et hôte alors cohérentes).
- **x18** : 1 522 526 instructions (0,35 % du code), dans tous les jeux, de 12 000 à 104 000 par jeu. C'est un temporaire ordinaire du compilateur Nintendo : dans TOTK, sa fréquence suit celle de x9..x17 (x16 : 10 700, x17 : 8 062, x18 : 6 406 sur 4 Mio). Le réécrire site par site coûterait une dizaine d'instructions par exécution dans le code le plus chargé en registres. L'étape 3 rend cette réécriture inutile.
- **Registres système à traiter** : MRS TPIDR_EL0 62 159, MRS TPIDRRO_EL0 3 071, SVC 2 937, MRS CNTPCT_EL0 1 229, MRS/MSR FPCR/FPSR/NZCV (natifs), MSR TPIDR_EL0 105, **MRS CTR_EL0 94** (refusé par macOS, A3.11), IC IVAU/DC CIVAC/DC CVAU (natifs). Aucun MRS CNTFRQ_EL0 ni CNTVCT_EL0.
- Faux positifs : une centaine de mots « registre système exotique » vus une seule fois et environ 2 900 « atomiques LSE » (absents du Cortex-A57) sont des données dans `.text`. Le patcher ne doit jamais s'arrêter sur un mot qu'il ne reconnaît pas : il le laisse tel quel, et s'il est exécuté le CPU lève la même exception que sur Switch.

## Étape 2 — Mémoire

Constats :

- **Pas de pages 4 Kio** pour un processus arm64 natif (M2.2). Seuls les processus x86_64 sous Rosetta en ont.
- **Rien sous 4 Gio** : un `__PAGEZERO` réduit est refusé à l'exec (M2.3). Les processus 32 et 36 bits ne peuvent pas avoir un mapping identité.
- **[63 Gio, 448 Gio) est réservé en permanence par le noyau** : `mmap` → EPERM, `mach_vm_map` OVERWRITE → `KERN_PROTECTION_FAILURE` (M2.5). Fenêtres utilisables : [448 Gio, 512 Gio) (64 Gio, fixe) et les trous entre 4 Gio et 63 Gio. Exécutable déclarant le SDK 12.3 (configuration retenue, étape 3) : 52,9 Gio dont **un trou continu de 51 Gio [12 Gio, 63 Gio)**, identique sur 3 lancements. Exécutable déclarant le SDK actuel : environ 29 Gio en 5 morceaux, car le nouvel allocateur malloc réserve environ 23 Gio à une position aléatoire (plus grand morceau de 13,6 à 26,8 Gio) (M2.4, M2.6).
- Les régions 39 bits d'Horizon de taille d'origine ne tiennent pas toutes : la région kernel-map de 64 Gio n'a pas de place (M2.7).
- Une fois les fenêtres réservées au démarrage, l'hôte (mmap, malloc, piles, Metal) alloue au-dessus de 512 Gio sans problème (M2.8).
- **Le noyau arrondit en silence** : `mprotect` sur 4 Kio agit sur 16 Kio, `mach_vm_protect` aussi (M2.11, M2.12). Les adresses et décalages non alignés sont refusés (M2.9, M2.10, M2.13).
- **Le code n'est pas exécutable depuis un objet shm** (le support actuel de `HostMemory` sur Apple) : faute d'instruction (M2.15). Une **entrée mémoire Mach nommée** mappée en RW et en RX à deux adresses fonctionne, et une réécriture par l'alias RW est vue par l'alias RX (M2.16). Les alias sont cohérents et ont des permissions indépendantes (M2.14).
- Fautes : FAR exact à l'octet, ESR fiable pour donnée/instruction, lecture/écriture et alignement. Le FSC traduction/permission n'est **pas** fiable (pages paresseuses), donc la décision se prend avec la table des pages invitée (M2.18).
- **Plan 4K→16K exact** (`src/nce_page_plan.h`) : une page hôte est directe si ses 4 pages invitées ont la même permission et un bloc de support aligné de 16 Kio ; sinon elle reçoit l'intersection des permissions et les accès refusés sont émulés après contrôle de la page de 4 Kio. Vérifié sur les 65 536 combinaisons : l'hôte n'accorde jamais plus que l'invité (M2.25). Testé en vrai : écriture permise émulée, écriture interdite = faute invitée exacte (M2.21), 1,95 µs par accès émulé (M2.22). Sur 200 modules synthétiques, 0,18 % des pages sont scindées, au plus 3 par module, toujours aux frontières de segments (M2.26).
- Exécuter du code dans une page scindée est détecté exactement (M2.23), mais ne peut pas se faire nativement (M2.24) : c'est une **décision** à prendre.

## Étape 3 — ARM64

- **x18 : conservé si l'exécutable principal déclare un SDK antérieur à 13.0.** Par défaut, XNU remet x18 à zéro à chaque retour en espace utilisateur (témoin A3.2 : 1000/1000 après un appel système ou un signal, une préemption sur trois à cinq). Le code de retour d'exception de XNU (`locore.s`) ne restaure x18 que si le bit 48 de TPIDR_EL0 (`MACHDEP_TPIDR_FLAG_PRESERVE_X18`) est posé. Le noyau le pose pour tout le processus quand l'exécutable principal déclare un SDK < 13.0 (compatibilité avec les anciens binaires, test `x18_legacy` de XNU). Mesuré ici : SDK 10.12 à 12.3 → 0 perte ; SDK 13.0 et 26 → pertes. Seul le champ SDK de `LC_BUILD_VERSION` compte (la version minimale non) ; le runtime renforcé ne change rien. Avec la suite liée en SDK 12.3 : 0 perte après préemption sous charge, appel système BSD, appel Mach qui cède le CPU, signal, et la valeur écrite dans le contexte d'un signal est appliquée au retour (A3.1). Les différentiels et les transitions vérifient maintenant x18 comme les autres registres (A3.13, T4.1, T4.13). Les autres voies ne conviennent pas : `os_set_custom_x18_abi_enabled` (macOS 26.4) pose le bit, mais le noyau l'efface au changement de contexte suivant sans le droit privé `com.apple.private.custom-x18-abi`.
- Effet de bord du SDK 12.3 : CNTFRQ_EL0 et le compteur exposé passent de 1 GHz à **24 MHz** (A3.6). Le patcher doit lire la fréquence hôte à l'exécution, jamais la supposer.
- **TPIDR_EL0 est écrasé par le numéro de CPU** (A3.3). TPIDRRO_EL0 + slot TSD est stable sous préemption (16 M lectures, A3.4).
- **Un SVC non patché est un vrai appel système macOS** : l'immédiat est ignoré et le x16 invité choisit l'appel (A3.5).
- CNTFRQ_EL0 vaut 24 MHz avec le SDK 12.3 et 1 GHz avec le SDK actuel (A3.6). CNTVCT et CNTPCT sont lisibles en EL0 (A3.7, A3.8). Le patcher laisse CNTVCT tel quel (A3.9) et s'arrête sur CNTFRQ (UNREACHABLE, A3.10).
- **`MRS CTR_EL0` est refusé en EL0 par macOS** (SIGILL), alors que DC ZVA/CVAU/CVAC/CIVAC, IC IVAU et DCZID passent (A3.11). DCZID donne 64 octets, comme le Cortex-A57 (A3.12).
- Différentiel original/réécrit avec le vrai patcher : 93 sites MRS/MSR TPIDR* (x0-x30) × 24 états aléatoires complets, 0 divergence (A3.13). Le contrôle négatif détecte les deux mutations (A3.14). CNTPCT réécrit : registres exacts, 19,2 MHz à moins de 0,1 % près sur 50 ms (A3.15). LDXR/STXR → LDAXR/STLXR : exact (A3.16), coût ×1,6 sous contention (A3.19). SVC : état complet, numéro, PC+4, puis retour par le trampoline (A3.17). Branchements sur un module de 64 Mio (A3.18). Exclusifs sous tempête de signaux (A3.21).
- Écart matériel non corrigeable : un LDXR non aligné à l'intérieur de 16 octets ne faute pas sur M4 (FEAT_LSE2) mais faute sur Cortex-A57. Seul un programme qui planterait sur Switch le voit (M2.20).

## Étape 4 — Transitions

- Entrée par signal avec contexte complet puis sortie : 2000 états aléatoires exacts, 3,2 µs l'aller-retour (T4.1).
- Registres hôte préservés (x19-x28, d8-d15, sp) à travers les sorties SVC et signal (T4.2).
- 200 000 SVC invité → hôte → invité par les trampolines du patcher, 30 ns l'aller-retour hors travail hôte (T4.5).
- Pile invitée de 64 octets au-dessus d'une page de garde : aucun accès hors pile (T4.8). Fautes invitées exactes, fautes hôtes chaînées (T4.9, T4.10).
- Un port d'exceptions Mach qui décline (comme celui de Dynarmic hors JIT) laisse passer le signal BSD, mais le coût d'une faute passe de 3,1 µs à environ 9 µs (T4.11, T4.12).
- Interruption d'une boucle infinie : 3000 fois exacte, latence médiane 2,4 µs (T4.13). 20 M de SVC sous 36 000 interruptions aléatoires : ni perte, ni doublon, ni blocage (T4.14). SIGURG dans du code hôte ignoré (T4.15). Exclusifs sur 2 cœurs sous interruptions exacts (T4.16).
- **Défaut existant, aussi sous Linux** : après une sortie SVC, le code hôte tourne avec le FPCR du jeu (arrondi, flush-to-zero) (T4.4).
- **Défaut existant, aussi sous Linux** : la sortie SVC écrit `GuestContext::nzcv`, mais `GetContext`/`SetContext` ne copient que `pstate`. Un thread qui reprend sur un autre cœur après un SVC récupère les drapeaux d'un autre thread (T4.7).

## Changements requis

Mémoire (couche réservée au NCE, `HostMemory` de Dynarmic inchangé) :

1. Réserver au démarrage, avant Metal/MoltenVK, [448 Gio, 512 Gio) et les trous libres entre 4 Gio et 63 Gio. Libérer si le jeu utilise Dynarmic.
2. Support du NCE : une entrée mémoire Mach nommée (`mach_make_memory_entry_64`), mappée par `mach_vm_map(FIXED|OVERWRITE)` avec des décalages alignés sur 16 Kio. Pas `shm_open`.
3. Toutes les opérations de mapping et de protection passent par le plan 4K→16K (`nce_page_plan.h`). Vérifier (assert) que chaque appel au noyau est aligné et multiple de 16 Kio.
4. Fautes sur pages scindées : contrôler la permission de la page invitée de 4 Kio, puis émuler par l'interpréteur (`InterpreterVisitor`, à adapter aux contextes Darwin) ou rendre une faute invitée. Le même gestionnaire sert SIGBUS et SIGSEGV, et classe par ESR/FAR.
5. Allocateur physique du noyau HLE : pour le NCE, donner des blocs de 16 Kio alignés et congrus à l'adresse virtuelle, pour que les pages restent directes.

Noyau HLE :

6. Disposition 39 bits adaptée : région ASLR [bas, 512 Gio) avec [63 Gio, 448 Gio) marqué inaccessible dans KPageTable ; alias dans [448, 512) ; code, heap et pile dans les trous bas ; région kernel-map réduite et annoncée par `svcGetInfo`.

Patcher (`patcher.cpp`) :

7. Lire les paramètres de thread par `mrs tpidrro_el0` + `ldr [slot TSD]`, et ne plus écrire TPIDR_EL0. C'est la substitution déjà validée par A3.13 à A3.17.
8. ~~Réécrire x18~~ : remplacé par une ligne d'édition de liens. Lier l'exécutable principal de NxEmu avec `-Wl,-platform_version,macos,<min>,12.3` (aujourd'hui SDK 27.0). Au démarrage, vérifier le bit 48 de TPIDR_EL0 et faire un aller-retour x18 à travers un appel système ; sinon repli sur Dynarmic. Le patcher ne doit pas toucher x18.
9. Intercepter CNTVCT_EL0 (mise à l'échelle 19,2 MHz, fréquence hôte lue à l'exécution), CNTFRQ_EL0 (constante 19 200 000 au lieu de UNREACHABLE) et CTR_EL0 (94 sites réels, valeur à choisir explicitement). Laisser intact tout mot non reconnu (données dans `.text`).
10. Aligner la taille et le début de la section de patch sur 16 Kio (aujourd'hui 4 Kio, M2.27).
11. Couvrir 100 % des SVC avant toute exécution, modules chargés plus tard compris (NRO via `ro`) ; sinon pas de NCE pour ce processus.

Runtime (`arm_nce.cpp`, `arm_nce.s`) :

12. Port Mach-O des trampolines (`src/nce_darwin.S` sert de base), contexte `__darwin_mcontext64`, interruptions par `pthread_kill`, invité/hôte décidé par le PC dans les fenêtres.
13. Sauvegarder/restaurer FPCR et FPSR hôte dans le trampoline SVC et les sorties par signal.
14. Un seul champ de drapeaux dans `GuestContext`, utilisé par les sorties SVC et signal, `Get/SetContext` et `LoadContext`.

Décision à prendre :

- **Code exécutable dans une page scindée** (fin de `.text` qui partage une page de 16 Kio avec `.rodata`) : soit un interpréteur A64 complet pour ces pages (exact, lent), soit une politique RX explicite et journalisée par page (rend exécutables au plus 12 Kio de `.rodata`). Jamais implicite.

## Repli sur Dynarmic avant le lancement

Le NCE n'est choisi que si toutes ces conditions tiennent. Sinon le jeu démarre directement avec Dynarmic, sans changer Dynarmic :

- processus 64 bits avec espace 39 bits (les 32 et 36 bits vont à Dynarmic) ;
- fenêtres réservées au démarrage et disposition adaptée qui tient dedans ;
- x18 conservé par le noyau (bit 48 de TPIDR_EL0 et aller-retour à travers un appel système vérifiés au démarrage) ;
- patcher capable de réécrire tous les sites (SVC, registres système) de tous les modules ;
- pages exécutables scindées traitées selon la décision ci-dessus.

## Ce que la suite exécute

- `tests/platform/CMakeLists.txt` compile directement le vrai `src/nxemu-cpu/nce/patcher.cpp`, sans substitution.
- `src/nce_runtime.*` et `src/nce_darwin.S` forment un runtime NCE Darwin minimal. Il garde `GuestContext`, `NativeExecutionParameters`, le verrou et le protocole de `PhysicalCore` (`EnterContext`/`ExitContext`, `SignalInterrupt`).
- Différentiel : l'instruction d'origine est modélisée (registre destination = valeur attendue, tout le reste inchangé), le code réécrit tourne nativement depuis un état aléatoire complet, et les deux états sont comparés : x0-x30, sp, pc, NZCV, FPCR, FPSR, v0-v31, TPIDR_EL0. C'est le principe d'`equiv.py` de d2r, en exécution native au lieu d'Unicorn, avec le même type de contrôle négatif par mutation.
- `T4.7` utilise une copie de `ArmNce::GetContext/SetContext`, qui ne peut pas être compilé sur macOS en l'état.

## MacNativeGames

- **Repris comme technique, pas comme code** : les trampolines `gate.S` et `hle_stack.S` confirment le choix TPIDRRO_EL0 + slot TSD. `equiv.py` (états aléatoires, comparaison complète, mutations) a guidé le différentiel. `patch_nso.py` réécrit x18 site par site (slot TSD, fenêtres, tables de saut, exceptions par build) : ce n'est plus nécessaire grâce au SDK déclaré 12.3. Licence MIT (d2r-mobile, `games/totk/third_party/d2r/LICENSE`), compatible GPL : si du code est repris plus tard, garder l'en-tête et le fichier de licence.
- **Pas repris** : `loader.c` donne à chaque page de 16 Kio l'**union** des permissions de ses pages de 4 Kio. C'est une approximation silencieuse. Il recharge aussi les modules à des adresses hôte quelconques après une réécriture statique. Le plan exact de cette suite le remplace.

## Limites

- La mesure sur vrais modules est statique : elle compte les pages scindées, pas la fréquence des accès émulés pendant le jeu. Celle-ci reste à mesurer (sous Dynarmic, en retirant ces pages de la table de pages de NXEmu pour compter les accès par les callbacks, sans changer Dynarmic).
- Effet du SDK déclaré 12.3 sur le reste de NxEmu : une copie de `NxEmu.app` modifiée par `vtool -set-build-version macos 12.0 12.3` puis resignée donne, sur le menu de Super Mario Odyssey sans limite de vitesse (75 s, frames après 40 s), 350 fps contre 351 pour l'original (médiane 2,867 ms contre 2,858 ms, p99 identique) et les mêmes erreurs dans le journal. Reste à vérifier en jeu, sur d'autres jeux et visuellement : c'est un changement de tout le processus (AppKit, Metal/MoltenVK, SDL, timebase à 24 MHz).
- Les coûts mesurés sont ceux des transitions, pas de jeux réels.
- Une seule machine testée (M4 Pro, macOS 26.6.2). La position des réservations malloc et la taille des trous bas varient d'un lancement à l'autre (M2.6).
