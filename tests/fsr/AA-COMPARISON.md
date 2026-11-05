# Coût des quatre options AA — RTX 5070 Ti, 18 octobre 2026

Suite des essais et choix High/Ultra désormais disponibles :
`SMAA-PRESETS-AND-OPTIMIZATIONS.md`. Le tableau ci-dessous conserve le SMAA
Ultra d'origine de la première campagne.

**Le banc ne reproduit pas un gain global de performances en activant l'AA.**
Dans les huit cas testés, Aucun a le coût GPU total le plus faible. Les autres
modes peuvent accélérer la présentation qui suit, mais cette économie ne
compense pas leur travail d'AA dans cette matrice.

## Mesure

- RTX 5070 Ti, pilote NVIDIA 616.92, Windows, Vulkan, build Release.
- Shaders de production : FXAA, SMAA **Ultra** (trois passes), CMAA2 **Medium**.
- Aucun réutilise directement la source ; aucune copie d'AA n'est ajoutée.
- Source RGBA8 optimale Vulkan, temporaires en mémoire GPU locale ; les AA
  produisent une image RGBA16F, comme dans le moteur. Sortie commune RGBA8.
- Même image, mêmes dimensions et filtre final pour les quatre modes d'un cas.
- Trois séries, chacune avec 24 images de chauffe puis 96 mesures par mode :
  **288 mesures par mode et par cas**, 9 216 mesures au total.
- Ordre des quatre modes tournant à chaque image et chaque série.
- Trois timestamps séparent AA, présentation et total ; aucun readback ni
  chargement de lookup, création de pipeline ou allocation dans les plages mesurées.
- Chronométrage final sans validation ; passe de validation séparée avec
  `VK_LAYER_KHRONOS_validation` et `VK_LAYER_VALIDATE_SYNC=1`.
- Les captures hors chronométrage vérifient que les quatre chemins rendent une
  image, qu'Aucun restitue exactement la source en natif, que les AA modifient
  les contours et qu'un aplat RGB ne change pas de plus d'un niveau sur 255.

Le renderpass LOAD des chemins FXAA/SMAA et les formats temporaires SMAA
RG16F/RGBA16F sont reproduits. Les lookup SMAA sont les tables area/search du
dépôt. Les images sont constantes pendant chaque cas ; cela ne teste pas les
changements de scène ni la rémanence d'éventuelles données temporaires.
Les barrières du banc sont conservatrices ; c'est une comparaison hors écran,
pas une capture exacte des commandes d'une frame d'émulation.

## Résultats

Temps GPU médians en **millisecondes**. Plus petit = moins coûteux.

Coût de la partie AA seule, hors présentation commune :

| Option | 720p | 1080p | 4K |
|---|---:|---:|---:|
| Aucun | 0 | 0 | 0 |
| FXAA | 0,0215 | 0,0390 | 0,1557 |
| SMAA | 0,0606 | 0,1018 | 0,3597 |
| CMAA2 | 0,0300 | 0,0463 | 0,1439 |

Aucun n'exécute aucune commande d'AA. Son intervalle entre timestamps vaut
0,000032 ms dans les données brutes : c'est le plancher d'instrumentation.

Coût **AA + présentation**, motif de contours :

| Cas | Aucun | FXAA | SMAA | CMAA2 |
|---|---:|---:|---:|---:|
| 720p natif, bilinéaire | 0,0106 | 0,0318 | 0,0709 | 0,0404 |
| 1080p natif, bilinéaire | 0,0153 | 0,0540 | 0,1168 | 0,0613 |
| 4K natif, bilinéaire | 0,0607 | 0,2007 | 0,4037 | 0,1890 |
| 720p → 1080p, Catmull-Rom | 0,0368 | 0,0577 | 0,0969 | 0,0665 |
| 4K → 1080p, Area | 0,0470 | 0,1739 | 0,3830 | 0,1679 |

Coût AA + présentation sur les autres contenus :

| Cas | Aucun | FXAA | SMAA | CMAA2 |
|---|---:|---:|---:|---:|
| 1080p bruit | 0,0152 | 0,0555 | 0,1681 | 0,1254 |
| 4K bruit | 0,0819 | 0,3054 | 0,9703 | 0,6066 |
| 1080p aplat | 0,0147 | 0,0539 | 0,0757 | 0,0562 |

Le classement FXAA/CMAA2 dépend de la résolution et de la densité de contours.
SMAA Ultra est plus coûteux sur cette matrice. Ces résultats ne constituent pas
une comparaison de qualité à réglages équivalents.

## Pourquoi une passe peut sembler plus rapide avec AA

Le code `NoAA::Draw` est vide. Le choix d'AA ne change pas ici la sélection
upscale/native/downscale ni les paramètres de netteté. Il change cependant
l'image lue par les étapes suivantes : la source d'origine avec Aucun, une
image fraîchement produite en RGBA16F avec FXAA/SMAA/CMAA2.

Dans le cas 4K → 1080p Area, le temps de **présentation seule** passe de
0,046944 ms sans AA à 0,023264 ms avec FXAA, 0,023424 ms avec SMAA et
0,023648 ms avec CMAA2. Le total reste respectivement 0,047008, 0,173888,
0,382976 et 0,167872 ms. Regarder une sous-partie peut donc donner une
impression différente du coût global.

L'hypothèse d'un effet de format, de cache ou de placement des images est
cohérente avec ces mesures, mais le banc ne distingue pas ces causes. Il ne
prouve pas la cause du gain FPS observé dans un jeu. Le compteur diagnostique
Vulkan actuel est placé autour de `window_adapt->Draw`, qui inclut la préparation
des couches et l'AA ; il n'est pas limité au dernier draw de présentation.

Les FPS du jeu dépendent aussi du CPU, de la synchronisation, du limiteur,
des fréquences GPU et du contenu. Les durées ci-dessus couvrent uniquement
AA + présentation, pas toute une frame de jeu. La source du banc n'est pas une
texture invitée capturée, et CAS/FSR, les crops, plusieurs couches et plusieurs
frames en vol ne sont pas exercés ici.

## Validation et reproduction

La matrice complète passe sans erreur de validation Vulkan/synchronisation.
Les contrôles de pixels passent pour les quatre options. L'ancien banc CMAA2
reste disponible ; son implémentation partagée est dans `cmaa2_fixture.h`.
Aucun algorithme de production n'a été modifié pour cette comparaison.

```powershell
cmake -S . -B build/windows-x64 -DNXEMU_ENABLE_INTEL_CMAA2=ON -DNXEMU_BUILD_FSR_BENCHMARK=ON
cmake --build build/windows-x64 --config Release --target nxemu-aa-vulkan-benchmark
$env:NXEMU_FSR_VALIDATION = '1'
$env:VK_LAYER_VALIDATE_SYNC = '1'
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/aa-validation
Remove-Item Env:NXEMU_FSR_VALIDATION
Remove-Item Env:VK_LAYER_VALIDATE_SYNC
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/aa-performance
```

Ajouter `quick` à la commande du banc pour un test court à dimensions impaires.

Données conservées dans `aa-results-rtx5070ti-20261018/` : `summary.csv`
(médianes, p95 et bornes des médianes par série), `samples.csv` (chaque mesure),
`checks.csv` (différences RGB, **pas** des scores de qualité), `device.txt`
et `validation.txt`. Les PPM de contrôle sont générés dans le dossier de sortie
du banc, sans être ajoutés au dépôt.
