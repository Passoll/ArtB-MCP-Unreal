# ArtB MCP Unreal

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7%20Tested-orange)](https://www.unrealengine.com)
[![Python](https://img.shields.io/badge/Python-3.12%2B-yellow)](https://www.python.org)
[![Status](https://img.shields.io/badge/Status-Experimental-red)](https://github.com/Passoll/ArtB-MCP-Unreal)

[中文](README.zh-CN.md)

ArtB MCP Unreal is an experimental Unreal Editor plugin that exposes artist-oriented asset understanding, diagnostics, and editing workflows to AI agents. It currently focuses on compact export and mutation tools for Material graphs, Blueprint graphs, and Niagara systems/emitters/modules through a local MCP server.

[Sample Video Bilibili](https://www.bilibili.com/video/BV12NEM62EiC)

## 🌟 Overview

- Token-efficient context: compact Unreal graph exports instead of dumping raw editor data.
- Editable workflows: read, diagnose, and patch Materials, Blueprints, and Niagara assets.
- Undo-aware operations: mutation tools are designed to enter Unreal transactions where possible.
- Workflow knowledge: references and catalogs capture common Material, Blueprint, and Niagara usage rules.
- Thin MCP layer: Python handles MCP protocol adaptation while Unreal-side C++ owns editor inspection and mutation.

<p align="center">
  <img src="Docs\pic\Cut1.png" alt="ToolPlayMCP screenshot" width="1080" />
</p>

<p align="center">
  <img src="Docs\pic\Ripple.png" alt="ToolPlayMCP screenshot" width="1080" />
</p>

## ⚠️ Experimental

This is an early Editor-only tool intended for research, prototyping, and fast iteration. It is primarily developed and tested on UE 5.7. Niagara, Graph Editor, Blueprint, and other editor APIs can change between Unreal versions, so other versions may require small adaptations.

Use this plugin with Git or another version-control system. AI agents should ask for explicit user approval before modifying existing project assets. For experiments, duplicate assets before letting an agent edit them.

## 🧩 Components

Main structure:

```text
Plugins/ToolPlayMCP/
├─ Source/ToolPlayMCP/              Unreal Editor plugin source
│  ├─ Public/                       Public headers and service interfaces
│  ├─ Private/ToolPlayMCP.cpp       Plugin startup, menu registration, Debug Panel tab registration
│  ├─ Private/ToolPlayMCPBridgeServer.cpp
│  │                                  Local TCP bridge, listening on 127.0.0.1:55557 by default
│  ├─ Private/SToolPlayMCPDebugPanel.cpp
│  │                                  In-editor tool browser and manual call panel
│  ├─ Private/ToolRegistry/         C++ tool registry, schemas, and dispatch
│  ├─ Private/Material/             Material graph export, compaction, parameters, and patches
│  ├─ Private/Blueprint/            Blueprint graph export, diagnostics, and editing
│  ├─ Private/Niagara/              Niagara system/module/catalog/diagnostics/editing
│  ├─ Private/Graph/                Graph sessions, node maps, selections, and generic graph patches
│  └─ Private/Services/             Cross-domain asset loading/saving services
├─ MCPServer/                       Python MCP server and thin tool layer
│  ├─ toolplay_mcp_server.py        MCP stdio entrypoint
│  ├─ toolplay_bridge.py            TCP client for the Unreal bridge
│  ├─ tools/                        MCP tool category wrappers
│  ├─ references/                   Agent usage guides and safety rules
│  ├─ catalogs/                     Material/Niagara semantic catalogs
│  └─ setup_toolplay_mcp.bat        Windows setup/check helper
└─ Docs/                            Screenshots, structure notes, and extra docs
```

Runtime path:

```text
MCP Client
  -> Python MCP Server
  -> Unreal TCP Bridge
  -> C++ Tool Registry
  -> Material / Blueprint / Niagara Editor APIs
```

Do not connect an MCP client directly to Unreal's TCP port. `127.0.0.1:55557` is the plugin's internal bridge, not the MCP protocol endpoint. External agents should connect to `toolplay_mcp_server.py`.

## 🚀 Quick Start

1. Copy this repository into your Unreal project at `Plugins/ToolPlayMCP`.
2. Open Unreal Editor and enable the `ToolPlayMCP` plugin from the Plugins panel.
3. Disable Live Coding and rebuild the Editor target. You can build from Rider/Visual Studio or run:

```powershell
<UE_ROOT>\Engine\Build\BatchFiles\Build.bat <ProjectName>Editor Win64 Development -Project="<ProjectRoot>\<ProjectName>.uproject" -WaitMutex
```

4. Reopen Unreal Editor. Once the plugin is loaded, the Unreal bridge listens on `127.0.0.1:55557`.
5. Open `ToolPlayMCP Debug Panel` from the editor menu to inspect registered tools, input schemas, and test calls manually.
6. Configure the MCP server in Codex, Claude Desktop, Cursor, Windsurf, or another MCP-capable agent.

## MCP Server

MCP entrypoint:

```text
Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py
```

Recommended Windows setup/check command:

```powershell
Plugins\ToolPlayMCP\MCPServer\setup_toolplay_mcp.bat
```

The script installs Python dependencies, validates local catalogs, checks whether the Unreal Editor bridge is online, and prints a copyable MCP configuration snippet. If an agent only needs to check the environment without installing dependencies, run:

```powershell
Plugins\ToolPlayMCP\MCPServer\setup_toolplay_mcp.bat --check-only
```

Manual dependency install:

```powershell
python -m pip install -r Plugins\ToolPlayMCP\MCPServer\requirements.txt
```

Manual server launch:

```powershell
python Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py
```

If you use `uv`, point `--directory` to the `MCPServer` directory:

```json
{
  "mcpServers": {
    "toolplay-mcp": {
      "command": "uv",
      "args": [
        "--directory",
        "<ProjectRoot>\\Plugins\\ToolPlayMCP\\MCPServer",
        "run",
        "toolplay_mcp_server.py"
      ]
    }
  }
}
```

If you use system Python:

```json
{
  "mcpServers": {
    "toolplay-mcp": {
      "command": "python",
      "args": [
        "<ProjectRoot>\\Plugins\\ToolPlayMCP\\MCPServer\\toolplay_mcp_server.py"
      ]
    }
  }
}
```

Unreal Editor must be open and the `ToolPlayMCP` plugin must be loaded before MCP calls can reach Unreal.

## Agent Configuration

Different MCP clients store configuration in different places, but the server definition is usually the same. Exact locations may change between client versions, so prefer the client's current settings UI or official docs when available.

| Client | Configuration approach |
| --- | --- |
| Codex | Usually `C:\Users\<You>\.codex\config.toml` |
| Claude Desktop | Add the server from Claude Desktop Developer/MCP settings; on Windows a common config file is `%APPDATA%\Claude\claude_desktop_config.json` |
| Cursor | Use a project-level `.cursor\mcp.json` or Cursor Settings MCP entry |
| Windsurf | Add the same JSON server config through Windsurf's MCP/Plugins settings |
| Other agents | Any stdio MCP-capable client can use the same JSON server config |

Codex TOML example:

```toml
[mcp_servers.toolplay-mcp]
command = 'python'
args = ['<ProjectRoot>\Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py']
```

Claude Desktop, Cursor, Windsurf, and other JSON-based clients:

```json
{
  "mcpServers": {
    "toolplay-mcp": {
      "command": "python",
      "args": [
        "G:\\UnrealProject\\ToolPlayProject\\Plugins\\ToolPlayMCP\\MCPServer\\toolplay_mcp_server.py"
      ]
    }
  }
}
```

Restart the agent after changing configuration. A good first step is to call a system/diagnostics tool to confirm connectivity before running asset mutation tools.

## Safety Notes

- Agents should ask for explicit user approval before modifying existing `.uasset` files.
- Duplicate assets or commit your project before large edits.
- Mutation tools should use `FScopedTransaction` where possible so Unreal Editor Undo/Redo can record the operation.
- Blueprint, Niagara, and Material edits should be followed by the relevant diagnostic or compile-check tools.
- Catalogs and references define agent operation rules. Do not let agents rewrite them without a governance rule.

## More Docs

- `Docs/code-structure.md`: deeper C++/Python structure notes.
- `MCPServer/README.md`: MCP server and bridge notes.
- `MCPServer/references/`: agent rules and Material/Blueprint/Niagara workflow guides.
- `MCPServer/catalogs/`: cacheable and extensible Material/Niagara semantic catalogs.

## License

MIT License. Keep the repository-level `LICENSE` file and original copyright notice when publishing or redistributing.
