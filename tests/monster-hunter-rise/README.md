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
samplers de cubemap et produit un SPIR-V valide. Une seconde capture du build
corrigé contient 30 images et se rejoue sans le jeu. L'inspection RenderDoc
confirme que les deux accès à la même cubemap occupent désormais les bindings
14 et 15 avec deux samplers distincts : maxLOD=15 pour chacun, mip linear pour
l'un et nearest pour l'autre, adressage mirror. Dans la première capture, ils
partageaient le binding 14 et un sampler clamp edge, nearest, maxLOD=0,25.
La cible finale recalculée ne présente plus les plaques brillantes du visage,
ce qui concorde avec le retour visuel de l'utilisateur.

La seconde capture locale est
`user/dump/graphics/20261013_223207_956/menu_trim_trigger_20261013T223935.gfxr`
sous le dossier de diagnostic. Le SHA256 de la DLL utilisée correspond au build
du commit ca45698d9a103044a91cc30c9a086119aa8bbb5e. Les détails des liaisons sont
dans `renderdoc-fixed/face-samplers.json`, et le rendu dans
`renderdoc-fixed/fixed-target-298.png`. Les couches de diagnostic LOD ne sont
pas actives dans cette nouvelle capture.

Cette validation porte sur la vue « Créer votre chasseur » de Monster Hunter
Rise avec Vulkan sur RTX 5070 Ti. Les FPS sans capture, les autres scènes et
le rendu OpenGL restent à vérifier ; la vitesse de relecture du dump ne mesure
pas les performances de l'émulation en jeu.
