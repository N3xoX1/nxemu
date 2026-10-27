# Réglages GPU et manettes après persistance du host

## GPU

Le signalement est confirmé : `DisplayService::EnsureDisplay()` réutilise le
display Vulkan existant, donc changer `VulkanDevice` ne change pas de carte au
lancement du jeu suivant. Le dialogue Général annonce désormais explicitement
qu'il faut redémarrer NXEmu pour utiliser l'autre carte. L'avertissement sur les
performances d'une carte intégrée conserve aussi cette indication si nécessaire.
Aucune reconstruction dynamique du device n'est ajoutée.

## Manettes hors jeu

Le signalement est confirmé : sans OS invité, `ControllerDialog::OnOpen()` prend
le mode hors ligne, avec clavier éditable et détection des périphériques physiques
en lecture seule. L'action « Controllers » demande maintenant la session utilitaire
avant l'ouverture du dialogue. En jeu, la session existante est réutilisée.

`MainWindow::Update()` conserve cette session tant que le dialogue reste ouvert,
puis la libère lorsqu'aucun autre dialogue utilisateur de cette session ne la
retient. Les chemins Save/Cancel et arrêt de mapping existants s'exécutent avant
le détachement du backend. La bibliothèque seule ne crée toujours pas d'OS invité.
En cas d'échec de création, l'action affiche l'erreur des modules.

## Validation

Compilation Release et test d'intégration UI avec les vrais modules loader/CPU/OS
et le vrai display Vulkan, dans un répertoire utilisateur temporaire :

Suite complète : **11/11 tests réussis** en 22,04 s. Le test UI a ensuite été
rejoué avec une assertion supplémentaire sur l'état d'écoute : réussi.

- Depuis la bibliothèque, trois ouvertures successives par la vraie action UI.
- Mode complet (classe `offline` absente), backend d'entrée disponible et liste
  des périphériques non vide ; session conservée après plusieurs mises à jour.
- Liaison A initialisée dans le backend, effacée dans le dialogue puis annulée :
  liaison restaurée après destruction/recréation de la session.
- Effacement enregistré : liaison toujours vide à la réouverture suivante.
- Fermeture pendant l'écoute d'une entrée et libération de la session.
- Absence de backend invité après chaque fermeture du dialogue.

Ce test valide le chemin de configuration et sa durée de vie avec le backend réel.
Il ne simule pas les axes/boutons d'une manette physique, et ne démontre pas un
changement effectif entre deux GPU après redémarrage sur une machine multi-GPU.
