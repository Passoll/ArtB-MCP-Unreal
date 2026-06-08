# ArtB MCP Unreal

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7%20Tested-orange)](https://www.unrealengine.com)
[![Python](https://img.shields.io/badge/Python-3.12%2B-yellow)](https://www.python.org)
[![Status](https://img.shields.io/badge/Status-Experimental-red)](https://github.com/Passoll/ArtB-MCP-Unreal)

[English](README.md)

ArtB MCP Unreal 是一个实验性的 Unreal Editor 插件，目标是把面向美术工作流的资产理解、诊断和编辑能力暴露给 AI Agent。它当前重点支持材质图、蓝图图、Niagara System/Emitter/Module 的紧凑导出和编辑，并通过本地 MCP Server 接入 Codex、Claude Desktop、Cursor、Windsurf 等支持 MCP 的客户端。

## 🌟 Overview

- 数据优化：用更少 token 表达 Unreal 图结构，避免把原始节点数据无差别塞给模型。
- 操作可逆：编辑行为尽量进入 Unreal Transaction，方便使用 Undo/Redo 追溯。
- 范式指引：通过 references/catalogs 沉淀材质、蓝图、Niagara 的使用规则和常见语义。
- 美术导向：围绕材质、蓝图、Niagara 的理解、修改、诊断和复用工作流设计接口。
- 轻量接入：Python MCP Server 只做协议适配，真正的 Unreal 读写逻辑集中在 Editor 插件内。

<p align="center">
  <img src="Docs\pic\Cut1.png" alt="ToolPlayMCP screenshot" width="1080" />
</p>
<p align="center">
  <img src="Docs\pic\Ripple.png" alt="ToolPlayMCP screenshot" width="1080" />
</p>

## ⚠️ Experimental

这是一个 Editor-only 的早期工具，适合研究、原型验证和快速迭代。当前主要基于 UE 5.7 开发和测试；Niagara、Graph Editor、Blueprint 等编辑器 API 在不同 UE 版本之间可能变化，如果你使用其他版本，可能需要做少量适配。

请在 Git 或其他版本管理下使用本插件。AI 修改现有工程资产前应先获得使用者确认；如果只是实验效果，建议先复制资产再让 Agent 修改。

## 🧩 Components

核心结构如下：

```text
Plugins/ToolPlayMCP/
├─ Source/ToolPlayMCP/              Unreal Editor 插件源码
│  ├─ Public/                       对外头文件和服务接口
│  ├─ Private/ToolPlayMCP.cpp       插件启动、菜单注册、Debug Panel tab 注册
│  ├─ Private/ToolPlayMCPBridgeServer.cpp
│  │                                  本地 TCP bridge，默认监听 127.0.0.1:55557
│  ├─ Private/SToolPlayMCPDebugPanel.cpp
│  │                                  编辑器内工具查看/手动调用面板
│  ├─ Private/ToolRegistry/         C++ 侧工具注册、schema 和调用分发
│  ├─ Private/Material/             材质图导出、压缩、参数和 patch 操作
│  ├─ Private/Blueprint/            蓝图图导出、编译诊断和编辑操作
│  ├─ Private/Niagara/              Niagara system/module/catalog/diagnostics/editing
│  ├─ Private/Graph/                图结构会话、节点映射、选区和通用 graph patch
│  └─ Private/Services/             资产加载、保存等跨领域服务
├─ MCPServer/                       Python MCP Server 和工具薄层
│  ├─ toolplay_mcp_server.py        MCP stdio 入口
│  ├─ toolplay_bridge.py            Python 到 Unreal bridge 的 TCP 客户端
│  ├─ tools/                        MCP tool 分类封装
│  ├─ references/                   Agent 使用指南和安全规则
│  ├─ catalogs/                     材质/Niagara 等语义 catalog
│  └─ setup_toolplay_mcp.bat        Windows 快速安装/检查脚本
└─ Docs/                            截图、结构说明和额外文档
```

运行链路是：

```text
MCP Client
  -> Python MCP Server
  -> Unreal TCP Bridge
  -> C++ Tool Registry
  -> Material / Blueprint / Niagara Editor APIs
```

不要把 MCP Client 直接连到 Unreal 的 TCP 端口。`127.0.0.1:55557` 是插件内部 bridge，不是 MCP 协议入口；外部 Agent 应连接 `toolplay_mcp_server.py`。

## 🚀 Quick Start

1. 把本仓库复制到 Unreal 工程的 `Plugins/ToolPlayMCP` 目录。
2. 打开 Unreal Editor，在 Plugins 面板中启用 `ToolPlayMCP`。
3. 关闭 Live Coding，重新编译 Editor target。可以用 Rider/Visual Studio 编译，也可以在命令行中执行：

```powershell
<UE_ROOT>\Engine\Build\BatchFiles\Build.bat <ProjectName>Editor Win64 Development -Project="<ProjectRoot>\<ProjectName>.uproject" -WaitMutex
```

4. 重新打开 Unreal Editor。插件加载后，Unreal 侧 bridge 会监听 `127.0.0.1:55557`。
5. 在编辑器菜单中打开 `ToolPlayMCP Debug Panel`，可以查看已注册工具、输入 schema，并手动调用接口检查返回结果。
6. 按下面的 MCP Server 配置把它接入 Codex、Claude Desktop 或其他 Agent。

## MCP Server

MCP 入口文件是：

```text
Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py
```

推荐先运行 Windows 安装/检查脚本：

```powershell
Plugins\ToolPlayMCP\MCPServer\setup_toolplay_mcp.bat
```

这个脚本会安装 Python 依赖、校验本地 catalog、检查 Unreal Editor bridge 是否在线，并打印可复制的 MCP 配置片段。如果 Agent 只需要检查环境，不想安装依赖，可以运行：

```powershell
Plugins\ToolPlayMCP\MCPServer\setup_toolplay_mcp.bat --check-only
```

手动安装依赖：

```powershell
python -m pip install -r Plugins\ToolPlayMCP\MCPServer\requirements.txt
```

手动启动 MCP Server：

```powershell
python Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py
```

如果你使用 `uv`，MCP 配置里的 `--directory` 应指向 `MCPServer` 目录：

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

如果你使用系统 Python，可以直接配置：

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

Unreal Editor 必须已经打开，并且 `ToolPlayMCP` 插件已加载，MCP 调用才能真正进入 UE。

## Agent 配置

不同 MCP Client 的配置文件位置不同，但 tool 配置内容基本一致。常见入口如下；具体位置可能随客户端版本变化，请以对应客户端文档或设置页为准。

| Client | 配置方式 |
| --- | --- |
| Codex | 通常写入 `C:\Users\<You>\.codex\config.toml` |
| Claude Desktop | 在 Claude Desktop 的 Developer/MCP 设置中添加 server；Windows 常见配置文件在 `%APPDATA%\Claude\claude_desktop_config.json` |
| Cursor | 可放在项目级 `.cursor\mcp.json`，或通过 Cursor Settings 的 MCP 配置入口添加 |
| Windsurf | 通过 Windsurf 的 MCP/Plugins 设置入口添加，使用同样的 JSON server 配置 |
| 其他 Agent | 只要支持 stdio MCP server，就使用上面的 JSON 配置方式 |

Codex 的 TOML 示例：

```toml
[mcp_servers.toolplay-mcp]
command = 'python'
args = ['<ProjectRoot>\Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py']
```

Claude Desktop、Cursor、Windsurf 等 JSON 配置示例：

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

配置后重启对应 Agent。建议先让 Agent 调用系统/诊断类工具确认连接状态，再执行资产修改类操作。

## Safety Notes

- 修改现有 `.uasset` 前，Agent 应先获得使用者明确同意。
- 批量编辑前建议先复制资产，或者确保项目已经提交到 Git。
- 编辑工具应尽量使用 `FScopedTransaction`，这样 Unreal Editor 的 Undo/Redo 才能记录操作。
- Blueprint、Niagara、Material 修改后应调用对应诊断或编译检查工具确认状态。
- Catalog 和 references 是 Agent 的操作范式来源，不建议让 Agent 无规则地随意改写。

## More Docs

- `Docs/code-structure.md`：更详细的 C++/Python 结构说明。
- `MCPServer/README.md`：MCP Server 和 bridge 的补充说明。
- `MCPServer/references/`：Agent 使用规则、Niagara/Material/Blueprint 工作流说明。
- `MCPServer/catalogs/`：可缓存、可扩展的材质和 Niagara 语义 catalog。

## License

MIT License。开源或二次分发时请保留仓库根目录中的 `LICENSE` 文件和原始版权声明。
