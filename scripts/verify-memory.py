"""Check Mnemosyne persistence in an isolated temporary store, never a user store."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory

from starlette.testclient import TestClient
from mnemosyne.mcp_server import build_sdk_streamable_http_app


def main():
    headers = {
        "Accept": "application/json, text/event-stream",
        "MCP-Protocol-Version": "2025-11-25",
    }
    request_id = 0

    def rpc(client, method, params):
        nonlocal request_id
        request_id += 1
        response = client.post(
            "/mcp", headers=headers,
            json={"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
        )
        response.raise_for_status()
        body = (json.loads(next(line[6:] for line in response.text.splitlines()
                                if line.startswith("data: ")))
                if response.text.startswith("event:") else response.json())
        assert "error" not in body, body
        assert not body["result"].get("isError"), body
        return body["result"]

    def initialize(client):
        result = rpc(client, "initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "burnos-memory-check", "version": "1.0"},
        })
        assert result["protocolVersion"] == "2025-11-25", result

    def call(client, name, arguments):
        return rpc(client, "tools/call", {"name": name, "arguments": {
            "tenant_id": "burnos-isolated-check", **arguments,
        }})

    with TemporaryDirectory(prefix="burnos-memory-check-") as temporary:
        store = Path(temporary)
        with TestClient(build_sdk_streamable_http_app(backend="sqlite", store_path=store)) as client:
            initialize(client)
            captured = call(client, "capture", {
                "user_id": "demo", "actor": "assistant", "trust_tier": 3,
                "source_type": "fictional-demo",
                "content": "Fictional Aurora beginner workshop: budget 600 dollars, blue and violet palette.",
            })
            cid = captured["structuredContent"]["cid"]
        with TestClient(build_sdk_streamable_http_app(backend="sqlite", store_path=store)) as client:
            initialize(client)
            result = call(client, "search", {"query": "Aurora workshop budget palette"})
            hits = result["structuredContent"]["hits"]
            assert any(cid in hit.get("provenance", []) for hit in hits), result
    print("PASS: MCP 2025-11-25; fictional record retrieved after SQLite store reopen.")


if __name__ == "__main__":
    main()
