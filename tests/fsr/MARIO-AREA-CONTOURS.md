# Diagnostic des marches sur la tour — Mario Odyssey / Vulkan

## Conclusion établie

Les grosses marches de la tour sont déjà présentes dans la texture source portable, avant Area. Le rendu Area courant correspond à l'intégrale CPU indépendante de cette source. En passant le jeu en docké, sans changer Area, le contour s'affine et la forte répétition horizontale par groupes de trois pixels disparaît.

La trace complémentaire identifie maintenant un mécanisme précis : **le shader de reconstruction portable `d7dd8ad8486de9d1` quantifie les coordonnées horizontales à la résolution native, après la conversion des coordonnées par le rescaler de NXEmu**. À 3×, ses coordonnées de lecture et son choix de colonne paire/impaire se répètent par groupes de trois pixels physiques. Ce comportement correspond aux répétitions observées dans la texture source. Area reçoit donc déjà un contour quantifié.

Il s'agit d'une limite de l'adaptation de ce shader à la résolution interne augmentée. Les calculs entiers du jeu ont un sens à la résolution native ; leur présence ne constitue pas en soi un bug du jeu. La suite de la trace a identifié la variante alternée `6e21983e3e40c587`. Les deux sont désormais adaptées et le clignotement a disparu en portable 3×. Voir [la correction et ses vérifications](MARIO-RECONSTRUCTION-FIX.md). Les sections ci-dessous décrivent le diagnostic initial, avant correction.

## Passe précise et calcul confirmé

Trace Vulkan `trace-1792341333580.txt` : deux images, 2 460 draws, soit 1 230 par image. Aucun événement `SCALEUP`/`SCALEDOWN` pendant ces deux images : cette trace n'indique pas un aller-retour intermédiaire vers une image native par le cache de textures.

Chaîne de fin d'image relevée sur la première image :

| Draw | Fragment shader | Rôle observé |
| --- | --- | --- |
| 1191 | `d7dd8ad8486de9d1` | Reconstruction, sortie 3840×2160, deux attachments. Entrées natives annoncées : trois 800×900 et une 640×720, toutes marquées rescalées. |
| 1192 | `442b2bc3474175fe` | Traitement de l'image et de l'attachment auxiliaire précédents, sortie 3840×2160. |
| 1230 | `baa94d419eba5ee0` | Copie finale par UV normalisées, après les draws d'interface du jeu, vers l'allocation 4800×2700. |

Les tailles natives d'allocations ne donnent pas à elles seules la taille utile des différentes passes ; il ne faut pas les assimiler systématiquement à la résolution de rendu de toute la scène.

L'IR final du shader 1191 contient le calcul suivant, résumé sans les conversions de types et protections NaN :

```text
x_natif = gl_FragCoord.x * ResolutionDownFactor  // 1/3 dans ce test
colonne = trunc(x_natif)
demi_colonne = trunc(x_natif * 0.5)
parite = colonne & 1
u = (2 * float(demi_colonne) + 1) * constante_du_jeu
```

Il existe plusieurs constantes/lectures, mais toutes les dépendances de la position X passent par ces deux troncatures ou leurs vérifications NaN. Les UV horizontales et les décisions de reconstruction perdent ainsi la position à l'intérieur d'un pixel natif. La position Y suit un calcul continu, ce qui explique l'asymétrie horizontale.

| Colonnes physiques à 3× | `colonne` | `demi_colonne` | `parite` |
| --- | --- | --- | --- |
| 0, 1, 2 | 0 | 0 | 0 |
| 3, 4, 5 | 1 | 0 | 1 |
| 6, 7, 8 | 2 | 1 | 0 |
| 9, 10, 11 | 3 | 1 | 1 |

Le script `analyze_portable_reconstruction.py` suit le graphe SSA de l'IR **final réellement compilé**, puis évalue en float32 les troncatures sur toutes les colonnes utiles. Résultat à 3× : **3 840 colonnes, 1 280 couples distincts, tous les groupes de trois identiques**. La même évaluation à 1×, 2×, 4×, 5× et 6× garde 1 280 couples distincts : augmenter le facteur seul ne retire pas cette quantification.

Cette analyse vérifie les coordonnées et les décisions issues de X, pas l'identité de tous les pixels de sortie du shader : les dérivées implicites pour le LOD et les traitements ultérieurs restent distincts. Les captures de la source finale apportent la confirmation visuelle indépendante du motif. Une capture avant/après chaque passe ou un correctif contrôlé permettraient encore de mesurer la contribution de cette passe par rapport aux détails déjà absents de ses entrées.

## Conséquence pour un correctif

`PatchFragCoord` dans `src/yuzu_shader_recompiler/ir_opt/rescaling_pass.cpp` remet les coordonnées dans les unités attendues par le jeu. Le même fichier sait réintroduire une phase sous-pixel pour les lectures entières `ImageFetch`/`ImageRead` via `SubScale`, mais cette reconstruction utilise des **`ImageSampleImplicitLod` à UV normalisées**, qui ne passent pas dans ce traitement.

La correction doit adapter ensemble la grille de reconstruction, le choix pair/impair et les différentes entrées. Supprimer globalement la division des coordonnées, la troncature ou le test de parité changerait le fonctionnement d'autres shaders. Il faut aussi contrôler les contours en mouvement et la stabilité temporelle, au-delà d'une capture statique. La correction ciblée et les vérifications ultérieures sont décrites dans le rapport lié plus haut.

## Preuves sur des captures réelles

Capture portable `capture-1792340525618` :

- Taille de texture lue par `textureSize` sur le GPU : 4800×2700, identique à celle annoncée par le CPU.
- Crop [0, 0, 0.8, 0.8], soit 3840×2160 utiles ; cible utile 1150×647.
- AA de présentation désactivé, Area sélectionné ; capture source avant l'AA de présentation et sortie avant watermark/UI, pour la même image.
- Comparaison de tous les pixels affichés à une intégration CPU séparable indépendante : erreur RGBA maximale **0,758/255**, erreur moyenne **0,142/255**, aucun pixel à plus de 2/255.
- Les marches sont directement visibles dans le fichier `-source.png`, sans réduction. Exemple de luminances source contiguës au bord (ligne 750) : 101,7 ; 101,7 ; 101,7 ; 132,7 ; 238,0 ; 238,0 ; 238,0 ; 233,3 ; 233,3 ; 233,3. La quantification horizontale existe déjà avant Area.

Capture dockée `capture-1792340668779` :

- Taille GPU : 4800×2700 ; crop entier, soit 4800×2700 utiles ; même cible utile 1150×647 et même Area.
- Erreur RGBA maximale / intégrale CPU : **0,709/255** ; aucun pixel à plus de 2/255.
- Le contour est nettement plus continu. Sur une région de la tour, la moyenne des gradients horizontaux selon leur phase modulo 3 vaut [3,347 ; 0,092 ; 0,852] en portable, contre [1,534 ; 1,500 ; 1,526] en docké. Cela appuie la répétition/quantification spécifique au chemin portable.

Les captures sont prises au même emplacement, à des instants différents ; l'animation et le léger mouvement de caméra empêchent une différence pixel à pixel entre les modes. Le docké fournit aussi 1,25× plus de résolution utile par axe : le test ne prétend pas isoler un changement unique de shader. Les fichiers source donnent cependant une preuve directe de la répétition horizontale portable, indépendamment de la réduction finale.

![Portable à gauche, docké à droite, Area inchangé — extraits agrandis au plus proche voisin pour inspecter les pixels](../../build/area/portable-docked-area.png)

## Portée et limites

Le compteur « 2160p » décrit la taille utile du framebuffer, pas la finesse réelle de chaque passe qui le compose. Un filtre Area ne peut pas recréer la couverture géométrique qui n'est plus dans sa source. Le test docké est une amélioration pratique démontrée, pas la preuve d'une correction du rendu portable. La trace initiale localise la quantification d'une passe de reconstruction ; elle ne démontre pas que les textures en entrée contiennent déjà tous les détails souhaités. La correction et les vérifications réalisées ensuite sont détaillées dans MARIO-RECONSTRUCTION-FIX.md.

## Outil ajouté

Capture Vulkan opt-in uniquement : `NXEMU_SCALE_CAPTURE=chemin_du_dossier`. Écrire/actualiser `capture.request` dans ce dossier déclenche une paire source/sortie de la même image. La taille source est lue sur GPU avec un petit shader de diagnostic, puis la vue est copiée 1:1 par échantillonnage nearest dans une image RGBA8. Le code respecte le format et les swizzles de la vue présentée. L'image de sortie est lue avant le watermark.

Hors mode diagnostic : pas de polling disque, d'allocation de capture ou de synchronisation supplémentaire. Une capture, en revanche, force une lecture CPU et perturbe la cadence : **ne pas utiliser ces passages pour mesurer les performances**.

Dans le paquet : `Diagnostiquer-Area.cmd` lance le mode, `Capturer-image.cmd` demande une capture. Le lanceur de comparaison habituel laisse ce diagnostic désactivé. Analyse : `python tests/fsr/analyze_scale_capture.py capture-TIMESTAMP.json` produit les PNG, la référence CPU et les statistiques. Les hypothèses de comparaison sont une couche opaque et l'AA de présentation désactivé. Les métadonnées initiales nomment `filter` le choix de downscale (0 Bilinear, 1 Area) ; le code final nomme explicitement ce champ `downscale_filter`.

Les nouvelles fonctions d'intégration du script ont été vérifiées contre une intégration brute par texel sur 736 pixels de petites images, y compris des crops inversés et hors texture.
