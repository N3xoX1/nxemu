# Catmull-Rom / CAS : exploration du 18 octobre 2026

Le paquet de comparaison du 18 octobre reste inchangé. Les variantes suivantes
ont été testées hors jeu sur la RTX 5070 Ti, sous Vulkan, en alternant les
pipelines (30 tours de chauffe, 160 mesures par pipeline et par cas).
Les temps sont des médianes du passage GPU, pas des gains de FPS du jeu.

| Variante | 720p → 1080p | 720p → 4K | Texture 1600×900 → 1652×929 |
|---|---:|---:|---:|
| Catmull exact, 9 lectures | 0,036672 ms | 0,124576 ms | 0,029280 ms |
| Catmull approximé, 5 lectures | 0,026304 ms | 0,083744 ms | 0,021408 ms |
| Réduction | 28,3 % | 32,8 % | 26,9 % |
| Catmull exact + CAS 50 % | 0,130976 ms | 0,495072 ms | 0,099680 ms |
| Catmull approximé + CAS 50 % | 0,080000 ms | 0,291840 ms | 0,061216 ms |
| Réduction | 38,9 % | 41,1 % | 38,6 % |

## Fidélité

La variante 5 lectures supprime les quatre produits des coins, puis normalise
le noyau restant pour conserver les couleurs constantes. Elle garde les lobes
négatifs sur les axes, mais modifie la réponse diagonale.

Sur cette mire seulement : erreur RGB maximale de 3/255 sans CAS, et jusqu'à
8/255 avec CAS à 50 %. L'erreur moyenne reste inférieure à 0,09/255 sur les dix
cas (montée et baisse de résolution, natif, flips, crop, noir, blanc). Cela ne
prouve pas l'absence d'artefacts sur d'autres images ni en mouvement. Une
comparaison visuelle en jeu reste nécessaire avant d'en faire un mode optionnel.
La voie native 1:1 conserve le contournement exact. Alpha : écart maximal 1/255.

Le FP16 mixte (coordonnées FP32, couleurs FP16) apporte moins de 1 % sur Catmull,
ne réduit pas le coût de CAS bilinéaire, et fait échouer le seuil de fidélité
avec Catmull+CAS (4/255 dans le cas real-window). Variante écartée.
Le CAS FP32 avec extrema et amplitude limités au canal vert produit exactement
les mêmes pixels que la version actuelle, mais ses temps restent équivalents :
0,020992 ms dans les deux cas en 1080p, 0,062240 contre 0,062400 ms en 4K.
La simplification ne justifie pas une mise à jour de production.

## Piste CAS avec Catmull

La version fusionnée actuelle reconstruit cinq pixels Catmull, soit jusqu'à
45 lectures. Réutiliser une image Catmull intermédiaire ramènerait les lectures
à 9 pour Catmull puis 5 pour CAS, mais ajouterait une passe et un tampon. Cette
piste conserve potentiellement mieux la reconstruction que l'approximation à
5 lectures ; elle reste à implémenter et mesurer, en particulier aux bords du
crop, avec les flips et l'alpha. Aucun gain de cette piste n'est revendiqué ici.
CAS bilinéaire n'effectue déjà que cinq lectures.

## Reproduction du prototype 5 lectures

Le fichier `experimental_catmull5.glsl` reste volontairement hors des shaders
livrés. Copier les cinq sources `catmull_rom.glsl`, `present_catmull_rom.frag`,
`opengl_present_cas.frag`, `vulkan_present_cas.frag` et
`vulkan_present_cas_catmull.frag` dans un dossier de test, puis remplacer sa
copie de `catmull_rom.glsl` par `experimental_catmull5.glsl`.
Compiler les trois fragments Vulkan via `glslangValidator -V --target-env
spirv1.3`, en nommant les sorties respectivement `catmull.spv`, `cas.spv`,
`cas_catmull.spv`. Le banc accepte ensuite :

```
nxemu-scaling-vulkan-benchmark OUTPUT_DIRECTORY CANDIDATE_SPIRV_DIRECTORY --report-only
```

Le mode `--report-only` désactive uniquement le rejet des différences de la
variante expérimentale ; les validations du filtre exact restent actives.
Sans cet argument, une variante dépassant les seuils d'image fait échouer le
banc. Les erreurs sont écrites dans `candidate-errors.csv` et les mesures dans
`timings.csv`.

Données locales : `build/scaling-03/five-tap-run1/` et
`build/scaling-03/five-tap-run1.log`. Premier essai FP16 :
`build/scaling-03/mixed16-run1.log` (arrêt volontaire au premier seuil dépassé).
