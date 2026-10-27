# Blocage de Mario Odyssey après changement de jeu — 27 octobre 2026

La régression provenait du loader rendu persistant par le patch Architecture.
Le contrôleur de fichiers conservait son registre `PID → titre / RomFS / sauvegarde`
après destruction de l'OS invité. Le nouvel OS réutilise le PID `0x51`.
`RegisterProcess()` utilise `emplace` : l'ancienne entrée reste donc sélectionnée.

## Constat avant correction

Dans le même processus NXEmu, lancement de 20XX (`0100749009844000`), arrêt,
puis lancement de Mario Odyssey (`0100000000010000`). Les logs montraient :

```text
PatchExeFS: Patching ExeFS for title_id=0100000000010000
PatchRomFS: Patching RomFS for title_id=0100749009844000
Userspace PANIC!
```

Mario exécutait son propre programme mais recevait les ressources de 20XX.
Le même panic a été observé après vidage du cache, avec `Total Pipeline Count: 0`.
Le nouveau format de cache shader n'était donc pas la cause de ce blocage.

## Correction

`Systemloader::ResetSession()` appelle désormais
`FileSystemController::ResetSession()`, qui vide les associations de processus
sous leur mutex. Ce nettoyage intervient après la destruction de l'OS et de ses
contrôleurs de fichiers empruntés. Les factories NAND/SD et le catalogue host
restent persistants.

Les objets du loader sont partagés par sa DLL et son test au moyen d'une cible
CMake OBJECT. Le test peut ainsi exercer les méthodes internes réellement
compilées, sans exporter une API de test ni recompiler une copie du loader.
La solution Visual Studio native continue d'utiliser les mêmes sources.

## Vérification

- Test `test_loader_session_isolation` : quatre titres, même PID `0x51`, données
  RomFS distinctes. Vérification de l'identité du titre, des octets lus et de
  la destruction des anciennes factories via `weak_ptr`.
- Rejeu sans l'appel de nettoyage : échec sur `borrowed.expired()`.
  Réactivation du correctif : test réussi.
- Recompilation du loader et des cibles de test ; **10/10 tests CTest réussis**,
  dont le test d'isolation ajouté et l'intégration Vulkan sur la carte réelle.
- Vérification dans l'interface : lancement de 20XX jusqu'à son affichage,
  arrêt, puis Mario Odyssey dans le même processus. Mario atteint son menu
  principal, avec environ 60 FPS affichés. Son `PatchRomFS` utilise maintenant
  `0100000000010000`, comme son exécutable.
- La DLL corrigée a été copiée dans
  `bin/x64/Release/modules/loader/nxemu-loader.dll` avant cette vérification.

Cette validation porte sur le blocage au lancement et l'isolation des sessions.
Le débit affiché au menu ne constitue pas un benchmark de performances en jeu.
Les premiers essais de fixture NRO complète ont rencontré une attente à l'arrêt ;
ils ont été remplacés par le test ciblé du registre et ne sont pas comptés comme
des tests réussis d'annulation NRO.
