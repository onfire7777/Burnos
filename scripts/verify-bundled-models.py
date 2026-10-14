"""Check bundled model hashes and first-run readiness with empty user caches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('package', type=Path)
parser.add_argument('--report', type=Path)
args = parser.parse_args()
package = args.package.resolve()
manifest = json.loads((package / 'package-manifest.json').read_text(encoding='utf-8-sig'))
models = [item for item in manifest if item['path'].startswith('models/')]
assert models, 'Package contains no voice models'
for item in models:
    path = package / item['path']
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    assert digest == item['sha256'], item['path']
with tempfile.TemporaryDirectory(prefix='Burnos bundled model test ') as tmp:
    root = Path(tmp)
    log = root / 'setup.log'
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('BURNOS_', 'MNEMOSYNE_', 'OPENROUTER_', 'PYTHON'))}
    env.update(LOCALAPPDATA=str(root / 'local'), APPDATA=str(root / 'roaming'),
               ProgramData=str(root / 'programdata'),
               PATH=os.environ['SystemRoot'] + '/System32',
               BURNOS_SETTINGS='Burnos-bundled-model-test', BURNOS_LIVE_LOG=str(log))
    start = time.monotonic()
    subprocess.run([str(package / 'Burnos.exe'), '--prepare-models'],
                   cwd=package, env=env, check=True, timeout=15)
    text = log.read_text(encoding='utf-8')
    assert 'downloading' not in text.lower(), text
    assert str(package / 'models').replace('\\', '/') in text.replace('\\', '/'), text
    assert not (root / 'local/BurnOS/models').exists(), 'Unexpected model cache created'
    assert not (root / 'programdata/BurnOS').exists(), 'Unexpected legacy cache created'
    report = dict(passed=True, model_files=len(models),
                  model_bytes=sum(item['size'] for item in models),
                  setup_seconds=round(time.monotonic() - start, 3),
                  empty_user_caches=True, download_started=False, setup_log=text)
if args.report:
    args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
