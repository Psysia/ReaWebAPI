"""Package corresponding modified source; keep upstream mirror tags immutable."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.version import source_version

revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
release = Path('release')
source = release / 'ReaWebAPI-source.zip'
subprocess.run(['git', 'archive', '--format=zip', f'--prefix=ReaWebAPI-{revision}/',
                '-o', str(source), revision], check=True)
manifest = {
    'schema': 'PsyReaSFX-ReaWebAPI-Mirror-v1', 'repository': 'Psysia/ReaWebAPI',
    'upstream': 'zaibuyidao/ReaWebAPI', 'modified': True,
    'upstream_commit': 'f3d83ee333a5df0f1242b20efc706ce5a893dabf',
    'patches': ['windows-dock-top-level-redraw'],
    'tag': 'v' + source_version(), 'commit': revision,
    'contracts': {'protocol': 1, 'runtime': 2, 'web': 1, 'max_message_bytes': 1048576},
    'files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in release.iterdir() if p.is_file() and p.suffix in ('.dll', '.zip')},
}
(release / 'mirror-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print('Packaged modified source and manifest for', revision)
