from __future__ import annotations

try:
    from mcp.server.fastmcp import FastMCP
except ModuleNotFoundError as exc:
    if exc.name == "mcp":
        raise SystemExit(
            "Missing Python dependency 'mcp'. Install it with:\n"
            "  python -m pip install -r Plugins\\ToolPlayMCP\\MCPServer\\requirements.txt\n"
            "Direct toolplay_bridge.py JSON calls are only for local debugging; MCP clients need this dependency."
        ) from exc
    raise

from tools import register_tools


mcp = FastMCP("ToolPlayMCP")
register_tools(mcp)


if __name__ == "__main__":
    mcp.run()
