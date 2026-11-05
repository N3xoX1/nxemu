# FSR Vulkan : résultats du 17 octobre 2026

Source : `8dfbc0a2b146d6094180ebebd1ec31ad79106261` avec patch FSR adapté et corrections de revue, build Release MSVC x64. GPU : NVIDIA GeForce RTX 5070 Ti, pilote 616.92.

Trois lancements sans couche de validation, chacun avec 60 tours de chauffe et 240 paires A/B par cas et précision. Les nombres ci-dessous sont les médianes des trois médianes. Ils mesurent la chaîne de présentation FSR, pas une hausse de FPS du jeu.

| Cas | Précision | Ancien (ms) | Nouveau (ms) | Réduction | Écart RGB maximal |
|---|---|---:|---:|---:|---:|
| 720p-1080p | fp32 | 0.0868 | 0.0682 | 21.4 % | 1/255 |
| 1080p-4k | fp32 | 0.3180 | 0.2498 | 21.4 % | 1/255 |
| native-1080p | fp32 | 0.0868 | 0.0203 | 76.6 % | 95/255 |
| native-flip | fp32 | 0.0869 | 0.0203 | 76.7 % | 95/255 |
| native-easu-only | fp32 | 0.0634 | 0.0155 | 75.6 % | 80/255 |
| easu-only | fp32 | 0.0639 | 0.0522 | 18.3 % | 1/255 |
| easu-crop-offset | fp32 | 0.0639 | 0.0531 | 17.0 % | 1/255 |
| easu-flip | fp32 | 0.0641 | 0.0534 | 16.6 % | 1/255 |
| rcas-flip | fp32 | 0.0868 | 0.0681 | 21.6 % | 1/255 |
| rcas-crop-offset | fp32 | 0.0869 | 0.0682 | 21.6 % | 1/255 |
| 720p-1080p | fp16 | 0.0840 | 0.0641 | 23.7 % | 0/255 |
| 1080p-4k | fp16 | 0.3172 | 0.2462 | 22.4 % | 0/255 |
| native-1080p | fp16 | 0.0841 | 0.0195 | 76.8 % | 94/255 |
| native-flip | fp16 | 0.0841 | 0.0195 | 76.8 % | 94/255 |
| native-easu-only | fp16 | 0.0606 | 0.0154 | 74.5 % | 80/255 |
| easu-only | fp16 | 0.0611 | 0.0472 | 22.7 % | 0/255 |
| easu-crop-offset | fp16 | 0.0612 | 0.0472 | 22.8 % | 0/255 |
| easu-flip | fp16 | 0.0613 | 0.0473 | 22.8 % | 0/255 |
| rcas-flip | fp16 | 0.0842 | 0.0644 | 23.5 % | 0/255 |
| rcas-crop-offset | fp16 | 0.0840 | 0.0642 | 23.6 % | 0/255 |

En agrandissement, le RGB FP16 est identique sur cette mire ; l'écart FP32 est au plus 1/255. L'alpha diffère au plus de 1/255 dans les cas EASU fusionnés, en raison de la suppression de l'arrondi de la texture intermédiaire FP16. Les cas avec RCAS conservent l'alpha exactement sur cette mire.

En natif, l'image change volontairement car EASU est supprimé. Sur la mire 1080p, le PSNR RGB par rapport à la source passe de 46,05 à 47,54 dB en FP16, et de 46,00 à 47,62 dB en FP32. Sans netteté en natif, B reproduit exactement le RGB de la source ; A conservait une erreur liée au filtrage. Ce résultat synthétique ne prouve pas une préférence visuelle universelle.

Validation : 20 cas Vulkan (10 scénarios × FP16/FP32), zéro erreur de validation ; assertions d'image réussies. Build complet nxemu réussi ; trois tests CTest réussis : diagnostics FSR, réglages par jeu et traductions/préférences. Le mode A/B conserve les corrections communes des bords RCAS et de l'alpha : la comparaison isole l'architecture des passes.

Les CSV bruts et images sont disponibles localement sous `build/fsr-validation/vulkan-final-run1`, `run2`, `run3` (chaque dossier porte le préfixe `vulkan-final-`), et la validation sous `build/fsr-validation/vulkan-final-validation`. Le résumé agrégé est `build/fsr-validation/vulkan-three-runs.json`.

Mario Odyssey est lancé pour un essai A/B réel avec enregistrement automatique. Les résultats hors jeu ne permettent pas encore de conclure sur ses FPS ni sur la qualité d'une scène 4K : il faut sélectionner des phases comparables, confirmer les dimensions dans le CSV, et tenir compte de l'avis visuel de l'utilisateur.

À 4K, supprimer une texture intermédiaire RGBA16F évite 63,3 Mio par image de swapchain et par couche FSR nécessitant EASU. Le gain dépend donc aussi du nombre d'images en vol. Le raccourci natif n'alloue aucune des deux textures FSR pour une couche qui reste native.
