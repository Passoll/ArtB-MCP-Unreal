# UE MCP Alignment

This note tracks how ToolPlayMCP should evolve relative to Unreal Engine 5.8's experimental `ModelContextProtocol` plugin.

## What UE MCP Does Better

- `tools/list` and `tools/call` are native protocol concepts, not category tools with an `action` switch.
- Tool registration is runtime-managed through a shared registry and can broadcast `notifications/tools/list_changed`.
- Deferred loading registers only `list_toolsets`, `describe_toolset`, and `load_toolset` first, then adds real tools on demand.
- Tools support async execution, cancellation, progress heartbeats, and active MCP sessions.
- `resources/list` and `resources/read` expose reusable context without turning every document/catalog into a tool call.
- Tool schemas and some metadata are generated from Unreal reflection / ToolsetRegistry instead of duplicated by hand.
- Server-level behavior follows MCP HTTP/SSE expectations, including initialize, session ids, protocol-version checks, and localhost origin checks.

## What ToolPlayMCP Currently Does Better For This Project

- Domain services are focused on Material, Blueprint, and Niagara graph understanding/editing.
- Compact graph exports hide GUIDs and transient UObject details while keeping alias maps in editor memory.
- Mutation tools are asset-aware and follow edit/re-export/validate workflows.
- Niagara module semantics can be cached and curated instead of repeatedly dumping large native graphs into model context.
- The Python MCP layer is easy to ship to Codex today and keeps the UE plugin independent from UE5.8 experimental APIs.

## Recommended Adjustments

1. Keep the category tools short-term for compatibility, but make `list_tools`, `list_toolsets`, and `describe_toolset` complete enough for generated UI and agents.
2. Add a `load_toolset` action or equivalent lazy-exposure flag before the tool list grows much larger.
3. Add MCP resources for long-lived context:
   - `toolplay://usage/material_patch`
   - `toolplay://usage/niagara_system`
   - `toolplay://catalog/niagara_modules.semantic`
   - `toolplay://catalog/material_nodes`
4. Add catalog governance and validation before expanding semantic databases.
5. Keep bridge-backed input schemas derived from the C++ bridge registry; Python should only own local action schemas, public aliases, and output schemas until a shared manifest exists.
6. Add output schemas for high-value tools once response shapes stabilize.
7. Add notifications or a lightweight version field so clients know when tool/catalog schemas changed.
8. Keep graph semantics as IR:
   - `schema`: editable fields and constraints.
   - `topology`: stack/graph/node/pin relationships.
   - `data`: current values and overrides.
   - `semantics`: curated meaning and pitfalls.

## Do Not Copy Blindly

Do not replace ToolPlayMCP with UE MCP wholesale yet. UE MCP gives a better generic protocol host, but our core value is the domain IR for Material, Blueprint, Niagara, and future graph edits.

Prefer this path:

1. Stabilize domain IR and catalog rules in ToolPlayMCP.
2. Keep the Python bridge for Codex users.
3. Later, add an optional UE5.8 native MCP adapter that registers the same domain actions/resources through Unreal's official MCP module when available.
