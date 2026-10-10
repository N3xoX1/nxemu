Tests du correctif graphique Immortals

`run-indirect-dispatch.ps1` extrait les méthodes DMA et KeplerCompute du commit
25a0b0e4 et des sources courantes. Il reproduit la perte de provenance des paramètres
de dispatch écrits par le GPU, puis téléchargés avant le lancement. Il vérifie les
envois simples et multiples aux niveaux Normal et High, le nettoyage entre deux
lancements, les continuations partielles et la provenance des paramètres de macros.
Le témoin échoue sur six des seize cas ; la correction doit passer les seize.
Les doubles de mémoire permettent de contrôler cette course sans lancer le jeu.

`run-shaders.ps1` lie le banc au recompileur SPIR-V réellement construit. Il couvre
une image Buffer en lecture seule, la lecture/écriture, et les tailles privées et
partagées identiques ou différentes, avec SPIR-V 1.6 et disposition Workgroup explicite.
Les shaders sont vérifiés par spirv-val (Vulkan 1.3). Les cas 0 et 2 doivent échouer
avec `-Variant original -LibraryRoot <checkout-original-construit>`. Ces quatre cas
et un cinquième cas de handles d'images 3D chargés depuis la mémoire partagée
doivent passer avec la bibliothèque corrigée.

`run-storage-array.ps1` exécute ce cinquième shader sur le GPU : handles divergents,
formats float/uint/sint, handle invalide ignoré, texture valide mais inutilisée
inchangée et masque d'écriture exact. `run-query-scope.ps1` vérifie la portée des
requêtes d'occlusion, 16 puis 32 pixels, et la transition d'une image de présentation.

`run-probe-lifetime.ps1` extrait le corps Windows de la détection des GPU depuis
la PR d'origine et les sources courantes. Un dispatch instrumenté vérifie que la
surface et l'instance sont détruites avant le déchargement de la bibliothèque,
en succès comme en cas d'erreur. Il ne déclenche pas la capture F10 d'un jeu.

`run-captured-shaders.ps1` recompille les deux programmes Maxwell `.ash` conservés
dans le dossier local de diagnostic et vérifie leur SPIR-V. Les métadonnées de
ressources sont synthétiques ; les dumps de jeu ne sont pas inclus au dépôt.

`run-vulkan.ps1` utilise la carte Vulkan réelle et les masques extraits du code de
production, avec vérification de l'intégration dans les trois chemins Dispatch.
Il teste WFI avant lecture GPU, écrasement/copie de résultat de requête puis lecture,
transition d'image suivie d'une lecture, et deux calculs dépendants. Chaque témoin
sans correction doit produire un conflit de synchronisation. Les chaînes corrigées
doivent avoir zéro erreur de validation et restituer les valeurs attendues.
La validation des accès shaders est activée explicitement : elle est désactivée
par défaut dans le SDK utilisé. Les mesures GPU utilisent 21 échantillons après
échauffement, sans couche de validation ni capture.

Pré-requis : solution Release x64 construite, MSVC v145, Vulkan SDK 1.4.357.0,
Python, et les fichiers générés par `tests/pr396/run-tests.ps1` et le shader de
`tests/pr396/run-vulkan.ps1`. Les résultats restent dans
`build/immortals-menu-diagnostic-20261009/capture-analysis`.

Les relectures GFXReconstruct ne lancent pas le jeu. Elles vérifient la même
séquence de commandes GPU ; un essai interactif reste nécessaire pour confirmer
la disparition durable d'un défaut intermittent.
