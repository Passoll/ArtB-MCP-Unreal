# ToolPlayMCP MCP Server

This is a thin Python MCP layer for the ToolPlayMCP Unreal plugin.

The Unreal plugin owns graph inspection and mutation logic. This server only translates MCP tool calls into newline-delimited JSON requests sent to the local Unreal bridge at `127.0.0.1:55557`.

## Tools

The MCP server exposes five category tools. Each uses the same signature:

```text
tool(action: str, params: dict)
```

- `system`: `ping`, `list_tools`, `list_toolsets`, `describe_toolset`, `get_usage`, `get_selected_graph_nodes`.
- `asset`: `resolve_path`, `exists`, `list`, `search`, `save`.
- `material`: `create`, `read`, `trace_output`, `trace_parameter`, `set_parameter`, `patch_validate`, `patch_apply`, catalog/config actions.
- `blueprint`: `read`, variable actions, node creation, pin default, connect/disconnect, remove, compile.
- `niagara`: `export`, system/emitter/module actions, local module creation, module graph patching, module input/static-switch actions, semantic search, compile diagnostics.

Success returns `{"ok": true, ...}`. Failure returns `{"ok": false, "error": {"code": "...", "message": "...", "retryable": false, "details": {...}}}`.

## Extending Tools

Add new actions in `tools/action_tools.py` and keep the public MCP surface category-based.

Use `toolplay_bridge.call_unreal_bridge(...)` to call UE. Keep graph understanding and editing logic in the C++ plugin.

For UE-backed actions, C++ `FToolPlayMCPToolRegistry` is the input-schema source of truth. Python action metadata should only map public action names to bridge commands and add MCP-facing output schemas. Python-local actions, such as catalog search and usage reads, keep their schemas in Python.

For tool discovery and schema rules, read `references/tool_registry.md`.

For catalog/database updates, read `references/catalog_governance.md` and run:

```powershell
python Plugins/ToolPlayMCP/MCPServer/scripts/validate_catalogs.py
```

For alignment with UE5.8's experimental MCP plugin, read `references/uemcp_alignment.md`.

## Run

On Windows, use the setup bat from the Unreal project root or from any working directory:

```powershell
Plugins/ToolPlayMCP/MCPServer/setup_toolplay_mcp.bat
```

The bat does the common setup/doctor steps:

- Checks Python.
- Installs `requirements.txt`.
- Verifies `mcp.server.fastmcp` imports.
- Validates ToolPlayMCP catalogs.
- Checks the Unreal Editor TCP bridge at `127.0.0.1:55557`.
- Prints a Codex `config.toml` snippet.

For AI agents or CI-like checks that should not install packages, use:

```powershell
Plugins/ToolPlayMCP/MCPServer/setup_toolplay_mcp.bat --check-only
```

Manual setup is:

```powershell
python -m pip install -r Plugins/ToolPlayMCP/MCPServer/requirements.txt
```

```powershell
python Plugins/ToolPlayMCP/MCPServer/toolplay_mcp_server.py
```

`toolplay_bridge.py` can send newline-delimited JSON directly to the Unreal TCP bridge for local debugging, but it is not the MCP server. Codex and other MCP clients should start `toolplay_mcp_server.py`.

## Codex Config

Add this to `C:\Users\<You>\.codex\config.toml`, then restart Codex:

```toml
[mcp_servers.toolplay-mcp]
command = 'python'
args = ['<ProjectRoot>\Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py']
```

Example:

```toml
[mcp_servers.toolplay-mcp]
command = 'python'
args = ['G:\UnrealProject\ToolPlayProject\Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py']
```

After restart, an MCP-aware agent should see category tools such as `system`, `asset`, `material`, `blueprint`, and `niagara`. If it only talks to `toolplay_bridge.py`, it is using the debug bridge path rather than the MCP server.
