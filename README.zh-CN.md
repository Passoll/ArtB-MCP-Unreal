# ArtB MCP Unreal

[English](README.md)

ArtB MCP Unreal 是一个实验性的 Unreal Editor 插件，用来把 Unreal 资产的理解和编辑能力暴露给 AI。它不是让 MCP 直接操作 Unreal 对象，而是在编辑器插件里提供稳定的图导出、诊断和修改能力，再通过本地 Python MCP Server 转发给 Codex 或其他 MCP 客户端。

## 当前重点

- 材质图：紧凑导出、输出/参数追踪、参数设置、图 patch 校验和应用。
- 蓝图图：紧凑导出、变量/节点/连线等基础编辑能力。
- Niagara：System 导出、Module catalog 搜索、Emitter/Module/Input 编辑、User Parameter 绑定、编译诊断和 Stack Issue 查询。
- MCP：Python MCP Server 负责把 MCP 调用转成 Unreal Editor 内的 TCP bridge JSON 请求。

## 状态

这是一个 Editor-only 的早期工具，适合研究和快速迭代，还不建议直接用于生产流程。

当前主要基于 UE 5.7 开发和测试。Niagara、Graph Editor、Blueprint 等编辑器 API 在不同 UE 版本之间可能变化，如果你使用其他版本，可能需要做少量适配。

## 安装

1. 把这个仓库文件夹复制到 Unreal 工程的 `Plugins/ToolPlayMCP`。
2. 在 Unreal Editor 里启用 `ToolPlayMCP` 插件。
3. 重新编译 Editor target。
4. 打开编辑器菜单里的 ToolPlayMCP Debug Panel，或直接通过 MCP Server 调用。

插件启用后，Unreal 侧 bridge 会监听 `127.0.0.1:55557`。

## MCP Server

MCP 入口文件是：

```text
Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py
```

Windows 推荐先运行：

```powershell
Plugins\ToolPlayMCP\MCPServer\setup_toolplay_mcp.bat
```

这个脚本会安装 Python 依赖、校验本地 catalog、检查 Unreal Editor bridge 是否在线，并打印 Codex 配置片段。如果 AI agent 只需要检查环境、不想安装依赖，可以运行：

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

不要把 Codex 或其他 MCP 客户端直接连到 Unreal TCP 端口。请启动 Python MCP Server；它会暴露稳定的 MCP tools，并把调用转发给 Unreal bridge。

## Codex 配置

把下面内容加入 `C:\Users\<You>\.codex\config.toml`，然后重启 Codex：

```toml
[mcp_servers.toolplay-mcp]
command = 'python'
args = ['<ProjectRoot>\Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py']
```

把 `<ProjectRoot>` 替换成你的 Unreal 工程路径，例如：

```toml
[mcp_servers.toolplay-mcp]
command = 'python'
args = ['G:\UnrealProject\ToolPlayProject\Plugins\ToolPlayMCP\MCPServer\toolplay_mcp_server.py']
```

Unreal Editor 必须已经打开，并且 `ToolPlayMCP` 插件已加载，MCP 调用才能真正进入 UE。

## AI 资产安全规则

默认情况下，现有 Unreal 资产是只读的。AI agent 可以读取、导出、诊断、搜索、追踪和校验资产，但创建新资产、修改现有资产或保存 package 前，必须先获得用户确认。

对于现有 `.uasset`，例如 Blueprint、Material、Niagara System、Level、Widget、Data Asset，优先复制一份再改副本。除非用户在当前对话里明确批准了具体资产路径和修改内容，否则不要直接改原资产。

## 更多文档

- `MCPServer/README.md`
- `MCPServer/references/tool_registry.md`
- `MCPServer/references/catalog_governance.md`
- `MCPServer/references/material_patch.md`
- `MCPServer/references/niagara_system.md`
- `Docs/code-structure.md`

## 备注

Unreal 生成文件不会提交。你本地编译后，`Binaries/`、`Intermediate/`、`Saved/` 等目录应保持未跟踪状态。
