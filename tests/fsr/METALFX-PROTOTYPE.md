# Prototype MetalFX Spatial / FSR

Ce prototype ajoute un backend natif MetalFX Spatial au chemin de présentation
Vulkan/MoltenVK. Il est désactivé à la compilation et à l’exécution par défaut.
Il n’implémente ni DLSS, ni upscaling temporel, ni interception de shader Switch 2.
Le code natif Apple et son test GPU restent à compiler et à exécuter sur Mac.

## Contrat commun

- `yuzu_video_core/upscaler.h` décrit les dimensions utiles, l’espace couleur,
  l’alpha et les capacités nécessaires. Le contrat temporel décrit explicitement
  profondeur, mouvement, unités, direction, orientation verticale, jitter,
  numéro d’image et réinitialisation d’historique.
- `renderer_vulkan/present/upscaler.h` sépare ces paramètres des handles Vulkan.
  Une implémentation temporelle devra fournir de vraies vues profondeur/mouvement,
  et les maintenir vivantes jusqu’à la fin de leur utilisation GPU. La présence
  de métadonnées ne remplace pas les ressources.
- `Upscaler` expose capacités et exécution ; un refus renvoie au chemin de repli.
  La fabrique propose un adaptateur FSR 1 et le backend MetalFX Spatial optionnel.
  Le chemin FSR existant reste la référence de présentation et de repli.
- Un futur backend temporel devra ajouter ses exigences propres (exposition,
  masques, historique, etc.) et son suivi des séquences. Le contrat ne prétend
  pas suffire à toutes les API temporelles et ne détecte aucun réseau DLSS invité.

## Compiler sur Mac

Utiliser Xcode complet avec un SDK macOS 13 ou plus récent, et les dépendances
habituelles du dépôt. Le contrôle de disponibilité à l’exécution et le lien faible
vers MetalFX permettent de conserver la cible minimale macOS du projet.

```sh
cmake -S . -B build/metalfx -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNXEMU_ENABLE_METALFX=ON \
  -DNXEMU_BUILD_FSR_BENCHMARK=ON
cmake --build build/metalfx --target nxemu nxemu_metalfx_smoke nxemu_upscaler_contract_tests
ctest --test-dir build/metalfx -R 'nxemu_(metalfx_smoke|upscaler_contract)' --output-on-failure
```

Le test `nxemu_metalfx_smoke` crée un scaler RGBA16F, agrandit une image rouge
sur le GPU et vérifie la lecture du résultat. Il est marqué ignoré (code 77)
si macOS ou le GPU ne propose pas MetalFX Spatial. Il ne valide pas à lui seul
l’interopérabilité MoltenVK, qui doit ensuite être testée en jeu.

## Comparer en jeu

Choisir le profil **Qualité** ; L’anticrénelage du profil (CMAA2, ou FXAA en repli) reste identique dans les deux essais.
Utiliser la même scène, le même viewport, la même résolution interne et les mêmes
réglages. L’image d’entrée doit être plus petite que la sortie. Démarrer directement
le binaire du bundle pour transmettre l’environnement, avec le chemin réel du build :

```sh
# Référence FSR
NXEMU_QUALITY_UPSCALER=fsr /path/to/nxemu.app/Contents/MacOS/nxemu

# Prototype MetalFX Spatial
NXEMU_QUALITY_UPSCALER=metalfx \
NXEMU_METALFX_CSV="$PWD/metalfx.csv" \
/path/to/nxemu.app/Contents/MacOS/nxemu
```

La sélection est lue une fois au démarrage, sans modifier les réglages sauvegardés.
Le journal doit indiquer `MetalFX Spatial prototype selected`. Des lignes de données
MetalFX dans le CSV confirment qu’un agrandissement a réellement été exécuté.
Un refus ou une erreur garde FSR ; la variable seule ne prouve pas l’utilisation
effective de MetalFX. Le curseur de netteté FSR règle uniquement la référence FSR :
le résultat MetalFX est présenté directement, sans CAS/RCAS supplémentaire.

Le prototype vise une seule couche opaque SDR. Les captures d’applet, compositions
multicouches, couches avec alpha, réductions, cas natifs 1:1 et crops hors de l’image
conservent le chemin existant. Les crops compatibles et inversions sont matérialisés
dans une texture RGBA16F avant MetalFX. Une vue source sRGB décodée est traitée en
linéaire ; les autres images SDR sont traitées en mode perceptuel.

## Synchronisation et mesures

L’export utilise `VK_EXT_metal_objects` sur le GPU Vulkan sélectionné, jamais un
GPU Metal choisi indépendamment. Les textures exportées restent détenues par les
ressources Vulkan. Les slots sont réutilisés après leur tick, les changements de
dimensions attendent les utilisations antérieures, et le fence Metal ordonne les
passes internes avec les ressources non suivies.

Cette première passerelle fait volontairement `Scheduler::Finish()` puis attend
la fin du command buffer Metal avant de reprendre Vulkan. Elle évite de laisser
courir simultanément des accès natifs et Vulkan non synchronisés, au prix d’une
rupture du pipeline CPU/GPU. Elle doit être remplacée par une synchronisation GPU
par événements partagés avant de comparer les performances de production.

Le CSV optionnel distingue `synchronized_cpu_ms` (normalisation, attentes et MetalFX)
de `metalfx_gpu_ms` (command buffer Metal, sans la normalisation Vulkan). Il est
complété en ajout lors des recréations. Les timestamps FSR existants et les FPS
incluent les attentes de cette passerelle : ne pas conclure que l’un des algorithmes
est plus rapide à partir de ces nombres. Les premières comparaisons portent sur
l’image, les couleurs, les crops et la stabilité des changements de résolution.

## Sources API

- [MetalFX Spatial](https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler)
- [Formats et espace couleur](https://developer.apple.com/documentation/metalfx/mtlfxspatialscalerdescriptor)
- [Export Metal](https://docs.vulkan.org/refpages/latest/refpages/source/vkExportMetalObjectsEXT.html)
- [Déclaration des objets exportables](https://docs.vulkan.org/refpages/latest/refpages/source/VkExportMetalObjectCreateInfoEXT.html)

## Vérifications à effectuer sur Mac

1. Compiler le backend et lancer le test GPU avec la validation Metal.
2. Confirmer une utilisation réelle en jeu, puis forcer le repli FSR.
3. Comparer une scène fixe et une caméra en mouvement ; vérifier couleurs, détails,
   texte et absence de netteté additionnelle inattendue.
4. Redimensionner, passer plein écran, changer de profil, arrêter et relancer un jeu.
5. Vérifier que les cas exclus restent sur FSR/Area et les captures système inchangées.
6. Refaire ces essais avec validation Vulkan et plusieurs tailles d’entrée/sortie.
