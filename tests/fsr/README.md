# FSR Vulkan : revue et comparaison A/B

Le patch est intégré et adapté à cette révision de nxemu. Les résultats matériels et les limites sont dans [RESULTS.md](RESULTS.md).

## Comparaison en jeu

Dans le menu du filtre de la barre d'état, choisir **A - FSR original** ou **B - FSR optimized**. Le changement prend effet entre deux images et recrée les ressources en attendant leur libération. La barre affiche `FSR [A]` ou `FSR [B]`. Le choix est une surcharge de session ; il ne remplace pas les réglages FSR globaux ou par jeu.

Garder la même scène, la même caméra, la même netteté et les mêmes résolutions. Faire chauffer les shaders, puis A/B/A/B pendant environ 20 secondes par phase, menu fermé. Écarter au moins trois secondes après chaque changement. Une scène en mouvement ne permet pas une comparaison pixel par pixel des captures.

Mario Odyssey en mode Unlocked convient pour mesurer la cadence. Une sortie 4K exige un viewport de 3840 × 2160 : le réglage interne « 2160p » ne suffit pas à le garantir. Les colonnes `input_width`, `input_height`, `width`, `height` indiquent ce qui passe réellement dans FSR. À résolution native, B saute EASU et modifie donc l'image ; ce cas se distingue de l'agrandissement, où l'objectif est l'équivalence visuelle.

## Mesures

Définir `NXEMU_FSR_METRICS` avec un chemin CSV neuf avant le lancement. Le paquet `Tester-FSR-AB.cmd` le fait automatiquement et utilise un fichier horodaté dans `user/log`.

- `cpu_ms` : préparation/enregistrement CPU de la présentation ; exclut compilation/recréation du filtre et collecte des requêtes.
- `gpu_ms` : intervalle GPU autour de la préparation des couches et de leur présentation. Inclut l'anticrénelage, les éventuels uploads et les dépendances GPU ; ce n'est pas le coût complet du jeu ni un chronométrage isolé de RCAS.
- `frame_interval_ms` : intervalle entre deux préparations de présentation du même objet. Permet une estimation de cadence de soumission, distincte des FPS effectivement affichés.
- `implementation`, `segment` : mode capturé avec l'image. Les résultats GPU différés conservent leur étiquette après un changement A/B.
- `offscreen_passes` : nombre de passes FSR intermédiaires pour cette image : A = 2, B en agrandissement avec netteté = 1, B en natif ou sans netteté = 0.

Sans la variable de mesures, aucun pool de requêtes ni fichier n'est créé. Avec mesures, un anneau de 16 paires de timestamps est lu sans attente par image. `NaN` signifie GPU non disponible/anneau occupé ; aucune mesure n'est inventée. Le teardown du diagnostic attend la fin des requêtes. Les écritures CSV sont synchrones : les temps et FPS globaux restent ceux d'un lancement instrumenté. Les deux modes doivent utiliser la même instrumentation.

Les captures d'applet utilisent leur filtre bilinéaire habituel ; les scripts d'analyse les excluent. Pour plusieurs couches, les dimensions d'entrée concernent la dernière couche FSR ; la comparaison de session sélectionne les images à une couche.

```powershell
python tests/fsr/compare_session.py capture.csv --settle 3 --trim-end 1 --output session.json
python tests/fsr/compare_metrics.py before.csv after.csv --warmup 120 --output comparison.json
```

`compare_session.py` sépare les phases et configurations. Il ne prétend pas que des scènes différentes sont comparables. `NXEMU_FSR_LEGACY=1` donne le mode A initial au frontend ; le menu peut ensuite le changer. `0` initialise B.

## Banc d'essai Vulkan hors jeu

```powershell
cmake -S . -B build/windows-x64 -DNXEMU_BUILD_FSR_BENCHMARK=ON
cmake --build build/windows-x64 --config Release --target nxemu-fsr-vulkan-benchmark nxemu_fsr_diagnostics_tests
build/windows-x64/src/yuzu_video_core/Release/nxemu-fsr-vulkan-benchmark.exe build/fsr-validation/repeat
$env:NXEMU_FSR_VALIDATION='1'
build/windows-x64/src/yuzu_video_core/Release/nxemu-fsr-vulkan-benchmark.exe build/fsr-validation/validation
Remove-Item Env:NXEMU_FSR_VALIDATION
ctest --test-dir build/windows-x64 -C Release -R nxemu_fsr_diagnostics --output-on-failure
```

Le banc utilise les véritables shaders SPIR-V générés par nxemu, en FP32 et FP16 si supportés, sans fenêtre ni jeu. Il teste l'agrandissement, le natif, les retournements, le recadrage, les marges et l'alpha. Après 60 tours de chauffe, 240 paires A/B alternent leur ordre. Les attentes de fence et téléchargements d'image sont hors intervalle mesuré. Les images PPM, timings bruts et statistiques sont exportés. La couche Vulkan s'active seulement pour la validation, pas pour les chiffres de performance.

Des assertions vérifient un écart maximal de 1/255 hors natif, la conservation de l'alpha à 1/255 près, et l'absence de régression PSNR en natif par rapport à l'image source synthétique. Cela ne remplace pas les essais sur des jeux, d'autres GPU, les surfaces sRGB, le HDR ou l'évaluation temporelle.

## Points corrigés pendant la revue

- Le patch visait des projets Visual Studio supprimés et une ancienne UI : adaptation à CMake et aux préférences actuelles.
- Les dépendances FSR étaient placées sur la mauvaise commande CMake ; elles reconstruisent maintenant les SPIR-V lorsque les includes changent.
- RCAS lisait hors texture aux bords : coordonnées bornées, dans l'ancienne chaîne et la nouvelle.
- L'alpha EASU intermédiaire ignorait crop/flip : il utilise maintenant la même transformation source que le RGB.
- Le curseur arrondit seulement l'affichage : ouvrir un menu ne transforme plus une ancienne valeur impaire enregistrée.
- Le changement de nombre d'images du swapchain réinitialise l'index de présentation.
- Les chemins FSR FP16 exigent aussi `shaderInt16`, utilisé par le code FidelityFX.
- Correction OpenGL de la taille des uploads uniformes et du vecteur RCAS FP16 ; les résultats prioritaires sont ceux de Vulkan.

A conserve **l'architecture ancienne avec les corrections communes de sûreté/alpha**. Ce n'est pas un exécutable historique inchangé. À netteté 0 %, B désactive réellement RCAS ; A conserve l'atténuation RCAS historique. Le test hors jeu « EASU only » compare deux chemins sans RCAS équivalents, pour isoler la fusion EASU ; il ne compare pas ce nouveau 0 % avec l'ancien curseur à 200.
