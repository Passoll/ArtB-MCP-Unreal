from __future__ import annotations

from pathlib import Path
from typing import Any

from .tool_response import error, ok


PROJECT_ROOT = Path(__file__).resolve().parents[4]
CONTENT_ROOT = PROJECT_ROOT / "Content"


def _strip_object_wrapper(value: str) -> str:
    text = value.strip()
    if "'" in text and text.endswith("'"):
        first_quote = text.find("'")
        if first_quote >= 0:
            text = text[first_quote + 1 : -1]
    return text


def _split_object_path(value: str) -> tuple[str, str]:
    text = _strip_object_wrapper(value)
    if "." in text.rsplit("/", 1)[-1]:
        package, object_name = text.rsplit(".", 1)
        return package, object_name
    return text, Path(text).name


def _plugin_content_roots() -> dict[str, Path]:
    roots: dict[str, Path] = {}
    plugins_dir = PROJECT_ROOT / "Plugins"
    if not plugins_dir.exists():
        return roots

    for descriptor in plugins_dir.rglob("*.uplugin"):
        plugin_name = descriptor.stem
        content = descriptor.parent / "Content"
        if content.exists():
            roots[plugin_name.lower()] = content
    return roots


def resolve_asset_path(asset_path: str) -> dict[str, Any]:
    package_path, object_name = _split_object_path(asset_path)
    if package_path.startswith("/Script/"):
        return ok(
            input=asset_path,
            kind="script_object",
            package_path=package_path,
            object_name=object_name,
            exists=False,
            note="Script objects do not map to project .uasset files.",
        )

    root: Path | None = None
    relative = ""
    mount = ""
    if package_path.startswith("/Game/"):
        root = CONTENT_ROOT
        relative = package_path.removeprefix("/Game/")
        mount = "/Game"
    else:
        parts = package_path.strip("/").split("/", 1)
        if len(parts) == 2:
            plugin_root = _plugin_content_roots().get(parts[0].lower())
            if plugin_root:
                root = plugin_root
                relative = parts[1]
                mount = f"/{parts[0]}"

    if not root:
        return error(
            "INVALID_PARAM",
            f"Unsupported asset mount path: {asset_path}",
            details={"supported": ["/Game", "project plugin content roots", "/Script"]},
        )

    file_path = root / f"{relative}.uasset"
    return ok(
        input=asset_path,
        kind="asset",
        mount=mount,
        package_path=package_path,
        object_name=object_name,
        file_path=str(file_path),
        exists=file_path.exists(),
    )


def asset_exists(asset_path: str) -> dict[str, Any]:
    resolved = resolve_asset_path(asset_path)
    if not resolved.get("ok"):
        return resolved
    return ok(asset_path=asset_path, exists=bool(resolved.get("exists")), resolved=resolved)


def _asset_record(file_path: Path, root: Path, mount: str) -> dict[str, Any]:
    rel = file_path.relative_to(root).with_suffix("")
    package = f"{mount}/{rel.as_posix()}"
    object_name = file_path.stem
    return {
        "asset_path": f"{package}.{object_name}",
        "package_path": package,
        "object_name": object_name,
        "file_path": str(file_path),
        "mount": mount,
    }


def _iter_asset_roots(include_plugins: bool) -> list[tuple[Path, str]]:
    roots: list[tuple[Path, str]] = []
    if CONTENT_ROOT.exists():
        roots.append((CONTENT_ROOT, "/Game"))
    if include_plugins:
        for plugin_name, content_root in sorted(_plugin_content_roots().items()):
            roots.append((content_root, f"/{plugin_name}"))
    return roots


def list_assets(
    package_path: str = "/Game",
    recursive: bool = True,
    limit: int = 100,
    include_plugins: bool = False,
) -> dict[str, Any]:
    safe_limit = max(1, min(limit, 1000))
    records: list[dict[str, Any]] = []
    normalized = package_path.rstrip("/") or "/Game"

    for root, mount in _iter_asset_roots(include_plugins):
        if normalized != mount and not normalized.startswith(f"{mount}/"):
            continue
        relative = normalized.removeprefix(mount).strip("/")
        base = root / relative if relative else root
        if not base.exists():
            continue
        pattern = "**/*.uasset" if recursive else "*.uasset"
        for file_path in base.glob(pattern):
            records.append(_asset_record(file_path, root, mount))
            if len(records) >= safe_limit:
                return ok(count=len(records), assets=records, truncated=True)

    return ok(count=len(records), assets=records, truncated=False)


def search_assets(
    query: str,
    package_path: str = "/Game",
    limit: int = 50,
    include_plugins: bool = False,
) -> dict[str, Any]:
    if not query.strip():
        return error("MISSING_PARAM", "query is required for search_assets.")

    listed = list_assets(package_path=package_path, recursive=True, limit=1000, include_plugins=include_plugins)
    if not listed.get("ok"):
        return listed
    needle = query.lower()
    matches = [
        asset
        for asset in listed.get("assets", [])
        if needle in asset["asset_path"].lower() or needle in asset["object_name"].lower()
    ]
    safe_limit = max(1, min(limit, 200))
    return ok(count=len(matches), assets=matches[:safe_limit], truncated=len(matches) > safe_limit)

