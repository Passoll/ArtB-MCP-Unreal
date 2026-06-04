# ArtB MCP Unreal

ArtB MCP Unreal is an experimental Unreal Editor plugin that exposes AI-oriented tools for understanding and editing Unreal assets through a local MCP bridge.

Current focus areas:

- Compact Material graph export, tracing, and parameter/patch operations.
- Compact Blueprint graph export and basic Blueprint edit operations.
- Niagara System export, module catalog search, module/input edits, user parameter binding, and compile diagnostics.
- A local Python MCP server that translates MCP calls into the plugin's Unreal Editor TCP bridge.

## Status

This is an early editor-only tool. It is intended for iteration and research, not production use yet.

Tested primarily against UE 5.7 during development. Niagara and graph editor APIs can move between engine versions, so expect version-specific fixes.

## Install

1. Copy this repository folder into an Unreal project as `Plugins/ToolPlayMCP`.
2. Enable the `ToolPlayMCP` plugin in Unreal Editor.
3. Rebuild the editor target.
4. Open the ToolPlayMCP debug panel from the editor menu, or use the bridge directly.

The Unreal-side bridge listens on `127.0.0.1:55557` while the plugin is active.

## MCP Server

The MCP entrypoint is:

```powershell
python Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py
```

Do not point Codex or another MCP client directly at the Unreal TCP port. Use the Python MCP server; it exposes stable MCP tools and forwards calls to the Unreal bridge.

For details, see:

- `MCPServer/README.md`
- `MCPServer/references/tool_registry.md`
- `MCPServer/references/niagara_system.md`
- `Docs/code-structure.md`

## Notes

Generated Unreal build outputs are intentionally ignored. If you build locally, `Binaries/` and `Intermediate/` should remain untracked.
