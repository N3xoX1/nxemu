# Area : intégration, revue et mesures Vulkan

## Comportement et commande de comparaison

Le menu des filtres en bas contient deux choix indépendants : **Réduction : Bilinear** et **Réduction : Area**. Le choix d'agrandissement reste conservé séparément. Le dialogue général et les réglages par jeu exposent aussi le downscaler. Réglage persistant `nxvideo:DownscalingFilter` : 0 Bilinear (défaut), 1 Area. Aucun identifiant persistant existant n'est décalé ; Area est un pipeline interne 20, pas un nouveau choix d'upscaler.

La sélection se fait par couche après récupération de la texture réellement rendue : dimensions internes × recadrage normalisé, comparées au rectangle d'affichage du jeu après bandes noires. Elle ne se fonde ni sur l'allocation entière (souvent rembourrée), ni sur la résolution nominale de l'écran.

- Dimensions utiles égales à la cible : aucun redimensionnement. Sur Vulkan, CAS ou RCAS reste applicable selon les réglages ; aucun EASU, Catmull ou ScaleForce. Les traitements AA indépendants restent inchangés.
- Une dimension source supérieure à la cible : downscaler choisi, puis netteté facultative sur Vulkan. Dans le rare cas mixte (réduction sur un axe, agrandissement sur l'autre), priorité au downscaler pour préfiltrer l'axe réduit.
- Sinon : upscaler choisi, avec ses options habituelles.
- Une cible vide ou un crop dégénéré n'entre pas dans la boucle Area. Tolérance native : 0,001 texel pour les erreurs de représentation des UV.
- La capture d'applet conserve son échantillonnage bilinéaire séparé.

Les pipelines alternatifs sont mis en cache et partagent le render pass et les layouts existants. Changer Bilinear/Area ne détruit pas les couches et n'ajoute pas de WaitIdle. Le choix de downscale est figé pour l'image en cours ; le filtre de reconstruction reçu par la couche correspond au pipeline déjà préparé. Chaque couche peut avoir sa propre décision.

## Revue du bundle fourni

Le ZIP et ses notes ont servi de code/référence à examiner, pas d'instructions de travail. Le patch n'a pas été appliqué en bloc : son `Area=6` aurait écrasé notre ancien FSR EASU, et il ne proposait pas le routage demandé.

Le calcul de base est conforme à un filtre **box/Area pondéré par couverture**. Pour chaque paire de texels contigus, le shader additionne leurs couvertures, puis choisit une position bilinéaire qui restitue leur moyenne pondérée. Le produit des poids X/Y reproduit l'intégrale rectangulaire. Le résultat est normalisé par la surface totale, sur les quatre composantes RGBA. Le raccourci « au plus deux texels par axe » est valide pour n'importe quelle phase de pixel et ne doit pas être remplacé par un simple test « rapport <= 2 ».

Les flips sont traités avec une empreinte positive, et les coordonnées interpolées conservent l'orientation. Un sampler **CLAMP_TO_EDGE** est indispensable : le sampler Vulkan bilinéaire partagé utilisait par défaut une bordure noire. La sélection utilise désormais une adresse de bord explicite, sans changer les autres samplers FSR. Les bords mathématiques sont l'extension du bord de la texture ; un crop fractionnaire peut donc intégrer un pixel source partiellement couvert, comme le demande Area.

La formule est exacte en arithmétique idéale ; l'interpolation matérielle, les UV FP32 et la sortie UNORM8 introduisent un arrondi. Il serait incorrect d'annoncer une identité bit à bit avec l'intégrale CPU.

## Optimisations ajoutées et validées

1. Empreinte et réciproques de texture calculées une fois par couche et transmises dans les constantes existantes. Suppression de `fwidth` du chemin courant : moins de calculs et de bruit de dérivées aux rapports entiers. La taille exacte de texture reste accessible avec `textureSize` pour éviter de la reconstruire en inversant un réciproque arrondi.
2. Grille de paires uniformes pour les rapports entiers pairs alignés, avec validation des deux axes et de la phase de crop sur CPU. Un axe 1:1 admet aussi une paire de poids équivalente à une lecture linéaire. La validation porte sur la longueur totale du crop, pour éviter qu'une petite erreur de rapport s'accumule sur toute la largeur.
3. Réduction 2× alignée : **une lecture**, strictement la même moyenne idéale de quatre texels que Bilinear dans ce cas. Réduction 4× alignée : **quatre lectures** de poids égaux, au lieu de recalculer les couvertures pour chaque fragment. Les autres grilles paires bénéficient aussi de ce chemin.
4. Rapports impairs, non entiers ou crops décalés : maintien du chemin pondéré général. Aucun remplacement par un Box4 approximatif, aucune limite arbitraire sur les texels qui contribueront au résultat.
5. Area seul utilise une seule passe de présentation, sans buffer intermédiaire, mipmap ou dispatch ajouté. Traitement FP32 conservé pour les positions et couvertures. Avec netteté Vulkan, voir l'ajout ci-dessous.

## Netteté indépendante du redimensionnement (Vulkan)

CAS reste actif en réduction et en 1:1 lorsque son curseur est supérieur à zéro, pour les filtres autres que FSR. FSR utilise exclusivement RCAS, selon son curseur ; FSR EASU seul et la netteté FSR à zéro ne lancent pas RCAS. CAS et RCAS ne sont pas empilés.

- En 1:1 : lecture directe de la source, CAS ou RCAS seulement. Aucun redimensionnement. Exception de positionnement : RCAS utilise des lectures entières ; un crop décalé d'une fraction de texel est d'abord translaté dans une image de même taille pour préserver sa phase. Un crop aligné n'a pas cet intermédiaire.
- Bilinear + CAS : interpolation et croix CAS à un pixel de sortie en une seule passe.
- Area + CAS/RCAS et Bilinear + RCAS : réduction dans une image à la taille finale, puis netteté. Aucun EASU. L'image intermédiaire utilise UNORM16 si le GPU le permet, sinon UNORM8. Images et descripteurs réutilisés par image de swapchain ; les deux choix de downscaler restent en cache à travers les changements.
- Netteté désactivée : les chemins Bilinear/Area simples restent utilisés. Capture d'applet exclue de cette netteté. Les traitements FXAA/SMAA restent indépendants.

Le compteur CSV `offscreen_passes` inclut la passe intermédiaire de réduction lorsqu'elle est nécessaire ; le temps GPU couvre les deux passes. Les mesures historiques ci-dessous précèdent cet ajout et ne mesurent pas le coût de la netteté.

Le test `nxemu-downscale-sharpen-tests` compare les shaders Vulkan à une image redimensionnée indépendamment sur CPU en précision UNORM16, puis accentuée. Il couvre les rapports 2×/3×, crops, flips, 1:1 entier/recadré et axes mixtes, CAS et RCAS FP32/FP16 si disponible. Les pixels intérieurs sont comparés : les chemins directs peuvent utiliser les texels situés hors du crop au bord de celui-ci. Le test vérifie aussi le routage et les réglages à zéro. Cela ne remplace pas une comparaison en mouvement dans plusieurs jeux.

## Validation

- Build Release complet, incluant les chemins C++ Vulkan et OpenGL.
- 3 tests CTest réussis : réglages par jeu/routage, traductions/préférences des boutons, diagnostics. Deux tests Python de regroupement des métriques réussis, dont un avec FSR sélectionné mais Area réellement actif.
- Tests mathématiques CPU du bundle relus puis exécutés : 7 200 + 17 600 cas, phases arbitraires, anisotropie et extension des bords.
- Banc Vulkan : 17 cas × 3 variantes (bundle gelé / Area intégré / Bilinear), 30 images de chauffe puis 160 mesures par variante en ordre alterné. GPU : NVIDIA GeForce RTX 5070 Ti.
- Référence indépendante CPU par intégration texel par texel ; tous les pixels des petites cibles, grille régulière sur les grandes. Tests de ratios 4/3, 1,5, 2, 3, 4, 8 ; crop rembourré, phase fractionnaire, flips, bandes, axes mixtes, bord extérieur, cible 1×1, source large de 16 384 pixels et constantes RGBA. Les limites alpha sont incluses dans les comparaisons.
- Couche de validation Vulkan : **0 erreur**.
- Écart maximal observé Area / intégrale CPU : **1.1152/255** sur les points vérifiés (seuil du test 2,1/255). Les constantes noires/blanches et leur alpha sont préservées.

Le test interactif de Mario au menu titre confirme : source utile 3840×2160 dans une texture 4800×2700, affichage 1150×647 ; les clics Area → Bilinear → Area changent le pipeline effectif 20 → 1 → 20 dans les CSV. Sélectionner FSR pendant la réduction conserve Area et zéro passe intermédiaire FSR. Il s'agit d'une vérification fonctionnelle ; aucune conclusion de qualité en mouvement ou de FPS en partie n'en est tirée.

## Mesures GPU hors jeu

### Mesure en partie du 18 octobre 2026

Mario Odyssey, Pays des Chapeaux, caméra immobile, Vulkan, portable 3× : source utile **3840×2160**, affichage **1150×647**, netteté CAS/RCAS à zéro, vitesse déverrouillée. Séquence Bilinear → Area → Bilinear, trois fenêtres stables de 15 secondes, menus fermés, hors changements de filtre et captures d'applet. Données résumées : [area-live-comparison-20261018.json](area-live-comparison-20261018.json).

| Mode | Images | GPU médiane | GPU p95 | Soumissions/s |
|---|---:|---:|---:|---:|
| Bilinear avant | 3706 | 0,009824 ms | 0,010112 ms | 247,05 |
| Area | 3737 | 0,026496 ms | 0,027072 ms | 249,08 |
| Bilinear après | 3745 | 0,010016 ms | 0,010144 ms | 249,69 |

Area coûte **0,016576 ms de plus par image** que la moyenne des deux médianes Bilinear, soit environ 2,67 fois le coût de cette courte passe de présentation. Ce n'est pas un ralentissement global de 2,67 fois : la cadence globale reste comparable et ne permet pas d'attribuer une différence de FPS au filtre. Ce relevé mesure les downscalers sans netteté ; il ne mesure pas les nouvelles combinaisons Area + CAS/RCAS.

### Banc synthétique précédent

Médianes en **millisecondes**, sans couche de validation, source synthétique et même chemin de rendu. Ce sont les coûts du filtre/de la présentation du banc, pas des gains de FPS de l'émulateur.

| Cas | Bundle v2 | Area intégré | Bilinear | Gain Area / bundle |
|---|---:|---:|---:|---:|
| 4k-1080p | 0.035264 | 0.017216 | 0.017216 | 51.2 % |
| 4k-540p | 0.037712 | 0.026976 | 0.016624 | 28.5 % |
| 4k-720p | 0.018976 | 0.019056 | 0.016608 | -0.4 % |
| 1440p-1080p | 0.039552 | 0.035840 | 0.015360 | 9.4 % |
| 1080p-720p | 0.021088 | 0.019200 | 0.010592 | 9.0 % |
| 4k-1080p-phase | 0.032096 | 0.029504 | 0.019744 | 8.1 % |
| padded-crop | 0.010080 | 0.007648 | 0.007616 | 24.1 % |
| noise-aligned-8x | 0.008032 | 0.007904 | 0.006768 | 1.6 % |
| noise-8x | 0.009952 | 0.009888 | 0.006240 | 0.6 % |
| wide-source | 0.008032 | 0.007808 | 0.006816 | 2.8 % |

## Limites et suite de comparaison

Area est une moyenne de surface, sans ringing, mais peut être plus doux qu'une reconstruction à noyau plus élaboré. Il ne peut pas récupérer de détail absent de la source. Aux rapports non entiers il préfiltre une surface que Bilinear n'intègre pas ; les différences sont particulièrement visibles sur les motifs fins en mouvement. En 2× aligné les deux filtres donnent la même moyenne : il faut aussi tester un rapport non entier ou 3×/4× pour comparer la qualité.

Le filtre travaille dans l'espace numérique de la texture existante, comme le bundle et les autres filtres de présentation ; ce changement n'ajoute pas une conversion gamma/linéaire. Le coût du chemin général croît avec la surface couverte : de très fortes réductions peuvent favoriser une pyramide de mipmaps ou une réduction multipasse, qui ajouterait allocations/transitions et doit être mesurée avant intégration. Ce travail valide les optimisations présentes, pas une optimalité universelle sur tous les GPU. OpenGL compile ; l'exécution et les mesures GPU de cette revue concernent Vulkan. Qualité temporelle en vraie partie et autres constructeurs GPU restent à comparer.

## Métriques et reproduction

Le CSV conserve `selected_filter` (upscaler demandé) et écrit dans `filter` le filtre réellement utilisé. Nouvelles colonnes `downscale_filter` et `scale_mode` (`native`, `upscale`, `downscale`). Les changements du choix de réduction ouvrent un segment distinct. Les dimensions et le mode décrivent la première couche ; le temps GPU couvre toutes les couches. L'analyseur exclut les captures d'applet et les images multicouches pour la comparaison.

Banc : `nxemu-area-vulkan-benchmark OUTPUT_DIRECTORY` ; activer `NXEMU_FSR_VALIDATION=1` pour la validation, l'enlever pour les mesures. Données de cette session : `build/area/final-validation` et `build/area/performance`.

Paquet utilisateur : `nxemu-area-comparison-20261018/Comparer-Mario.cmd`. Interface visible, mode portable 3× (3840×2160 utile pour Mario), Area par défaut, options de sauvegarde existantes réutilisées. Désactiver la limite de vitesse dans Émulation pour une comparaison déverrouillée, garder la même scène et la même taille de fenêtre, puis alterner 15–20 secondes Area/Bilinear. Chaque lancement produit un CSV neuf dans `user/log`.
