# Reconstruction portable — correction Vulkan

**État actuel : la sélection par hashes décrite ci-dessous a été remplacée par une [règle structurelle sans identité de jeu](GENERIC-RECONSTRUCTION.md).** Ce document conserve les preuves et résultats de la première correction ciblée ; la portée actuelle, les bornes UV et les tests de la version générique sont décrits dans le nouveau rapport.

Le problème venait de deux shaders de reconstruction alternés, `d7dd8ad8486de9d1` et `6e21983e3e40c587`. Ils calculent une colonne entière, une demi-colonne et une parité paire/impaire. L'adaptation générique de `gl_FragCoord` à la résolution native faisait répéter ces décisions et leurs lectures par groupes de trois colonnes à 3×.

## Correction

Pour ces deux flux d'instructions précis, le rescaler conserve la grille physique des fragments et adapte en même temps les quatre dimensions réciproques du shader. Le choix de parité, les centres de paires, les lectures et les décalages voisins restent ainsi exprimés sur la même grille. La limite UV est ajustée pour conserver la même zone utile avec une marge d'un demi-texel physique. L'origine normalisée du viewport reste inchangée.

Les dimensions adaptées sont partagées entre toutes les branches/lectures du shader, dès le bloc d'entrée. Cela évite de répéter leurs calculs à chaque utilisation ; il ne s'agit pas d'un calcul CPU par image. Aucun rendu intermédiaire, aucune passe et aucune instruction d'échantillonnage supplémentaire n'ont été ajoutés. Chaque variante garde ses huit instructions statiques `ImageSampleImplicitLod` ; le chemin effectivement exécuté dépend toujours des branches et de l'image.

La correction est active par défaut dans le chemin Vulkan pour ces deux shaders lorsque le facteur interne dépasse 1×. Elle ne modifie ni les autres shaders, ni le rendu 1× ou sous-échantillonné. OpenGL n'active pas cette compatibilité. Une version du jeu utilisant d'autres hashes conserve le comportement générique : il faudra vérifier sa reconstruction avant de l'ajouter.

`NXEMU_HALF_RATE_RECONSTRUCTION=0`, lu au démarrage de la compilation des shaders, permet un A/B avec le comportement précédent. Les lanceurs A/B du paquet utilisent le même exécutable et la même sauvegarde. Le lanceur habituel active la correction par défaut.

## Vérifications

- Compilation Release de NXEmu réussie.
- Deux tests CTest passent : correction activée et désactivée. Ils exécutent le vrai `RescalingPass`, puis la vérification SSA/types/ordre des définitions et un évaluateur des coordonnées de l'IR. Référence indépendante sur toute la largeur utile pour les facteurs 0,5×, 1×, 2×, 3×, 4×, 5× et 6×, les deux hashes, deux hashes non concernés et des render targets non rescalées. Les facteurs autres que 3× sont ici une validation des calculs, pas des essais en jeu.
- Les SPIR-V des deux variantes de l'essai en jeu passent `spirv-val --target-env vulkan1.3 --uniform-buffer-standard-layout`, correspondant à la disposition des UBO du moteur.
- En jeu : Mario Odyssey portable 3×, même scène du Pays des Chapeaux. Adapter une seule variante provoquait une alternance visible ; adapter les deux a supprimé ce clignotement, confirmé par l'utilisateur.
- Six présentations successives capturées avec les deux variantes adaptées : `capture-1792342455076` à `capture-1792342455293`. Les gradients horizontaux dans la même région source, groupés par phase modulo 3, valent respectivement `[2,894 ; 2,778 ; 2,764]`, `[3,075 ; 2,959 ; 2,953]`, `[3,129 ; 3,007 ; 3,002]`, `[3,139 ; 3,020 ; 3,013]`, `[2,822 ; 2,715 ; 2,700]`, `[2,919 ; 2,800 ; 2,779]`. L'ancienne source `capture-1792342086819` donnait `[4,028 ; 0,123 ; 1,191]` : la forte répétition par triplets disparaît dans les six captures.

La scène est animée et la caméra peut bouger légèrement : ces captures démontrent la suppression de la quantification, pas une différence pixel à pixel entre deux images figées, ni l'élimination de tout aliasing. Les captures forcent des lectures GPU/CPU et ne servent pas à mesurer les performances. Un essai prolongé dans d'autres scènes et en mouvement reste utile pour détecter d'éventuels défauts temporels.

## Coût et améliorations de présentation

Le correctif de reconstruction ajoute quelques opérations arithmétiques uniformes et change la répartition des branches par colonne. Le nombre de passes et d'instructions de texture reste identique. Son coût GPU exact n'a pas été isolé par un timestamp de cette passe ; aucun gain de FPS n'est annoncé pour ce correctif de qualité.

Deux coûts CPU évitables ont été corrigés dans `WindowAdaptPass::Draw` :

1. Les trois vecteurs de données par couche, puis leurs copies dans la commande différée, deviennent une seule liste avec stockage intégré pour la couche courante, déplacée dans la commande. Le cas habituel à une couche évite les six allocations des anciens vecteurs. Les couches multiples restent prises en charge et les données restent possédées jusqu'à l'exécution de la commande GPU.
2. Les pipelines de secours Bilinear/Area sont créés seulement quand une couche les utilise réellement. Les samplers sont disponibles avant la préparation des descriptors ; le shader et ses trois pipelines de blend sont différés. Cela évite des créations de pipelines inutilisées au démarrage/changement de filtre.

Ces suppressions de travail sont vérifiables dans le code, mais aucun pourcentage de gain global n'est revendiqué. Les anciennes/nouvelles sessions ont aussi varié le filtre, la taille de fenêtre et la limite de cadence : elles ne constituent pas un benchmark contrôlé de ces seules modifications.

Autres pistes repérées, non modifiées : allocation anticipée des images/staging de secours même lorsque `AccelerateDisplay` réussit, et coût du rendu de capture applet. Il faut vérifier leurs consommateurs et leur synchronisation avant de les rendre paresseux. Un audit complet des performances de l'émulateur dépasse ce chemin de reconstruction/présentation.
