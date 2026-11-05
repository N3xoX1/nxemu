# ScaleForce rapide — variante séparée

Ajout du filtre persistant `ScaleForceFast` (identifiant 9), à côté de
ScaleForce classique (identifiant 4 conservé). Le menu affiche « ScaleForce
rapide ». Le mode est disponible sur Vulkan (FP16 si pris en charge, FP32
sinon) et OpenGL (FP32). CAS reste désactivé pour les deux ScaleForce.

## Simplification

Le calcul de direction utilise quatre voisins cardinaux au lieu de huit.
La distance colorimétrique YCbCr, ses racines carrées, les poids et le limiteur
de déplacement restent ceux du filtre original. Les quatre contributions
diagonales sont omises. Le nombre maximal d'échantillonnages passe de 10 à 6,
dans une seule passe. C'est une approximation, pas une équivalence de rendu.

Le shader commun est compilé avec `SCALEFORCE_FAST` pour cette variante.
Sans ce drapeau, le chemin classique est conservé. Les dépendances CMake
incluent désormais le shader commun pour les variantes classiques et rapides,
afin que sa modification reconstruise correctement les en-têtes SPIR-V.

## Mesures hors jeu sur RTX 5070 Ti

30 tours de chauffe, puis 160 mesures alternées par pipeline, GPU au repos.
Les temps sont ceux du passage GPU, et ne prédisent pas le gain de FPS global.

| Cas (FP16) | Classique | Rapide | Réduction du coût GPU |
|---|---:|---:|---:|
| 720p-1080p | 0.039616 ms | 0.028864 ms | 27.1 % |
| 720p-4k | 0.136512 ms | 0.093824 ms | 31.3 % |
| mario-window | 0.031392 ms | 0.023328 ms | 25.7 % |
| native | 0.021600 ms | 0.016608 ms | 23.1 % |
| pixel-art | 0.023232 ms | 0.017664 ms | 24.0 % |

## Qualité et limites

Le mode classique FP32 est comparé à une copie figée indépendante du shader
initial : pixels identiques sur les dix cas. Les variantes FP16 et FP32 sont
compilées et exécutées. Noir/blanc constants à alpha 137 sont conservés.
Les dix cas couvrent upscale, downscale, natif, flip, crop décalé, pixel art
et couleurs constantes. Zéro erreur de validation Vulkan ; tests C++ des
réglages/traductions/métriques et test Python des phases CSV réussis.

La version rapide change sensiblement certains contours. Sur les mires
courantes, 2 à 7 % des pixels ont un écart RGB supérieur à 2/255, avec des
écarts ponctuels pouvant atteindre 130/255. Sur la mire pixel-art très agrandie,
environ 21 % des pixels dépassent ce seuil, erreur moyenne 3/255, PSNR 29,5 dB.
Ces écarts peuvent être visibles : aucune amélioration générale de qualité
n'est revendiquée. La variante peut sembler plus lisse sur certains détails
et moins fidèle sur d'autres, notamment les diagonales et les pointes fines.
L'alpha est toujours échantillonné avec la couleur déplacée ; une direction
différente peut donc aussi changer l'alpha, particulièrement aux bords de
texture. Il n'est ni forcé à 1 ni déclaré identique au filtre classique.

Le cas « mario-window » utilise une texture 1600×900 recadrée sur 1280×720,
vers 1652×929. Il s'agit d'une mire synthétique, pas d'une capture de Mario.
La validation en jeu reste nécessaire : comparer ScaleForce puis ScaleForce
rapide, même scène/caméra, 20–30 s par mode, menu fermé.

Reproduction : activer `NXEMU_BUILD_FSR_BENCHMARK`, compiler la cible
`nxemu-scaleforce-vulkan-benchmark`, puis fournir un répertoire de sortie.
La variable `NXEMU_FSR_VALIDATION=1` active les couches de validation du banc.
Les CSV `timings.csv` et `quality.csv` ainsi que les images PPM sont produits
par le banc. Données locales : `build/scaleforce-fast/performance/` et
`build/scaleforce-fast/validation/`.
