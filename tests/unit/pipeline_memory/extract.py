# Execute the production Windows method with a deterministic VirtualAlloc seam.
from pathlib import Path
import sys
root, output = map(Path, sys.argv[1:])
text = (root / 'src/nxemu-cpu/dynarmic/backend/x64/block_of_code.cpp').read_text()
start = text.index('void BlockOfCode::EnsureMemoryCommitted(')
end = text.index('\nHaltReason BlockOfCode::RunCode', start)
output.write_text(text[start:end])
