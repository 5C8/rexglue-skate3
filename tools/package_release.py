"""Package only runtime files and documentation, never a whole build folder."""
import argparse, hashlib, json, zipfile
from pathlib import Path

root=Path(__file__).resolve().parents[1]
project='rexglue-skate3'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',type=Path)
parser.add_argument('--install-dir',type=Path)
parser.add_argument('--output',required=True,type=Path)
args=parser.parse_args()
files={}
if project=='rexglue-skate3':
    if not args.install_dir:parser.error('--install-dir is required for the SDK')
    for directory in ('bin','include','lib','cmake','share','licenses'):
        for p in (args.install_dir/directory).rglob('*'):
            if not p.is_file() or p.suffix in ('.pdb','.ilk','.exp') or '.bak' in p.name:continue
            if directory=='bin' and p.name not in ('rexglue.exe','rexruntime.dll','TracyClient.dll'):continue
            files[p.relative_to(args.install_dir).as_posix()]=p
    for name in ('bin/rexglue.exe','bin/rexruntime.dll','lib/rexruntime.lib'):
        if name not in files:parser.error('SDK file missing: '+name)
else:
    if not args.build_dir:parser.error('--build-dir is required for a game')
    exe='skate3.exe' if project=='skate3recomp' else 'sk83_na_m.exe'
    required=[exe,'rexruntime.dll']
    if project=='sk83_na_m':required+=['sk83_na_m_EAWebkit.dll']
    optional=['TracyClient.dll','openvr_api.dll','mimalloc.dll','mimalloc-redirect.dll']
    for name in required+optional:
        p=args.build_dir/name
        if p.is_file():files[name]=p
        elif name in required:parser.error('Runtime file missing: '+name)
    if 'mimalloc.dll' in files and 'mimalloc-redirect.dll' not in files:
        parser.error('mimalloc redirect DLL must be included with mimalloc')
for directory in ('docs','licenses'):
    for p in (root/directory).rglob('*'):
        if p.is_file():files[p.relative_to(root).as_posix()]=p
for name in ('README.md','LICENSE','THIRD_PARTY_NOTICES.md'):
    if (root/name).is_file():files[name]=root/name
if project=='sk83_na_m':files['docs/ZD_FEATURES.md']=root/'src/zd_debug/README.md'
manifest={name:hashlib.sha256(p.read_bytes()).hexdigest() for name,p in sorted(files.items())}
args.output.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(args.output,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for name,p in sorted(files.items()):z.write(p,name)
    z.writestr('SHA256SUMS.txt',''.join(digest+'  '+name+'\n' for name,digest in manifest.items()))
print(json.dumps({'archive':args.output.name,'files':len(files),'bytes':args.output.stat().st_size,'sha256':hashlib.sha256(args.output.read_bytes()).hexdigest()}))
