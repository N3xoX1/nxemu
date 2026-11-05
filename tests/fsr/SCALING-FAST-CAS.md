# Catmull rapide et CAS optimisé — 18 octobre 2026

## Changements livrés

- Catmull-Rom classique (9 lectures) reste disponible. Nouveau choix explicite
  « Catmull-Rom rapide » (5 lectures normalisées), avec et sans CAS.
- Vulkan : CAS partage les deux voisins déjà reconstruits dans chaque groupe
  de quatre fragments. Trois lectures par pixel au lieu de cinq, sans changer
  la formule de netteté. Repli sur le shader à cinq lectures si les opérations
  de sous-groupe BASIC/QUAD ne sont pas disponibles au stade fragment.
- Catmull + CAS : au-delà de 1 048 576 pixels de sortie, une passe Catmull écrit
  une image RGBA16 UNORM réutilisable par CAS, jusqu’à CAS 66 %. Les calculs restent FP32. Un halo
  d'un pixel de sortie conserve les voisins de la reconstruction fusionnée,
  y compris aux bords des crops et avec les flips. La voie native 1:1 évite
  cette passe ; les petits rendus et CAS au-delà de 66 % restent fusionnés.
  Le format intermédiaire est vérifié sur le GPU ; la voie fusionnée est
  conservée s’il n’est pas pris en charge. Le seuil est une
  heuristique conservatrice vérifiée sur cette RTX, pas un optimum universel.
- Images et descripteurs intermédiaires réutilisés par image de swapchain ;
  aucune attente GPU ajoutée à chaque image. Le halo doit tenir dans les
  limites d'image du GPU, sinon la voie fusionnée est conservée.
- L'alpha est reconstruit, puis reste inchangé par CAS. CAS=0 conserve le
  raccourci sans shader CAS. FSR A/B ne reçoit jamais CAS.
- OpenGL conserve le chemin fusionné et reçoit le choix Catmull rapide.
  Les gains CAS ci-dessous concernent Vulkan.

## Mesures RTX 5070 Ti, hors jeu

Médianes GPU, 30 tours de chauffe et 160 mesures alternées par pipeline.
Les mesures incluent les deux passes et leurs barrières lorsque utilisées.

| Filtre | Avant → après en 1080p | Avant → après en 4K |
|---|---:|---:|
| Catmull 9 → Catmull rapide | 0,036736 → 0,026304 ms (−28,4 %) | 0,124064 → 0,083296 ms (−32,9 %) |
| Bilinéaire + CAS 50 % | 0,020960 → 0,018560 ms (−11,5 %) | 0,061456 → 0,051984 ms (−15,4 %) |
| Catmull classique + CAS 50 % | 0,132000 → 0,058912 ms (−55,4 %) | 0,497392 → 0,199328 ms (−59,9 %) |
| Catmull rapide + CAS 50 %, fusionné → deux passes + partage | 0,080960 → 0,048288 ms (−40,4 %) | 0,294432 → 0,184960 ms (−37,2 %) |

En 4K, Catmull rapide + CAS final coûte donc 62,8 % de moins que l'ancien
Catmull classique + CAS fusionné. Ces pourcentages portent sur le filtre,
pas sur les FPS globaux du jeu. Le tampon RGBA16 UNORM avec halo consomme environ
66,4 Mo par image en 4K, soit environ 199 Mo pour trois images de swapchain.
Il est alloué uniquement si ce chemin Catmull + CAS est effectivement utilisé.

## Fidélité et validation

La simplification à cinq lectures est volontairement une approximation des
coins diagonaux. Écart maximal sur la mire : 3/255 sans CAS ; jusqu'à 8/255
pour le chemin final rapide + CAS comparé à l'ancien Catmull exact + CAS.
La validation visuelle en jeu reste nécessaire, notamment sur les détails
fins en mouvement. Le choix classique permet la comparaison dans le même menu.

Le CAS partagé seul produit les mêmes pixels RGB/alpha sur les douze cas à
50 %. Les balayages de force 0, 1, 25, 66, 67, 75 et 100 % passent avec une
tolérance RGB de 1/255 et un alpha strictement identique. Bilinéaire et nearest
sont aussi vérifiés avec leur bord noir réel.

Le tampon UNORM16 conserve plus de précision SDR que le premier prototype en
demi-float à taille égale. Il est lu directement par texel, sans interpolation
supplémentaire. La recomposition en deux passes introduit encore de petits
écarts d'arrondi : au plus 3/255 sur les cas testés jusqu'à CAS 66 %, alpha
au plus 1/255, erreur moyenne maximale inférieure à 0,004/255. Au-delà de 66 %,
les différences de reconstruction peuvent être amplifiées par la netteté :
le chemin final reste donc fusionné et conserve l'image du shader de référence
(avec les gains du partage des voisins). Il ne s'agit pas d'une équivalence
bit à bit des deux passes. Cette règle est vérifiée jusqu'à CAS 100 %.

- 12 cas GPU : upscale, downscale, petits rendus, natif, flips, crop décalé,
  noir et blanc, avec alpha variable. Zéro erreur de validation Vulkan.
- Réglages, traductions, métriques C++ : 3/3 tests passent.
- Analyse CSV Python : test des anciens et nouveaux identifiants passe.
- Mario : démarrage et passage à chaud rapide+CAS → classique+CAS →
  bilinéaire+CAS vérifiés sur l'écran titre, interface visible. CSV :
  `user/log/smoke-fast-cas.csv`. Le binaire final est aussi vérifié dans
  `user/log/smoke-unorm.csv` : passage CAS 72 % (fusionné, zéro passe
  intermédiaire) vers CAS 50 % (une passe intermédiaire), à 1705×959. Ce contrôle n'est pas
  une comparaison de performances en scène fixe.

Les règles de partage par quads et de participation des fragments auxiliaires
sont décrites dans la [spécification Khronos](https://docs.vulkan.org/glslext/latest/glslext/khr/GL_KHR_shader_subgroup.html).

## Comparer en jeu

Lancer `Comparer-Mario.cmd`. Réglage initial : Portable 1×, Catmull rapide,
CAS 0 %, interface visible. Désactiver « Émulation → Limiter la vitesse » pour
un essai sans limite de vitesse. Attendre la chauffe des shaders, puis garder
la même scène et la même caméra. Comparer Catmull classique et rapide à CAS=0,
puis aux mêmes forces CAS. Bilinéaire et FSR A/B restent dans ce menu.
Les CSV automatiques se trouvent dans `user/log/fsr-*.csv`. Les premières
3 secondes et la dernière seconde de chaque phase sont écartées à l'analyse.
Le paquet précédent reste disponible pour comparer l'ancien CAS.

Données : `build/scaling-04/release-performance/` et
`build/scaling-04/release-validation/`. Le banc accepte des shaders candidats
`catmull.spv`, `cas.spv`, `cas_catmull.spv`. Pour reproduire les mesures, compiler
respectivement `present_catmull_rom.frag`, `vulkan_present_cas_quad.frag` et
`vulkan_present_cas_catmull_quad.frag` via `glslangValidator -V --target-env
spirv1.3`, puis fournir leur dossier en deuxième argument du banc. Le benchmark
ajoute ces trois variantes aux voies de référence et utilise le candidat CAS
Catmull pour la deuxième passe des essais à deux passes.
