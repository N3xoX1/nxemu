# Comparaison Bilinéaire, Catmull-Rom et FSR A/B

## Intégration du patch 02

Catmull-Rom est ajouté au filtre d'affichage avec l'identifiant stable 7. Les identifiants des anciens filtres restent inchangés. Bicubique, Gaussian et ScaleForce conservent leur comportement : les suppressions et remplacements par du bilinéaire du patch fourni ne font pas partie de cette intégration.

Le noyau Catmull-Rom conserve ses lobes négatifs et combine seulement les deux poids centraux positifs : neuf lectures bilinéaires au lieu de seize lectures directes. Le calcul des poids est factorisé en forme polynomiale ; taille et inverse de taille sont partagés avec CAS. LOD 0 explicite et clamp aux bords stabilisent le résultat. En correspondance native complète 1:1, y compris les retournements, une seule lecture suffit. Un crop partiel n'active pas ce raccourci.

CAS est optionnel sur Bilinéaire, Catmull-Rom et Plus proche voisin. Il est désactivé par défaut ; à 0 %, le pipeline du filtre sans CAS est choisi. FSR garde son RCAS existant et n'ajoute jamais CAS. Le noyau est une adaptation de CAS en croix, avec coefficient vert, approximation gamma-2 pour le SDR et mélange d'intensité continu jusqu'à zéro. Le correctif reprend les extrema de la croix complète plutôt que les sommes de minima/maxima horizontaux et verticaux du patch.

Les deux algorithmes partagent le même helper Catmull-Rom en Vulkan/OpenGL. Les alpha sont reconstruits par le filtre choisi, mais ne sont pas accentués par CAS. Les shaders restent en FP32 ; le FSR conserve ses chemins FP16/FP32 et son mode A/B.

## Vérification

Sur RTX 5070 Ti, dix scénarios ont été comparés à une référence Catmull-Rom indépendante à seize texels : agrandissement, natif, retournements, crop, marges, réduction, image constante noire/blanche. L'écart RGB maximal est de 1/255 dans la plupart des cas, 2/255 pour la fenêtre 1600×900 vers 1652×929 (précision du filtrage matériel). L'erreur moyenne reste inférieure à 0,018/255. Le natif est identique à la référence. L'alpha avec CAS est identique à celui du filtre sans CAS ; CAS à zéro préserve le filtre de base à 1/255 près.

Zéro erreur de validation Vulkan pour ces scénarios. Les vingt scénarios FSR FP16/FP32 existants passent également. Les trois tests CTest et le test Python des groupes de mesures passent.

Mesures hors jeu, médianes de 160 échantillons après 30 tours de chauffe, couche de validation désactivée :

| Entrée → sortie | Bilinéaire | Catmull direct 16 | Catmull optimisé 9 | Bilinéaire + CAS 50 % | Catmull + CAS 50 % |
|---|---:|---:|---:|---:|---:|
| 1280×720 → 1920×1080 | 0,0150 ms | 0,0477 ms | 0,0366 ms | 0,0209 ms | 0,1308 ms |
| 1280×720 → 3840×2160 | 0,0404 ms | 0,1732 ms | 0,1248 ms | 0,0624 ms | 0,4950 ms |
| 1600×900 → 1652×929 | 0,0129 ms | 0,0375 ms | 0,0292 ms | 0,0173 ms | 0,0996 ms |
| Natif 1280×720 | 0,0104 ms | 0,0271 ms | 0,0103 ms | 0,0129 ms | 0,0156 ms |

Catmull optimisé réduit ici le coût du noyau direct de 23 à 28 % en agrandissement. Ces valeurs concernent ce banc, une seule série et ce GPU ; elles ne prédisent pas les FPS du jeu. Catmull + CAS peut coûter plus que FSR : les cinq pixels nécessaires à CAS sont chacun reconstruits, soit jusqu'à 45 lectures filtrées. CAS reste donc facultatif.

Raw : `build/scaling-02/performance/timings.csv`, images et erreurs dans `build/scaling-02/validation`, régression FSR dans `build/scaling-02/fsr-regression`.

## Essai Mario

Le nouveau paquet démarre avec l'interface visible, en portable 1×, sur Bilinéaire et CAS à 0 %. Le menu de la barre d'état contient Catmull-Rom, A - FSR original, B - FSR optimized et le curseur CAS. Le choix A/B sélectionne automatiquement FSR. Aucun redémarrage n'est nécessaire pour changer de filtre. Le changement peut créer un à-coup de recréation des ressources ; l'analyse écarte les premières secondes.

Faire chauffer la même scène/caméra, débloquer la vitesse, puis mesurer Bilinéaire → Catmull-Rom → FSR A → FSR B pendant environ 20 secondes chacun, CAS à 0 %. Revenir ensuite en sens inverse. Tester CAS séparément à valeur fixe. Garder la taille de fenêtre et la netteté FSR fixes ; une sortie 4K doit être vérifiée dans le CSV.

Les nouveaux CSV enregistrent `selected_filter`, `sharpness_stored`, `cas_strength`, le filtre effectif, le mode FSR et un numéro de phase. Ils distinguent les captures d'applet bilinéaires du vrai affichage, même si les dimensions sont identiques. `input_width/input_height` décrivent la ressource texture ; `viewport_width/viewport_height` décrivent le crop effectivement reconstruit. Cela permet de vérifier le 720p au lieu de l'inférer du libellé 1×.

```powershell
python compare_session.py user/log/fsr-....csv --output comparison.json
```

La qualité en jeu doit encore être jugée sur la même scène : Catmull-Rom peut produire des halos, et la netteté change le grain et les détails. Les tests de référence vérifient l'implémentation, sans conclure à une supériorité visuelle universelle sur FSR.
