"""Exercise packaged Burnos + its owned memory process with an isolated profile.

No microphone, personal store, API key, or model API call is used.
Usage: python scripts/verify-windows-package.py PATH_TO_PACKAGE [--report FILE]
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    app = args.package.resolve()
    memory = app / 'mnemosyne/mneme-mcp.exe'
    assert memory.is_file(), memory
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    url = f'http://127.0.0.1:{port}'
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    counter = 0

    def rpc(method, params):
        nonlocal counter
        counter += 1
        request = urllib.request.Request(url + '/mcp', method='POST',
            data=json.dumps(dict(jsonrpc='2.0', id=counter, method=method, params=params)).encode(),
            headers={'Content-Type': 'application/json',
                     'Accept': 'application/json, text/event-stream',
                     'MCP-Protocol-Version': '2024-11-05'})
        with opener.open(request, timeout=15) as response:
            text = response.read().decode()
        body = json.loads(next(line[6:] for line in text.splitlines() if line.startswith('data: '))) if text.startswith('event:') else json.loads(text)
        assert 'error' not in body, body
        result = body['result']
        assert not result.get('isError'), result
        return result

    def wait_up(process):
        end = time.monotonic() + 30
        while time.monotonic() < end:
            assert process.poll() is None, 'Burnos exited before memory became healthy'
            try:
                with opener.open(url + '/healthz', timeout=1) as response:
                    return json.load(response)
            except OSError:
                time.sleep(.2)
        raise AssertionError('Memory startup timed out')

    with tempfile.TemporaryDirectory(prefix='Burnos package test ') as tmp:
        root = Path(tmp)
        store = root / 'local/BurnOS/memory'
        config = root / 'mcp.json'
        config.write_text(json.dumps({'mcpServers': {'memory': {
            'url': url + '/mcp', 'command': str(memory),
            'args': ['--sdk-streamable-http', '--http-host', '127.0.0.1',
                     '--http-port', str(port), '--backend', 'sqlite', '--store', str(store)],
            'cwd': str(memory.parent)}}}))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('BURNOS_', 'MNEMOSYNE_', 'OPENROUTER_', 'PYTHON'))}
        env.update(PATH=os.environ['SystemRoot'] + '/System32',
            APPDATA=str(root / 'roaming'), LOCALAPPDATA=str(root / 'local'),
            BURNOS_SETTINGS='Burnos-package-test-' + root.name,
            BURNOS_MCP_CONFIG=str(config), BURNOS_LIVE_LOG=str(root / 'native.log'))
        fact = 'Fictional Aurora workshop budget is 600 dollars and the palette is cobalt violet.'
        health = None
        cid = None
        for phase in (1, 2):
            script = root / f'phase{phase}.txt'
            script.write_text('@wait 5\n@context What is the Aurora workshop budget and palette?\n')
            output = root / f'phase{phase}.json'
            proc = subprocess.Popen([str(app / 'Burnos.exe'), '--talk-probe', str(script), str(output)], env=env, cwd=app)
            try:
                health = wait_up(proc)
                rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                    'clientInfo': {'name': 'burnos-package-test', 'version': '1'}})
                if phase == 1:
                    result = rpc('tools/call', {'name': 'capture', 'arguments': {
                        'tenant_id': 'burnos', 'user_id': 'owner', 'branch': 'main',
                        'actor': 'assistant', 'trust_tier': 3, 'source_type': 'fictional-test',
                        'content': fact}})
                    cid = result['structuredContent']['cid']
                result = rpc('tools/call', {'name': 'search', 'arguments': {
                    'tenant_id': 'burnos', 'user_id': 'owner', 'branch': 'main',
                    'query': 'Aurora workshop budget palette'}})
                assert any(cid in hit.get('provenance', []) for hit in result['structuredContent']['hits']), result
                assert proc.wait(timeout=45) == 0
                native = json.loads(output.read_text())
                assert native['memory'] is True, native
                assert '600' in json.dumps(native) and 'cobalt' in json.dumps(native), native
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
            # The application job object must close its child, not orphan a server.
            time.sleep(.5)
            with socket.socket() as sock:
                assert sock.connect_ex(('127.0.0.1', port)) != 0, 'Memory child survived Burnos exit'
        report = {'passed': True, 'memory_build': health, 'checks': [
            'packaged executable launched with Windows-only PATH and isolated profile',
            'Burnos starts its bundled memory process', 'native MCP 2024-11-05 handshake',
            'capture and search preserve record identity',
            'native context includes stored fact before and after full process restart',
            'owned memory process exits with Burnos', 'no model API or microphone used']}
        if args.report:
            args.report.write_text(json.dumps(report, indent=2))
        print(json.dumps({'passed': True, 'checks': report['checks']}, indent=2))


if __name__ == '__main__':
    main()
