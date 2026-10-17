"""Download pinned source archives and collect upstream redistribution notices.

Run in the release build's Python environment. Outputs contain only public
upstream archives and license files, never a user's application state.
"""
import argparse
import hashlib
import importlib.metadata as md
import json
from pathlib import Path
import re
import sys
import tarfile
import urllib.request
import zipfile


def main():
    p = argparse.ArgumentParser()
    p.add_argument('output', type=Path)
    p.add_argument('--sherpa-source', type=Path, required=True)
    args = p.parse_args()
    archives = args.output / 'sources'
    notices = args.output / 'licenses'
    archives.mkdir(parents=True, exist_ok=True)
    notices.mkdir(parents=True, exist_ok=True)
    manifest = []

    def fetch(name, url, expected=None):
        dest = archives / name
        if not dest.exists():
            temp = dest.with_suffix(dest.suffix + '.part')
            with urllib.request.urlopen(url, timeout=120) as response, temp.open('wb') as out:
                while chunk := response.read(1024 * 1024):
                    out.write(chunk)
            temp.replace(dest)
        sha = hashlib.sha256(dest.read_bytes()).hexdigest()
        if expected and sha != expected:
            raise ValueError(f'SHA256 mismatch: {name}')
        manifest.append(dict(file=name, url=url, sha256=sha))
        print('Source:', name, flush=True)

    for name, url in {
        'qtbase-6.11.2.tar.xz': 'https://download.qt.io/archive/qt/6.11/6.11.2/submodules/qtbase-everywhere-src-6.11.2.tar.xz',
        'qtmultimedia-6.11.2.tar.xz': 'https://download.qt.io/archive/qt/6.11/6.11.2/submodules/qtmultimedia-everywhere-src-6.11.2.tar.xz',
        'whisper.cpp-1.9.5.tar.gz': 'https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.5.tar.gz',
        'sherpa-onnx-1.13.8.tar.gz': 'https://github.com/k2-fsa/sherpa-onnx/archive/refs/tags/v1.13.8.tar.gz',
        'onnxruntime-1.28.2.tar.gz': 'https://github.com/microsoft/onnxruntime/archive/refs/tags/v1.28.2.tar.gz',
        'mnemosyne-1.4.1.tar.gz': 'https://github.com/onfire7777/Mnemosyne/archive/edddea3ee18b0bb9d83d4a5405afee6b6a2425a6.tar.gz',
        'Python-3.12.14.tar.xz': 'https://www.python.org/ftp/python/3.12.14/Python-3.12.14.tar.xz',
    }.items():
        fetch(name, url)
    for component in ('kaldi-native-fbank', 'kaldi-decoder', 'simple-sentencepiece',
                      'portaudio', 'json', 'espeak-ng-for-piper', 'piper-phonemize',
                      'hclust-cpp', 'websocketpp', 'asio'):
        text = (args.sherpa_source / 'cmake' / (component + '.cmake')).read_text()
        url = re.search(r'set\([^\s]+_URL\s+"([^"]+)"', text)[1].replace('http://', 'https://')
        sha = re.search(r'SHA256=([0-9a-f]{64})', text)[1]
        suffix = '.zip' if url.endswith('.zip') else '.tar.gz'
        fetch(component + suffix, url, sha)
    for dist in sorted(md.distributions(), key=lambda d: d.metadata['Name'].lower()):
        name, version = dist.metadata['Name'], dist.version
        if name.lower() == 'mnemosyne-memory':
            continue
        with urllib.request.urlopen(f'https://pypi.org/pypi/{name}/{version}/json', timeout=60) as response:
            data = json.load(response)
        source = next((x for x in data['urls'] if x['packagetype'] == 'sdist'), None)
        if source:
            fetch(source['filename'], source['url'], source['digests']['sha256'])
        # Some Windows-only packages have no sdist on PyPI; preserve notices from the installed wheel.
        for file in dist.files or []:
            if re.search(r'license|copying|copyright|notice', str(file), re.I):
                original = dist.locate_file(file)
                if original.is_file() and original.stat().st_size < 2_000_000:
                    target = notices / ('python-' + name) / str(file).replace('..', '_')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(original.read_bytes())
    (archives / 'source-manifest.json').write_text(json.dumps(manifest, indent=2))
    # Read archive members rather than extracting executable/source paths.
    for file in archives.iterdir():
        if file.suffix not in ('.zip', '.gz', '.xz'):
            continue
        def keep(name, data):
            if not re.search(r'(^|/)(licen[cs]e[^/]*|copying[^/]*|copyright[^/]*|notice[^/]*|qt_attribution[^/]*)$', name, re.I):
                return
            parts = Path(name).parts
            if '..' in parts or Path(name).is_absolute() or len(data) > 2_000_000:
                return
            target = notices / file.name / Path(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        if file.suffix == '.zip':
            with zipfile.ZipFile(file) as z:
                for item in z.infolist():
                    if not item.is_dir() and item.file_size < 2_000_000:
                        if re.search('licen[cs]e|copying|copyright|notice|qt_attribution', item.filename, re.I):
                            keep(item.filename, z.read(item))
        else:
            with tarfile.open(file) as t:
                for item in t:
                    if item.isfile() and item.size < 2_000_000 and re.search('licen[cs]e|copying|copyright|notice|qt_attribution', item.name, re.I):
                        keep(item.name, t.extractfile(item).read())
    for name in ('COPYING3', 'COPYING.RUNTIME'):
        url = 'https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-13.1.0/' + name
        with urllib.request.urlopen(url, timeout=60) as response:
            (notices / ('GCC-' + name + '.txt')).write_bytes(response.read())
    print('Collected', len(manifest), 'source archives and upstream notices.')


if __name__ == '__main__':
    main()
