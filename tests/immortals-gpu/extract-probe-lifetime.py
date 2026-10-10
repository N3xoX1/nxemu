from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/capture-analysis'
baseline_ref='fa29fce321314eb99b49f083fd5601818450a6e3'
for variant,source in [('baseline',subprocess.check_output(['git','show',f'{baseline_ref}:src/nxemu/startup_checks.cpp'],cwd=root,text=True)),('fixed',(root/'src/nxemu/startup_checks.cpp').read_text())]:
    start=source.index('void PopulateVulkanRecords(')
    end=source.index('#elif defined(__APPLE__)',start)
    function=source[start:end].replace('#ifdef _WIN32','').replace('GetProcAddress(', 'FakeGetProcAddress(').replace('FreeLibrary(', 'FakeFreeLibrary(')+'}\n'
    (out/f'probe-lifetime-{variant}.inc').write_text(function)
