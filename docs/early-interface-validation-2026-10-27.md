# Affichage anticipé et validation des sessions — 27 octobre 2026

## Changement

Le frontend initialise d'abord SDL/RmlUI, les deux polices embarquées et le module
vidéo. Il affiche ensuite une vue légère de bibliothèque avec un état de chargement,
avant de charger les modules loader/CPU/OS, les polices système et les dialogues.
La fenêtre native reste masquée jusqu'à la première soumission d'interface.
Le device Vulkan et le contexte RmlUI sont conservés pour l'interface complète.

Les phases différées traitent les événements entre étapes et entre polices, sur le
thread UI. Ce découpage n'est pas une initialisation entièrement parallèle : une
DLL ou une police lente peut encore retarder le traitement du prochain événement.
Vulkan reste nécessaire avant le premier affichage. La vue initiale est en anglais.

`SetupDisplay` et `CompleteFrontendSetup` séparent ces responsabilités. L'échec
des services conserve le display jusqu'au détachement de l'interface. L'ancienne
entrée `SetupFrontend` conserve son contrat tout-ou-rien.

L'annulation précoce libère désormais le rendez-vous des workers CPU déjà créés,
même si le GPU n'a pas terminé son démarrage. Sinon le démontage du noyau pouvait
attendre des workers encore bloqués sur cette barrière. Le chemin annulé n'appelle
pas `System::Run`.

## Mesures

Release Windows x64, Ryzen 9800X3D, RTX 5070 Ti, trois ouvertures successives avec
la configuration et la bibliothèque existantes. Temps depuis l'entrée application,
mesurés dans le journal ; premier affichage = soumission, pas mesure écran physique.

| Ouverture | Première soumission | Interface complète prête |
|---|---:|---:|
| 1 | 365 ms | 626 ms |
| 2 | 373 ms | 643 ms |
| 3 | 380 ms | 652 ms |
| Médiane | 373 ms | 643 ms |

La première vue précède l'interface complète d'environ 270 ms. Ce n'est pas un
benchmark A/B avec l'ancien exécutable, ni un gain de FPS ou de compilation shaders.
La première mesure précède l'extraction de la vue dans son en-tête ; les deux
suivantes utilisent l'implémentation finale. Sources et données dans
`build/early-interface-20261027/` (journaux locaux ignorés par Git).

## Raffinement : transition sans bibliothèque en construction

La vue provisoire de bibliothèque a été remplacée par un écran dédié : nom NXEmu,
état et barre animée, sans fausses cartes. Son fond opaque et son ordre au premier
plan couvrent les documents créés pendant la préparation. La première image est
soumise avant de révéler la fenêtre native.

La transition attend maintenant la fin du scan initial (réussi ou échoué), la
consommation de ses résultats et le chargement des icônes de la zone virtualisée.
Deux mises à jour prêtes consécutives laissent le layout se stabiliser, puis
l'écran est retiré et la bibliothèque présentée dans la même frame. Aucun délai
minimum n'est ajouté. Un scan lent reste derrière l'écran de chargement, avec
traitement des événements et fermeture possibles entre mises à jour.

Avec les 81 jeux de la bibliothèque réelle : première soumission **356 ms**,
bibliothèque prête **885 ms**. La seconde mesure inclut désormais la préparation
du catalogue et des premières jaquettes ; les mesures précédentes correspondaient
à un document initialisé avant cette préparation. Journal local :
`build/early-interface-20261027/startup-splash-real.log`.

Nouvelle compilation Release et **11/11 tests réussis** (21,52 s). Le test UI
vérifie aussi l'occlusion d'un document montré après le splash à plusieurs points
de la fenêtre et la transition vers le vrai MainWindow avec catalogue vide, en
complément des chemins d'erreur et d'annulation. Les thèmes clair et sombre sont
exercés. Vérification graphique de la bibliothèque réelle avec jaquettes chargées.

Le splash utilise désormais le PNG existant `images/nxemu-logo.png`, centré sur un
fond bleu nuit fixe pour conserver le contraste du mot-symbole blanc, avec statut
discret et barre cyan animée. Aucun nouvel asset ni délai produit n'est ajouté.
Son URL de base suit l'échappement des chemins RmlUi, y compris les lecteurs
Windows, et fonctionne avec les ressources empaquetées. Le test vérifie les
dimensions intrinsèques positives du logo pour détecter une image manquante.
Test d'intégration UI réussi après recompilation et aperçu graphique vérifié.
L'option `--preview` du programme de test garde temporairement ce splash visible
pour inspection ; elle n'est utilisée ni par CTest ni par le produit.

## Tests

Compilation Release et **11/11 tests CTest réussis**. Assertions actives en Release.

- `test_ui_startup` utilise les vrais SDL, RmlUI et Vulkan : rendu avant les
  services volontairement absents, rendu encore valide après leur échec,
  fermeture pendant les polices différées et bouton de fermeture.
- `test_module_lifecycle` attache et rend le display avant les services ; leur
  complétion répétée conserve le même objet vidéo.
- `test_launch_gate` exerce 840 scénarios avec le vrai `EmuThread` et des
  dépendances contrôlées, dont arrêt pendant un GPU volontairement bloqué,
  libération de la barrière CPU, aucune exécution invitée et fermeture du processus.
- Les tests existants couvrent aussi l'isolation du loader et les sessions Vulkan.

Dans un même processus graphique, vérification manuelle de :

1. Chargement de 20XX jusqu'à son animation, redémarrage, puis arrêt et bibliothèque.
2. Chargement de Mario Odyssey après 20XX, jusqu'au menu ; redémarrage puis arrêt
   demandé immédiatement pendant le démarrage, retour à la bibliothèque.
3. Chargement de Sonic X Shadow Generations, arrêt demandé depuis l'écran de
   démarrage, confirmation et retour à la bibliothèque.
4. Nouveau chargement de 20XX après Sonic, animation visible puis fermeture propre.

Dans les deux arrêts précoces manuels, le jeu avait déjà commencé à produire des
frames au moment de la confirmation. Ils ne prouvent donc pas une annulation au
milieu d'une compilation shaders. Cette fenêtre précise est couverte par les tests
contrôlés, pas par un cache réel long à compiler dans cette session.
Pas de purge des caches. Aucun PANIC, Critical ou assertion relevé dans le journal
de cette séquence. Ces essais de démarrage ne remplacent pas des heures de jeu,
des tests multi-GPU ou un benchmark de présentation asynchrone.
