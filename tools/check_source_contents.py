"""Reject accidental original game files, generated main-game code and local state."""
from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[1]
paths=subprocess.check_output(['git','-C',str(root),'ls-files','-z']).decode().split('\0')
errors=[]
for name in filter(None,paths):
    p=Path(name)
    if p.parts[0] in ('game','assets','generated','out','build','pgo','saves','logs','profiles'):
        errors.append(name)
    if p.suffix.lower() in ('.xex','.xexp','.iso','.big','.pdb','.idb','.i64','.profdata','.profraw','.exe','.dll'):
        errors.append(name)
    if '.xex.c' in name or '.bak' in p.name or '__pycache__' in p.parts:
        errors.append(name)
# Embedded enhanced fonts and src/zd_debug are intentional inclusions.
if errors:
    print('Unexpected files in the source index:');print('\n'.join(sorted(set(errors))));sys.exit(1)
print('Source content check passed.')
