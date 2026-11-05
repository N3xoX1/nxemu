# Intel CMAA2 dans NXEmu

Intégration expérimentale Vulkan sur `fix/filter-scal`, 18 octobre 2026.
Le comparatif ultérieur des **quatre** options, avec AA et présentation séparés,
est décrit dans `tests/fsr/AA-COMPARISON.md`.
Le menu **AA** de la barre inférieure permet de changer Aucun / FXAA / SMAA /
CMAA2 pendant le jeu. Il utilise les mêmes surcharges de paramètres que les
filtres de mise à l'échelle. Le réglage avancé reste disponible.

## Source et port

Le HLSL Intel Apache-2.0 est conservé sans modification dans
`src/3rd_party/intel_cmaa2/`, licence incluse. Son empreinte de blob Git est
`717101c85ef6e933dee3d193f514410b51d09c45`.
Référence : https://github.com/GameTechDev/CMAA2 . Le module Unreal commercial
de Dmitry Karpukhin a été identifié sur Fab ; son code ne fait pas partie des
pièces fournies et ses optimisations internes ne sont pas revendiquées ici.

`prepare_intel_shader.py` produit une copie adaptée à Vulkan : formats R8UI / R32UI,
lectures HLSL compatibles avec DXC SPIR-V, vérifications des capacités de listes,
dimensions impaires et tuiles partielles, bornage aux images temporaires allouées.
Les lectures de couleur sont prolongées au bord pour éviter un contour noir sur
les petites images. Les calculs de formes et de pondération restent ceux d'Intel.

Le port résout les contributions dans une passe fragment RGBA16F qui préserve
l'alpha et copie implicitement les pixels sans contribution. Il évite donc une
copie plein écran séparée et l'écriture storage dans la texture du jeu.
Le preset Intel est Medium (1), avec luma calculée sur place et couleurs HDR
compactées en R11G11B10. Il ne nécessite pas FP16 natif.

Optimisations supplémentaires : suppression des écritures et de l'append atomique
dans la liste de positions devenue inutile ; remise à zéro des compteurs dans la
passe de préparation des arguments ; suppression du second dispatch d'arguments
et d'une barrière compute. Trois dispatchs compute restent, dont un indirect, puis une
passe graphique. Une barrière protège la réutilisation des ressources. Le changement
d'AA attend les commandes qui référencent l'ancienne passe avant sa destruction.

Les limites de dimensions, de buffers storage, de dispatch et les formats sont
contrôlés avant allocation. En cas d'indisponibilité, Vulkan journalise le problème
et utilise FXAA. OpenGL ne propose pas CMAA2 dans le menu du bas.

## Construction et vérifications

L'option reste explicite pour ne pas imposer DXC aux builds existants :

```text
cmake -S . -B build/windows-x64 -DNXEMU_ENABLE_INTEL_CMAA2=ON -DNXEMU_BUILD_FSR_BENCHMARK=ON
cmake --build build/windows-x64 --config Release --target nxemu nxemu-video nxemu-cmaa2-vulkan-benchmark
python tools/cmaa2/verify_port.py
python tools/cmaa2/test_contracts.py
python tools/cmaa2/validate_p0.py
ctest --test-dir build/windows-x64 -C Release --output-on-failure
```

DXC, glslangValidator et spirv-val sont nécessaires ; ils proviennent ici du SDK
Vulkan 1.4.357.0. La compilation vérifie les SPIR-V DXC avec spirv-val.
`validate_p0.py` vérifie aussi les étapes et les bindings des cinq shaders réels.
Le script de récupération n'est pas exécuté par CMake ni au lancement du jeu.

Banc GPU Windows, depuis PowerShell :

```powershell
$env:NXEMU_FSR_VALIDATION = '1'
$env:VK_LAYER_VALIDATE_SYNC = '1'
& build/windows-x64/src/yuzu_video_core/Release/nxemu-cmaa2-vulkan-benchmark.exe build/cmaa2-validation.csv
```

Supprimer ces deux variables pour chronométrer sans couche de validation.
Le banc emploie les shaders de production CMAA2 et FXAA, la même image source
et une destination RGBA16F, avec des buffers temporaires en mémoire GPU locale.
Il ne mesure ni la compilation des pipelines ni les allocations. Chaque cas
effectue 12 images de chauffe puis 60 mesures par mode (médiane et p95).
Le GPU reste libre entre soumissions : ce n'est pas une mesure en jeu à GPU saturé.

La matrice couvre 1×1, 3×5, 1279×719, 1920×1080 et 3840×2160, avec motif de
contours, couleur uniforme et bruit. Les tests CMAA2 vérifient valeurs finies,
alpha, conservation des aplats et stabilité sur 72 images identiques hors bruit.
Le champ `changed_rgb_channels` n'est pas un score de qualité ; zéro dans une
ligne FXAA signifie que cette métrique n'y est pas calculée.

## Limites de la validation

Les listes Intel restent dimensionnées à un quart des pixels pour les candidats
et un demi-pixel pour les contributions. Le bruit peut saturer ces listes : les
écritures restent bornées, mais des contributions sont perdues et la sélection
peut varier selon l'ordonnancement GPU. Les tests de stress n'exigent donc pas
l'identité entre images de bruit. Aucune supériorité universelle sur FXAA ou SMAA
n'est établie par ce banc.

La validation hors écran utilise une seule soumission à la fois sur une RTX 5070 Ti.
Elle n'établit pas l'identité pixel à pixel avec le resolve compute Intel original,
ni la qualité temporelle en jeu. Les entrées HDR et les listes ont des tests CPU,
mais le banc GPU utilise des sources RGBA8. Le changement à chaud via l'interface,
plusieurs frames en vol, les captures de jeux, les combinaisons FSR et les GPU
AMD/Intel restent à vérifier lors de la campagne de benchmark en jeu.

## Résultats locaux du 18 octobre 2026

Compilation Release de `nxemu`, `nxemu-video` et du banc GPU : réussie.
DXC, spirv-val, contrôle des bindings, transformation de la source épinglée et
4 000 tests CPU HDR : réussis. Les 11 tests CTest passent. Les tests qui écrivent
des fichiers temporaires doivent avoir un TEMP/TMP accessible en écriture dans
l'environnement d'exécution.

La matrice complète CMAA2/FXAA passe avec la couche Khronos et
`VK_LAYER_VALIDATE_SYNC=1`, sans erreur Vulkan signalée. Les sources GPU
contiennent notamment un alpha variable ; l'alpha CMAA2 est vérifié à 0,001 près
après conversion RGBA16F. Cette tolérance couvre l'arrondi du format de sortie.

Mesures **sans validation**, RTX 5070 Ti, médianes GPU en millisecondes :

| Source et contenu | CMAA2 | FXAA |
|---|---:|---:|
| 1279×719, contours | 0,0326 | 0,0178 |
| 1920×1080, contours | 0,0493 | 0,0357 |
| 3840×2160, contours | 0,1493 | 0,1630 |
| 1920×1080, aplat | 0,0454 | 0,0353 |
| 3840×2160, aplat | 0,1396 | 0,1443 |
| 1920×1080, bruit | 0,1076 | 0,0360 |
| 3840×2160, bruit | 0,4707 | 0,1900 |

Résultats bruts, y compris p95 et petits formats :
`tests/fsr/cmaa2-rtx5070ti-performance.csv` et
`tests/fsr/cmaa2-rtx5070ti-validation.csv`.
Ces scènes synthétiques n'établissent ni le classement en jeu ni un gain de
qualité mesuré. Le coût varie avec le nombre de contours et de contributions.
À 1080p, FXAA reste plus rapide dans ces cas ; à 4K, CMAA2 devance légèrement
FXAA sur le motif peu dense et l'aplat, et reste plus lent sur bruit dense.
