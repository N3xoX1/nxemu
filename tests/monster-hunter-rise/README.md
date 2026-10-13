Le banc `run-storage.ps1` extrait le résolveur de buffers des sources de production
et du témoin 1f5f552cb71527a0f42969bc963843576bc4f9ef. Il conserve les 17 cas
des allocations sparse et ajoute six cas de mappings ordinaires : pages physiques
disjointes, lecture et écriture, adresse non alignée, trou après plusieurs pages,
allocation distincte après un trou et descripteur de cbuf personnalisé.

Le témoin échoue sur cinq des 23 contrôles. Le correctif passe les 23 contrôles.
Le résolveur assemble le préfixe invité mappé avec le mécanisme sparse existant,
sans rendre accessible une allocation située après le premier trou invité.
Le chemin des buffers physiquement continus reste inchangé.

Ce défaut de mémoire correspond au problème décrit dans la PR Eden 4362.
Le banc ne prouve pas qu'il cause les reflets excessifs de Monster Hunter Rise.
La bibliothèque yuzu_video_core a aussi été compilée dans un dossier isolé.

Les dumps locaux et essais GPU restent dans `build/monster-hunter-rise-20261013`.
Ils ne sont pas inclus dans le dépôt. La relecture de trois images GFXReconstruct
reproduit les reflets. Une capture RenderDoc permet d'inspecter la passe de peau,
les données et les shaders sans lancer le jeu.

Les contrôles GPU suivants ont été réalisés sur cette capture : arithmétique
stricte (aucun changement), suppression des cubemaps dynamiques (aucun changement),
suppression également de la cubemap statique (réduit la brillance mais enlève
de l'éclairage voulu), extension des LOD des samplers (aucun changement).
La suppression de cubemap est un diagnostic et n'est pas intégrée au moteur.
