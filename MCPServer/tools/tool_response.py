from __future__ import annotations

from collections.abc import Callable
from typing import Any


ActionHandler = Callable[[dict[str, Any]], dict[str, Any]]


def ok(**fields: Any) -> dict[str, Any]:
    return {"ok": True, **fields}


def error(
    code: str,
    message: str,
    *,
    details: dict[str, Any] | None = None,
    retryable: bool = False,
) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "ok": False,
        "error": {
            "code": code,
            "message": message,
            "retryable": retryable,
        },
    }
    if details:
        payload["error"]["details"] = details
    return payload


def require(params: dict[str, Any], *names: str) -> dict[str, Any] | None:
    missing = [name for name in names if name not in params or params[name] in (None, "")]
    if missing:
        return error(
            "MISSING_PARAM",
            f"Missing required parameter(s): {', '.join(missing)}",
            details={"missing": missing},
        )
    return None


def dispatch_action(
    domain: str,
    action: str,
    params: dict[str, Any],
    handlers: dict[str, ActionHandler],
) -> dict[str, Any]:
    normalized = action.strip().lower()
    handler = handlers.get(normalized)
    if not handler:
        return error(
            "INVALID_ACTION",
            f"Unknown {domain} action: {action}",
            details={"allowed_actions": sorted(handlers)},
        )

    try:
        return handler(params)
    except TimeoutError as exc:
        return error("BRIDGE_TIMEOUT", str(exc), retryable=True)
    except ConnectionError as exc:
        return error("EDITOR_NOT_CONNECTED", str(exc), retryable=True)
    except RuntimeError as exc:
        return error(
            str(getattr(exc, "code", "UNREAL_EXCEPTION")),
            str(exc),
            details=getattr(exc, "details", None),
            retryable=bool(getattr(exc, "retryable", False)),
        )
    except Exception as exc:
        return error("UNREAL_EXCEPTION", str(exc))
