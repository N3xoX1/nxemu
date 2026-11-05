"""Create a fresh, isolated FSR A/B package from the current Windows build."""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
package = Path(sys.argv[1]).resolve()
if package.exists():
    raise SystemExit("Refusing to overwrite an existing comparison package")
runtime = repo / "bin/x64/Release"
shared = Path(sys.argv[2]).resolve()
game = Path(sys.argv[3]).resolve()
if not game.is_file():
    raise SystemExit("Game file is missing")
for directory in ("nand", "sdmc", "load", "amiibo", "tas"):
    if not (shared / directory).is_dir():
        raise SystemExit(f"Missing shared data: {shared / directory}")
package.mkdir(parents=True)
for name in ("nxemu.exe", "SDL2.dll"):
    shutil.copy2(runtime / name, package / name)
shutil.copytree(runtime / "ui", package / "ui")
shutil.copytree(runtime / "modules", package / "modules")
for directory in ("config", "cache", "shader", "log", "screenshots", "play_time"):
    (package / "user" / directory).mkdir(parents=True)
(package / "user/portable.txt").touch()
config = json.loads((repo / "config/NxEmu.config").read_text(encoding="utf-8"))
config.setdefault("Core", {})["ModuleDirectory-x64"] = "./modules"
ui = config.setdefault("UI", {})
ui.setdefault("Language", {})["Directory"] = "./ui/lang"
ui["EnableDiscordPresence"] = False
ui["StartGamesInFullscreen"] = False
ui["StartGamesWithUiHidden"] = False
ui["ShowPerformance"] = True
ui["ShowEmulationControls"] = True
ui["RecentFiles"] = [str(game)]
ui["StatusOverrides"] = {"nxvideo:ScalingFilter": 5, "nxvideo:ResolutionSetup": 4,
                         "nxvideo:FSPSharpness": 25, "nxos:DockedMode": 1}
video = config.setdefault("nxemu-video", {}).setdefault("video", {})
video.update(renderer_backend="Vulkan", vsync_mode="Immediate", resolution_setup="Res2X",
             scaling_filter="Fsr", fsr_sharpening_slider=25, fsr_legacy_benchmark=False)
config_path = package / "user/config/NxEmu.config"
config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
for directory in ("nand", "sdmc", "load", "amiibo", "tas"):
    # mklink /J is one explicit, nonrecursive creation. No shell enumeration or deletion.
    subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(package / "user" / directory),
                    str(shared / directory)], check=True, capture_output=True)
launcher = r'''$ErrorActionPreference = 'Stop'
$fsrLaunch = New-Object System.Diagnostics.ProcessStartInfo
$fsrLaunch.FileName = Join-Path $PSScriptRoot 'nxemu.exe'
$fsrLaunch.WorkingDirectory = $PSScriptRoot
$fsrLaunch.UseShellExecute = $false
foreach ($fsrKey in @($fsrLaunch.EnvironmentVariables.Keys)) {
    if ([string]$fsrKey -like 'NXEMU_*' -or [string]$fsrKey -like 'GFXRECON_*' -or
        [string]$fsrKey -in @('VK_INSTANCE_LAYERS', 'VK_LAYER_SETTINGS_PATH', 'VK_LAYER_VALIDATE_SYNC')) {
        $fsrLaunch.EnvironmentVariables.Remove([string]$fsrKey)
    }
}
$fsrCapture = Join-Path $PSScriptRoot ('user/log/fsr-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.csv')
$fsrLaunch.EnvironmentVariables['NXEMU_FSR_METRICS'] = $fsrCapture
$fsrLaunch.EnvironmentVariables['NXEMU_FSR_LEGACY'] = '0'
'''
launcher += "$fsrLaunch.Arguments = '" + ('"'+str(game)+'"').replace("'", "''") + "'\n"
launcher += "$fsrProcess = [System.Diagnostics.Process]::Start($fsrLaunch)\n$fsrProcess.Id | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'user/log/fsr-process-id.txt')\n"
(package / "Lancer-FSR-AB.ps1").write_text(launcher, encoding="utf-8-sig")
(package / "Tester-FSR-AB.cmd").write_text('@echo off\npowershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0Lancer-FSR-AB.ps1"\n', encoding="ascii")
(package / "LISEZ-MOI.txt").write_text("""Test FSR Vulkan A/B — Mario Odyssey

Lancer Tester-FSR-AB.cmd. Les mesures CPU/GPU sont automatiques dans user/log/fsr-*.csv.
Le jeu démarre avec Vulkan, VSync désactivée, FSR, netteté 88 % et rendu 2x (2160p nominal en mode docké).
Désactiver la limite de vitesse (Ctrl+U / contrôle de vitesse) : vérifier « Unlocked ».
Pour une sortie 4K, passer en plein écran sur l'écran 4K (F11). Le CSV indique les dimensions réellement utilisées.

Dans le menu FSR de la barre d'état :
  A - FSR original : ancienne chaîne EASU + RCAS + présentation.
  B - FSR optimized : nouvelle chaîne, fusion des passes et raccourci en résolution native.
La barre indique FSR [A] ou FSR [B]. Pas besoin de relancer le jeu.

Chauffer la scène et les shaders 60 secondes. Garder Mario et la caméra immobiles.
Faire A 20 s, B 20 s, A 20 s, B 20 s. Fermer le menu pendant les mesures.
Les 3 premières secondes et la dernière seconde de chaque phase sont écartées à l'analyse.
Un à-coup au changement de version correspond à la recréation des ressources, pas au coût permanent du FSR.
La qualité visuelle se compare à la même netteté : ne pas déplacer le curseur pendant la comparaison.

A conserve l'architecture originale, avec les corrections communes des bords RCAS et de l'alpha recadré.
La fusion vise la même image en agrandissement. Le raccourci natif change l'image en évitant EASU ; il est à examiner séparément.
Un gain de 20 % sur un filtre de 0,3 ms ne signifie pas +20 % de FPS du jeu.

Ce paquet a ses propres réglages, journaux et caches. Les dossiers de données de jeu existants sont liés ; les sauvegardes du jeu restent partagées.
""", encoding="utf-8-sig")
manifest = {"source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
            "dirty_source": True, "runtime": {str(p.relative_to(package)): hashlib.sha256(p.read_bytes()).hexdigest()
                                              for p in package.rglob("*") if p.suffix.lower() in (".exe", ".dll")}}
(package / "BUILD-INFO.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print(package)
