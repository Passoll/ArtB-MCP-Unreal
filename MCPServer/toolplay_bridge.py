from __future__ import annotations

import json
import socket
from typing import Any


BRIDGE_HOST = "127.0.0.1"
BRIDGE_PORT = 55557
REQUEST_TIMEOUT_SECONDS = 15.0


def call_unreal_bridge(command: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    request = {
        "command": command,
        "params": params or {},
    }
    payload = (json.dumps(request, separators=(",", ":")) + "\n").encode("utf-8")

    with socket.create_connection((BRIDGE_HOST, BRIDGE_PORT), timeout=REQUEST_TIMEOUT_SECONDS) as sock:
        sock.settimeout(REQUEST_TIMEOUT_SECONDS)
        sock.sendall(payload)

        chunks: list[bytes] = []
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
            if b"\n" in chunk:
                break

    if not chunks:
        raise RuntimeError("Unreal bridge returned no data.")

    line = b"".join(chunks).split(b"\n", 1)[0]
    response = json.loads(line.decode("utf-8"))
    if not response.get("ok"):
        raise RuntimeError(response.get("error", "Unreal bridge request failed."))

    result = response.get("result", {})
    if isinstance(result, dict) and result.get("ok") is False:
        raise RuntimeError(result.get("error", "Unreal command failed."))

    return result
