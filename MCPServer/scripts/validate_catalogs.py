from __future__ import annotations

import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CATALOG_DIR = ROOT / "catalogs"


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def validate_niagara_semantics(errors: list[str]) -> None:
    path = CATALOG_DIR / "niagara_modules.semantic.json"
    data = load_json(path)
    require(data.get("schema_version") == 2, f"{path.name}: expected schema_version 2", errors)
    require(data.get("domain") == "niagara", f"{path.name}: expected domain niagara", errors)

    entries = data.get("entries")
    require(isinstance(entries, list), f"{path.name}: entries must be an array", errors)
    if not isinstance(entries, list):
        return

    required_fields = {
        "asset",
        "name",
        "summary",
        "semantic_tags",
        "preferred_stacks",
        "inputs",
        "outputs",
        "writes",
        "side_effects",
        "input_value_kinds",
        "key_inputs",
        "common_edits",
        "pitfalls",
        "notes",
    }
    seen_keys: set[str] = set()
    blocked_fragments = ("guid", "uobject", "e0.", ".m0", ".g0")

    for index, entry in enumerate(entries):
        label = str(entry.get("name") or f"entry[{index}]") if isinstance(entry, dict) else f"entry[{index}]"
        require(isinstance(entry, dict), f"{path.name}: {label} must be an object", errors)
        if not isinstance(entry, dict):
            continue

        missing = sorted(required_fields - set(entry))
        require(not missing, f"{path.name}: {label} missing fields {missing}", errors)

        asset = str(entry.get("asset", ""))
        name = str(entry.get("name", ""))
        key = f"{asset.lower()}|{name.lower()}"
        require(key not in seen_keys, f"{path.name}: duplicate entry {label}", errors)
        seen_keys.add(key)

        require(bool(name), f"{path.name}: {label} name must not be empty", errors)
        require(bool(entry.get("summary")), f"{path.name}: {label} summary must not be empty", errors)

        for field in ("semantic_tags", "preferred_stacks", "inputs", "outputs", "writes", "side_effects", "input_value_kinds", "key_inputs", "common_edits", "pitfalls"):
            require(isinstance(entry.get(field), list), f"{path.name}: {label}.{field} must be an array", errors)

        serialized = json.dumps(entry, ensure_ascii=False).lower()
        for fragment in blocked_fragments:
            require(fragment not in serialized, f"{path.name}: {label} contains blocked transient/internal fragment '{fragment}'", errors)


def validate_catalog_domains(errors: list[str]) -> None:
    path = CATALOG_DIR / "catalog_domains.json"
    data = load_json(path)
    require(data.get("schema_version") == 1, f"{path.name}: expected schema_version 1", errors)
    domains = data.get("domains")
    require(isinstance(domains, list), f"{path.name}: domains must be an array", errors)
    if not isinstance(domains, list):
        return
    for domain in domains:
        if not isinstance(domain, dict):
            errors.append(f"{path.name}: domain entries must be objects")
            continue
        require(bool(domain.get("domain")), f"{path.name}: domain entry missing domain", errors)
        catalog = str(domain.get("catalog", ""))
        status = str(domain.get("status", "active"))
        if catalog and status == "active":
            require((CATALOG_DIR / catalog).exists(), f"{path.name}: referenced catalog does not exist: {catalog}", errors)


def main() -> int:
    errors: list[str] = []
    validate_catalog_domains(errors)
    validate_niagara_semantics(errors)

    if errors:
        print("catalog validation failed")
        for error in errors:
            print(f"- {error}")
        return 1

    print("catalog validation ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
