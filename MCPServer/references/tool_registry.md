# Tool Registry

ToolPlayMCP keeps the public MCP surface small:

```text
system(action, params)
asset(action, params)
material(action, params)
blueprint(action, params)
niagara(action, params)
```

New capabilities should normally be added as domain actions, not as new top-level MCP tools.

## Discovery

Use these system actions:

- `system(action="list_tools")`: returns public domain actions with input schemas plus the underlying UE bridge registry.
- `system(action="list_toolsets")`: returns available domains/toolsets.
- `system(action="describe_toolset", params={"domain":"material"})`: returns actions for one domain and bridge command schemas for that domain.
- `system(action="get_usage", params={"topic":"..."})`: returns longer workflow notes.

Use `system(action="get_usage", params={"topic":"catalog_governance"})` before editing semantic catalogs or node/module databases.

Public action metadata is the contract MCP clients should use:

```json
{
  "description": "Set a scalar, vector, or texture parameter...",
  "input_schema": {
    "type": "object",
    "properties": {
      "asset_path": {"type": "string"},
      "parameter": {"type": "string"},
      "type": {"type": "string", "enum": ["scalar", "vector", "texture"]},
      "value": {}
    },
    "required": ["asset_path", "parameter", "type", "value"]
  },
  "usage_topic": "material_patch",
  "bridge_command": "set_material_parameter"
}
```

`registry_source` explains where the public action metadata came from:

- `cpp_bridge`: bridge-backed action whose description/input schema were derived from `FToolPlayMCPToolRegistry`.
- `python_local`: local Python action with no UE bridge command, such as catalog search or usage document reads.
- `python_fallback`: bridge-backed action where the editor bridge was unavailable or the C++ registry did not include the command, so Python fallback metadata was used.

The UE bridge registry stores lower-level command metadata:

```json
{
  "name": "set_material_parameter",
  "domain": "material",
  "description": "Set a scalar, vector, or texture parameter...",
  "params_example": {"asset_path": "..."},
  "input_schema": {},
  "usage_topic": "material_patch"
}
```

## Extension Rules

- Keep Python MCP tools domain-based.
- For UE-backed actions, add or update command metadata in `FToolPlayMCPToolRegistry`; this is the input-schema source of truth.
- Add bridge execution in `FToolPlayMCPBridgeServer` or a future domain command handler.
- Add Python action mapping in `tools/action_tools.py`, but do not duplicate UE bridge input schemas unless a fallback is needed.
- For Python-local actions, keep the local handler and schema in `tools/action_tools.py`; use catalog JSON only for searchable domain data, not for executable action handlers.
- Add or update output schemas in Python for public action result contracts, because C++ bridge registry currently only owns input schemas.
- Add usage docs when behavior is not obvious from schema.
- For catalog/database changes, follow `references/catalog_governance.md` and run `scripts/validate_catalogs.py`.

## Why This Exists

The registry prevents tool sprawl from becoming invisible hardcoded state. Debug UI, MCP discovery, and future generated parameter forms should all read the same metadata instead of maintaining separate tool lists.

ToolPlayMCP currently uses a layered registry:

- C++ bridge registry owns UE command metadata and input schemas.
- Python local registry owns local-only actions such as usage docs, asset filesystem helpers, Material node catalog search, and Niagara semantic catalog search.
- Python also owns public action aliases and output schemas, because these are MCP-facing conveniences layered over lower-level bridge commands.

## UE MCP Alignment

UE5.8's experimental MCP plugin exposes native `tools/list`, `tools/call`, `resources/list`, `resources/read`, async tool execution, deferred toolset loading, and `notifications/tools/list_changed`.

ToolPlayMCP currently keeps category tools for Codex compatibility, but should borrow these ideas over time:

- Add lazy loading when domain action lists become too large.
- Expose long usage docs and catalogs as MCP resources instead of only `get_usage` tool results.
- Keep a single source of truth for action schemas to avoid Python/C++ drift.
- Preserve ToolPlayMCP's domain IR for Material, Blueprint, and Niagara instead of exposing raw Unreal objects.

See `references/uemcp_alignment.md` for the living checklist.
