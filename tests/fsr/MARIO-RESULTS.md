# Mario Odyssey : mesures A/B

Analyse du 18 octobre 2026 de la capture du 17 octobre `fsr-20261017-202756-303.csv`. Phases à une couche, sortie identique 1652 × 929. Les trois premières secondes et la dernière seconde de chaque phase sont écartées. Les phases initiales (menus, chargement, changement de taille) sont exclues de la comparaison.

| Mode | Temps GPU médian | Cadence de soumission approximative | Passes FSR intermédiaires |
|---|---:|---:|---:|
| A original, phases 1 et 3 | 0.068384 ms | 260.1/s | 2 |
| B avec netteté, phases 2 et 4 | 0.052688 ms | 260.2/s | 1 |
| B netteté désactivée, phase 4 filtre 6 | 0.036640 ms | 260.4/s | 0 |

B réduit l'intervalle GPU de présentation de 23.0 % face à A. Désactiver la netteté réduit encore cet intervalle de 30.5 % face à B avec RCAS, soit 46.4 % face à A avec netteté. Les économies absolues sont respectivement 0.015696 et 0.016048 ms.

La suppression de RCAS à 0 % est confirmée : filtre effectif 6 et aucune passe FSR intermédiaire. Le GPU exécute EASU dans la présentation finale. L'ancien chemin conserve ses deux passes intermédiaires.

La cadence reste proche de 260 images soumises/s ; ces mesures ne montrent aucun gain significatif de FPS du jeu. Ce compteur est une cadence de préparation de présentation, pas une mesure indépendante des images affichées. Les changements de netteté non nuls ne sont pas consignés dans cette version du CSV ; ces phases sont regroupées en « avec netteté », sans prétendre certifier une amplitude identique du curseur.

Ce test est fenêtré, pas une sortie 4K. Les journaux indiquent une texture source de 1600 × 900 ; ses dimensions ne certifient pas le viewport recadré du jeu. Le mode de lancement portable 1× ne suffit donc pas, à lui seul, à certifier une entrée effective 1280 × 720.

Les métriques de performance ne jugent pas la qualité visuelle d'une vraie scène. La comparaison subjective appartient à l'utilisateur ; la validation hors jeu vérifie l'équivalence du RGB en agrandissement (FP16 identique sur la mire, FP32 à 1/255 près), avec un traitement séparé du raccourci natif.

Résumé complet des trois sessions : `build/fsr-validation/mario-all-sessions.json`.
