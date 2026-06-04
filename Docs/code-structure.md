# ToolPlayMCP Code Structure

This plugin is organized around small domain services while preserving the existing MCP bridge behavior.

## Runtime Entry Points

- `ToolPlayMCP.cpp`: plugin lifecycle, editor menu/tab registration, Python MCP server launch controls.
- `SToolPlayMCPDebugPanel.*`: editor debug UI for listing bridge tools, editing params JSON, and calling commands locally.
- `ToolPlayMCPBridgeServer.*`: TCP bridge protocol plus the shared command spec list used by `list_tools` and the debug panel.
- `ToolRegistry/ToolPlayMCPToolRegistry.*`: shared tool metadata, toolset/domain descriptions, input schemas, and discovery JSON used by bridge and debug UI.

## Domain Service Surface

- `Services/ToolPlayMCPAssetService.*`: asset-level operations such as saving loaded assets.
- `Blueprint/ToolPlayMCPBlueprintService.*`: Blueprint compact graph export and safe graph editing command surface.
- `Material/ToolPlayMCPMaterialService.*`: Material export, trace, config, and patch command surface.
- `Niagara/ToolPlayMCPNiagaraSystemExporter.*`: Niagara System-level export facade.
- `Niagara/ToolPlayMCPNiagaraSystemService.*`: Niagara System creation and emitter-level editing facade.
- `Niagara/ToolPlayMCPNiagaraModuleService.*`: Niagara module input inspection and mutation facade.
- `Niagara/ToolPlayMCPNiagaraCatalog.*`: Niagara module/dynamic-input/function catalog search through editor APIs.

Bridge code should depend on these service classes instead of reaching into graph/export internals directly.

## Implementation Files

- `Graph/ToolPlayMCPRawGraphExporter.*`: raw Blueprint/Material graph export used by the debug panel.
- `Graph/ToolPlayMCPGraphExportTypes.h`: USTRUCT types for raw graph JSON.
- `Blueprint/ToolPlayMCPBlueprintService.*`: Blueprint event/function/macro compact export, session aliases, and node/pin edit implementation.
- `Material/ToolPlayMCPCompactMaterialTypes.h`: compact Material graph/session structs.
- `Material/ToolPlayMCPMaterialService.*`: Material compact graph export, tracing, config/schema, patching, asset creation, and material graph sessions.
- `Niagara/ToolPlayMCPNiagaraSystemExporter.*`: Niagara System export implementation.
- `Niagara/ToolPlayMCPNiagaraSystemService.*`: Niagara System asset creation and emitter insertion implementation.
- `Niagara/ToolPlayMCPNiagaraModuleService.*`: Niagara script/module graph export, session alias storage, and stack/module mutation implementation.

Do not add new bridge commands directly to implementation files. Prefer adding or extending a domain service first, then add one registry command spec and one bridge handler so the debug panel and discovery tools see it automatically.

## Recommended Next Split

1. Split Material service internals into:
   - `Material/ToolPlayMCPMaterialExporter`
   - `Material/ToolPlayMCPMaterialTracer`
   - `Material/ToolPlayMCPMaterialPatchService`
   - `Material/ToolPlayMCPMaterialCatalog`
2. Move Blueprint raw graph export into `Blueprint/ToolPlayMCPBlueprintExporter`.
3. Split Niagara module service internals into:
   - `Niagara/ToolPlayMCPNiagaraSession`
   - `Niagara/ToolPlayMCPNiagaraScriptExporter`
   - `Niagara/ToolPlayMCPNiagaraModuleService` implementation
4. Split bridge command parsing into domain command handlers once the services are stable:
   - `Bridge/ToolPlayMCPMaterialCommands`
   - `Bridge/ToolPlayMCPNiagaraCommands`
   - `Bridge/ToolPlayMCPAssetCommands`
