from __future__ import annotations

import json
import socket
from typing import Any


BRIDGE_HOST = "127.0.0.1"
BRIDGE_PORT = 55557
REQUEST_TIMEOUT_SECONDS = 15.0


class UnrealBridgeError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        code: str = "UNREAL_BRIDGE_ERROR",
        details: dict[str, Any] | None = None,
        retryable: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = details or {}
        self.retryable = retryable


def _raise_bridge_error(command: str, raw_error: Any, *, code: str = "UNREAL_COMMAND_FAILED") -> None:
    if isinstance(raw_error, dict):
        message = str(raw_error.get("message") or raw_error.get("error") or "Unreal bridge request failed.")
        error_code = str(raw_error.get("code") or code)
        retryable = bool(raw_error.get("retryable", False))
        details = raw_error.get("details") if isinstance(raw_error.get("details"), dict) else {}
    else:
        message = str(raw_error or "Unreal bridge request failed.")
        error_code = code
        retryable = "Unknown command" in message or "old DLL" in message
        details = {}

    details = {**details, "command": command}
    raise UnrealBridgeError(message, code=error_code, details=details, retryable=retryable)


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
        raise UnrealBridgeError(
            "Unreal bridge returned no data.",
            code="EMPTY_BRIDGE_RESPONSE",
            details={"command": command},
            retryable=True,
        )

    line = b"".join(chunks).split(b"\n", 1)[0]
    response = json.loads(line.decode("utf-8"))
    if not response.get("ok"):
        _raise_bridge_error(command, response.get("error", "Unreal bridge request failed."))

    result = response.get("result", {})
    if isinstance(result, dict) and result.get("ok") is False:
        _raise_bridge_error(command, result.get("error", "Unreal command failed."))

    return result
