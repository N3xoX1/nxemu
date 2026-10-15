"""Exercise the production condvar handoff without starting an emulated game."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[3]
out=root/'build/performance-review/abandoned-locks'
out.mkdir(parents=True,exist_ok=True)
path='src/nxemu-os/core/hle/kernel/k_condition_variable.cpp'
def function(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
current=(root/path).read_text()
previous=subprocess.check_output(['git','show','9fc41c5b:'+path],cwd=root,text=True)
pieces={
    'signal-current.inc':function(current,'void KConditionVariable::SignalImpl('),
    'signal-reference.inc':function(previous,'void KConditionVariable::SignalImpl(').replace('::SignalImpl(', '::SignalBefore('),
    'wait-address.inc':function(current,'Result KConditionVariable::WaitForAddress('),
}
for name,text in pieces.items():(out/name).write_text(text)
(out/'source.json').write_text(json.dumps({name:hashlib.sha256(text.encode()).hexdigest() for name,text in pieces.items()},indent=2))
print('Extracted actual current and 9fc41c5b condition-variable functions')
