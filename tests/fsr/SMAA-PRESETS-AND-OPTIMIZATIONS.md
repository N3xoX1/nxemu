# SMAA : presets, optimisations et intégration High/Ultra

18 octobre 2026 — branche `fix/filter-scal`, RTX 5070 Ti, pilote 616.92.

**High est le compromis retenu comme recommandation.** High et Ultra sont
maintenant deux choix distincts dans les réglages globaux, par jeu et dans la
barre inférieure, avec changement à chaud. L'ancienne valeur numérique 2 et le
nom sauvegardé `Smaa` restent **Ultra**. High est ajouté sans déplacer les
identifiants existants ; le réglage AA par défaut reste Aucun.

## Comparaison des presets avant optimisation

Les trois presets ont d'abord été mesurés avec les mêmes formats et passes :
contours RG16F, poids RGBA16F, sortie RGBA16F. Ils proviennent du même shader
de référence, avec uniquement le define du preset remplacé dans les copies du
banc. Les sources de production n'ont pas été changées pour cette comparaison.

La méthode est celle de `AA-COMPARISON.md` : huit cas synthétiques, trois séries,
24 images de chauffe puis 96 mesures par mode et série, ordre tournant des
quatre AA, GPU timestamps séparant AA et présentation, chronométrage sans
validation. **Ce ne sont pas des mesures FPS dans un jeu.**

Temps médians du **SMAA seul**, en ms :

| Preset | 720p contours | 1080p contours | 4K contours | 4K bruit |
|---|---:|---:|---:|---:|
| Ultra | 0,0605 | 0,1021 | 0,3599 | 0,8574 |
| High | 0,0526 | 0,0884 | 0,2843 | 0,7045 |
| Medium | 0,0448 | 0,0725 | 0,2355 | 0,5160 |

Sur le motif de contours, High réduit le coût d'environ 13 % en 1080p et
21 % en 4K face à Ultra. Medium réduit le coût de 29 % et 35 %.

La [référence SMAA](https://github.com/iryoku/smaa/blob/master/SMAA.hlsl)
définit Ultra avec un seuil de 0,05 et des recherches de 32 pas horizontaux/
verticaux et 16 diagonaux. High utilise 0,1, 16 et 8, et conserve le traitement
des coins et diagonales. Medium utilise 0,1 et 8 pas, mais désactive le
traitement spécifique des coins et diagonales. Les différences de rendu ne
sont donc pas seulement une question de distance de recherche.

Les captures High/Ultra sont proches sur ces motifs, mais pas identiques.
Medium change davantage les contours. Les différences RGB globales conservées
dans les CSV ne sont pas des scores de qualité ni une référence de vérité.
Une erreur localisée sur une diagonale peut disparaître dans une moyenne
calculée sur toute l'image.

## Optimisations testées après les presets

1. **Contours RG8 + renderpasses sans rechargement**, avec effacement des
   contours à chaque image. En 4K, High passe de 0,2843 à 0,2812 ms pour l'AA
   seul, soit seulement environ 1 %. Medium régresse légèrement. Quelques
   différences locales apparaissent en High/Ultra : cette variante n'est pas
   retenue en production. Medium est identique sur les cinq captures comparées.
2. **Poids RGBA8 en plus des contours RG8**, testé séparément en Ultra : coût
   de 0,3364 ms contre 0,3599 ms en 4K, environ 6,5 % de mieux. La précision
   des poids change et les captures diffèrent ; cette variante reste un test.
3. **Écriture explicite de zéro sans contour**, à précision et preset inchangés,
   puis DONT_CARE sur les attachments entièrement réécrits. Le gain de temps est
   négligeable, mais cette variante corrige un défaut de réutilisation démontré
   ci-dessous. Elle est intégrée pour High et Ultra.
4. **Resolve SMAA fusionné avec la sortie native 1:1**, en conservant contours
   RG16F et poids RGBA16F. La passe finale écrit directement en RGBA8, évitant
   une image RGBA16F et le draw de présentation suivant. Ce prototype est
   uniquement dans le banc ; il ne s'applique ni aux crops ni à l'upscale,
   downscale, CAS ou FSR.

Pour le prototype fusionné, comparer le **total AA + présentation**, car la
présentation est alors incluse dans le resolve :

| Cas natif | High classique | High fusionné | Gain sur le total |
|---|---:|---:|---:|
| 720p | 0,0629 ms | 0,0508 ms | 19 % |
| 1080p | 0,1034 ms | 0,0841 ms | 19 % |
| 4K | 0,3283 ms | 0,2660 ms | 19 % |

Le total Ultra classique en 4K est 0,4041 ms : le prototype High fusionné
coûte environ 34 % de moins. Medium fusionné atteint 0,2172 ms en 4K, mais
avec le compromis de qualité du preset Medium.

Sur les cinq captures comparées pour chacun des deux presets, l'écriture
explicite de zéro produit exactement les mêmes pixels que la référence
statique. Le prototype fusionné diffère d'au plus **1 niveau RGB sur 255** sur
les captures natives, car il supprime l'arrondi intermédiaire FP16. Les cas
upscale/downscale utilisent le chemin ordinaire et restent identiques. Cela
ne prouve pas l'absence d'erreur sur toute autre image ou format de sortie.

## Défaut de contours périmés, reproduit puis corrigé

Le shader de détection couleur utilisait `discard` quand aucun contour n'était
détecté. Vulkan rechargeait pourtant l'ancien attachment avec LOAD, et ne
l'effaçait qu'à l'initialisation. Ainsi, les contours d'une image pouvaient
rester dans la suivante. OpenGL effaçait déjà l'attachment chaque frame.

Le test affiche une image A, puis une image B décalée, et compare le résultat
de B avec celui obtenu après remise à zéro des contours :

- Ultra d'origine : **26 763 composantes RGB différentes**, écart maximal 84/255.
- High d'origine : **26 831 composantes RGB différentes**, écart maximal 84/255.
- Après écriture explicite de zéro : **zéro différence**, High et Ultra.

La correction de production conserve les calculs et la précision RG16F/RGBA16F.
Les attachments Vulkan entièrement réécrits ne sont plus rechargés. Le banc
échoue maintenant si le résultat dépend des anciens contours. Les deux presets
de production, incluant leurs vrais SPIR-V High, passent cette régression et la
matrice complète sous validation Vulkan/synchronisation.

## Implémentations optimisées recherchées

- [SMAA de référence](https://github.com/iryoku/smaa/blob/master/SMAA.hlsl) :
  recommande le masquage stencil de la passe de calcul des poids. Notre SMAA
  actuel ne l'utilise pas ; c'est une piste distincte des presets.
- [Godot, SMAA Vulkan](https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_rd/effects/smaa.cpp) :
  exemple public utilisant stencil et textures RG8/RGBA8. Aucun gain de vitesse
  de Godot n'a été transposé ou mesuré dans NXEmu pendant ces essais.
- [GShade SMAA](https://github.com/Mortalitas/GShade/blob/master/Shaders/SMAA.fx) :
  utilise également RG8/RGBA8 et annonce des optimisations légères. Nos essais
  de formats ci-dessus montrent qu'il faut mesurer le gain sur le GPU ciblé.
- [iMMERSE Anti Aliasing](https://github.com/martymcmodding/iMMERSE#immerse-anti-aliasing) :
  l'auteur annonce environ 15 % via micro-optimisations et jusqu'à un facteur
  deux sur les plateformes compute, notamment par réordonnancement des threads.
  Ce sont **ses annonces, pas nos mesures**. Sa
  [licence](https://github.com/martymcmodding/iMMERSE/blob/main/LICENSE) exige une
  permission pour réutiliser des parties du code dans un autre projet : aucun
  code de cette implémentation n'a été intégré.

## État livré et vérifications

- High et Ultra accessibles dans l'interface ; Medium reste dans le banc.
- Les anciens réglages restent Ultra et la sérialisation High est distincte.
- CTest couvre les bornes des choix par jeu, les anciens noms sauvegardés,
  le roundtrip High, et le changement High/Ultra à chaud dans le vrai module
  de réglages vidéo.
- Build Release de l'application et des backends ; 11 tests CTest passent.
- Matrices GPU High et Ultra de production : validation Vulkan/synchronisation,
  captures, aplats et transition A → B réussis.
- Les 12 étapes de shaders OpenGL High/Ultra compilent en sémantique OpenGL.
  Ce contrôle ne remplace pas une exécution OpenGL en jeu.
- La fusion 1:1, les formats compacts et les presets Medium ne sont pas activés
  dans l'application. Le preset de chaque configuration existante est préservé.

## Reproduire les essais

```powershell
cmake --build build/windows-x64 --config Release --target nxemu-aa-vulkan-benchmark
# Sans argument de variante : Ultra. High et Medium sont explicites.
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/ultra
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/high high
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/medium medium
# Puis les prototypes :
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/high-compact high-compact
& build/windows-x64/src/yuzu_video_core/Release/nxemu-aa-vulkan-benchmark.exe build/high-fused high-fused
python tests/fsr/compare_smaa.py build/high build/high-fused build/high-diff.csv --max-error 1
```

Le banc courant inclut désormais la correction d'écriture des contours nuls ;
les CSV `ultra`, `high`, `medium` conservés ici sont la campagne de presets
effectuée **avant** cette correction, conformément à la séparation demandée.
Les variantes `*-zero` isolent son effet, quasi nul sur les chronométrages.
Activer les variables de validation décrites dans `AA-COMPARISON.md` pour les
contrôles Vulkan, puis les retirer pour mesurer les performances.

Données dans `smaa-results-rtx5070ti-20261018/` : les douze variantes testées,
mesures SMAA seules filtrées des CSV complets du banc, contrôles RGB,
dépendance à l'historique avant/après, différences entre captures et logs de
validation des prototypes fusionnés. Les captures PPM sont dans `build/`.

Pour CMAA2, les résultats restent ceux de `AA-COMPARISON.md` : son coût se
rapproche de FXAA sur le motif testé et augmente avec le bruit. La validation
technique obtenue ne prouve ni son identité pixel à pixel avec le resolve Intel
original ni sa qualité temporelle sur des jeux en mouvement.
