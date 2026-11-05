# Mario Odyssey — résultats du 18 octobre 2026, 12:54

Source : `fsr-20261018-125443-521.csv`, dernier essai après correction du sélecteur.
Vulkan, image utile 1280×720 dans une texture 1600×900, sortie 1652×929.
Les 3 premières secondes et la dernière seconde de chaque phase sont retirées ;
les captures internes de l'applet sont exclues. Médianes des temps GPU du
filtrage et de la présentation, pas du rendu complet du jeu.

| Filtre | CAS désactivé | CAS à 50 % |
|---|---:|---:|
| Plus proche voisin | 0,008736 ms | 0,011616 ms |
| Bilinéaire | 0,008832 ms | 0,011776 ms |
| Catmull-Rom classique | 0,024960 ms | 0,045984 ms |
| Catmull-Rom rapide | 0,017440 ms | 0,038752 ms |
| Bicubique | 0,015456 ms | non pris en charge |
| ScaleForce | 0,027072 ms | non pris en charge |

FSR B avec netteté active : 0,053376 ms. Deux passages stables, netteté affichée
88 % et 68 %, donnent la même médiane. FSR B avec netteté à 0 % (EASU seul) :
0,036960 ms, soit 30,8 % de moins. Aucun passage FSR A stable dans cet essai.
CAS n'est jamais ajouté à FSR, Bicubique ou ScaleForce. Le bref passage
Bicubique avec CAS réglé à 50 dans les paramètres reste donc du Bicubique seul.

- Catmull rapide réduit le coût de 30,1 % sans CAS, et de 15,7 % avec CAS 50 %,
  par rapport au classique dans cette session.
- CAS ajoute environ 0,00294 ms au bilinéaire, et 0,0210 à 0,0213 ms à Catmull.
  Les deux Catmull avec CAS utilisent bien une passe intermédiaire ; les
  autres filtres du tableau sans CAS et Bilinéaire+CAS en utilisent zéro.
- Bilinéaire+CAS est 77,9 % moins coûteux que FSR B avec netteté active ;
  Catmull rapide+CAS est 27,4 % moins coûteux. Les rendus sont différents :
  ces écarts ne constituent pas une équivalence de qualité.
- Sur les phases comparables Bilinéaire, Catmull, ScaleForce et FSR, la cadence
  reste à environ 251–253 soumissions/s. Pas de gain global mesurable sur ces
  passages. Le premier passage Plus proche voisin appartient à la phase
  initiale et atteint environ 324 soumissions/s ; il ne sert pas de référence
  de FPS pour les phases suivantes.

Les temps sont stables : p95 Bilinéaire 0,009088 ms, Catmull classique 0,025248,
Catmull rapide 0,017664 ; avec CAS 50 %, respectivement 0,012064, 0,046464 et
0,039232 ms. Le coût mesuré ne suggère pas de problème persistant de pipeline.
Ces logs ne permettent pas de conclure sur la qualité visuelle en mouvement.

Le détail par phase et le nombre d'échantillons sont sauvegardés dans
`build/scaling-ui-fix/mario-latest-summary.json`.
