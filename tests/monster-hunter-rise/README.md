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

Le banc `run-constant-sampler.ps1` utilise la vraie passe `TexturePass`, puis
le compositeur de handles utilisé par le moteur vidéo. Le témoin est la version
2c68b91222e0a01e43c143e2917c40920b510868, compilée avec les en-têtes actuels pour
conserver la même ABI. Le témoin échoue sur 13 des 23 contrôles ; la correction
passe les 23. Le banc requiert MSVC et une bibliothèque du recompileur reconstruite
avec les en-têtes actuels ; son emplacement se règle avec `-LibraryRoot`.

Le shader de peau enregistré utilise des OR immédiats pour combiner le handle
de texture avec les samplers 267 et 263. La passe perdait ces constantes et
fusionnait les accès à une même texture malgré leurs samplers différents.
La correction conserve les constantes dans les descripteurs et les applique
dans les chemins graphiques et compute de Vulkan et OpenGL. Elle invalide aussi
les anciens caches de pipelines, dont les environnements omettent ces handles.
Elle n'ajoute ni lecture GPU, ni synchronisation, ni remplacement d'éclairage.
La compilation des bibliothèques du recompileur et du moteur vidéo a été vérifiée.

La relecture GPU avec arithmétique stricte est inchangée. Supprimer la cubemap
statique atténue les reflets mais enlève une partie de l'éclairage prévu : ce
diagnostic n'est pas intégré au moteur. L'extension des LOD des samplers capturés
atténue également les reflets dans la cible finale du jeu. Les images présentées
par la courte capture restent identiques, car elles étaient déjà en attente
d'affichage ; elles ne permettent pas d'évaluer cette modification.

La recompilation hors jeu du shader de peau distingue maintenant les deux
samplers de cubemap et produit un SPIR-V valide. L'essai LOD montre l'importance
du sampler, mais ne remplace pas une validation du correctif exact : le dump
ne contient pas les descripteurs Maxwell des samplers auparavant ignorés.
Il reste à capturer la même vue avec le module corrigé, vérifier les samplers
réellement liés, comparer le rendu et mesurer les performances en jeu.
