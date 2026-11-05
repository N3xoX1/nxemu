# Reconstruction normalisée : règle structurelle Vulkan

La correction sélectionnée par hash de shader a été retirée. Le compilateur reconnaît maintenant une famille de calculs de reconstruction, indépendamment du jeu, du code binaire précis et de l'emplacement des constantes. Il ne contient pas de liste de jeux/shaders autorisés ni de banque/offset de constante prédéfini pour cette correction.

## Règle reconnue

Le détecteur suit les expressions SSA à partir de `Position.X/Y`, à travers les conversions et troncatures, jusqu'aux coordonnées des lectures normalisées. Il recherche une quantification horizontale en demi-colonnes avec une parité de colonne utilisée comme prédicat de branche, des pas UV horizontaux et verticaux, plusieurs images 2D et une sortie auxiliaire. Les coefficients UV par pixel sont retrouvés dans les produits/FMA avec ces coordonnées ; leurs banques et offsets sont déduits du shader.

La reconstruction passe sur la grille physique et ses coefficients UV sont divisés par le facteur de résolution. Le facteur et chaque coefficient adapté sont partagés entre leurs utilisations, dans le bloc d'entrée. Les origines et bornes déjà normalisées restent celles du programme : le détecteur ne suppose pas qu'un emplacement arbitraire de constante encode une marge d'un demi-texel.

Une condition uniforme contrôle les indicateurs `IsTextureScaled` de **toutes** les images utilisées par cette reconstruction. Si une entrée n'est pas rescalée, les coordonnées et les coefficients reprennent ensemble le fonctionnement générique précédent. Cette décision ne demande ni lecture CPU/GPU ni interrogation du pilote.

## Limites volontaires

La règle couvre cette famille de reconstructions, pas toutes les utilisations de coordonnées entières. Elle laisse le chemin existant lorsque la structure est ambiguë/non reconnue : entrées de matériau, export de profondeur, discard, écritures de mémoire, accès d'image autres que l'échantillonnage normalisé implicite 2D, parité servant de couleur/poids, coefficient utilisé aussi hors des calculs de coordonnées, sortie auxiliaire absente ou reconstruction à une seule texture. L'analyse des dépendances a une profondeur bornée et se replie lorsqu'elle ne peut pas conclure.

L'activation est actuellement limitée à Vulkan, pour un facteur interne supérieur à 1×. Le 1×, le sous-échantillonnage et OpenGL gardent leur chemin existant. `NXEMU_HALF_RATE_RECONSTRUCTION=0` désactive la règle pour l'A/B, sans dépendre d'une identité de jeu.

Cette reconnaissance repose sur une forme de calcul et des conditions conservatrices. Elle ne démontre pas l'intention de tous les effets graphiques possibles. Les tests sur d'autres jeux restent nécessaires avant de revendiquer une compatibilité générale ; une reconstruction écrite différemment peut simplement ne pas être reconnue.

## Vérifications automatiques

Les deux tests CTest `nxemu_reconstruction` et `nxemu_reconstruction_disabled` passent. Ils exécutent le vrai rescaler, la vérification SSA/types/ordre et un évaluateur de coordonnées avec une référence indépendante sur toute la largeur utile.

- Deux écritures algébriques des centres de paires.
- Trois dispositions de constantes : banques 3, 7 et 1, offsets de base 0, 128 et 64 ; mêmes coefficients retrouvés sans identité de shader.
- Facteurs 0,5×, 1×, 2×, 3×, 4×, 5× et 6×, ainsi que des render targets natives avec un facteur global supérieur à 1×.
- Repli lorsqu'une des deux textures reste native ; backend OpenGL non modifié.
- Six familles de cas négatifs : parité couleur, coefficient couleur, entrée unique, sortie unique, attribut de matériau et prédicat de parité réutilisé comme couleur.

## Audit sur les shaders réellement chargés

Dans la version finale, avec le contrôle des textures rescalées, chargée pour Mario Odyssey portable 3× : **335 shaders fragment examinés, deux reconnus**, automatiquement. Il s'agit des deux phases de reconstruction précédemment observées ; leurs identifiants ne participent plus à la décision.

Les **333 autres SPIR-V sont identiques octet par octet** à ceux de la capture initiale sans correction. Chaque variante conserve huit instructions de texture et ajoute quatre contrôles uniformes de mise à l’échelle. Les deux SPIR-V adaptés passent `spirv-val --target-env vulkan1.3 --uniform-buffer-standard-layout`.

Après reprise de Mario, l’utilisateur confirme que les contours de cette version générique restent améliorés et stables sur la scène testée en portable 3×. Cela constitue une validation visuelle de cette scène, sans établir la compatibilité avec les autres jeux.

Ces vérifications de code et de coordonnées ne remplacent pas un essai visuel prolongé dans plusieurs scènes et plusieurs jeux. Les six captures du rapport Mario précédent proviennent de la première correction ciblée et ne sont pas présentées comme une validation de cette version générique.

## Coût

La détection travaille à la compilation du shader, pas à chaque image. Le shader reconnu ajoute quelques opérations uniformes pour contrôler la mise à l'échelle des entrées et partager les coefficients. Il garde ses passes et ses instructions d'échantillonnage ; les deux variantes Mario gardent chacune huit instructions statiques de texture. Le coût exact de la passe n'a pas été isolé par timestamp ; aucun gain de FPS n'est annoncé pour cette correction de qualité.

Les optimisations de présentation déjà réalisées restent indépendantes : stockage intégré pour la couche courante dans la commande différée, et création des pipelines de secours quand une couche les utilise.
