from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from tools import register_tools


mcp = FastMCP("ToolPlayMCP")
register_tools(mcp)


if __name__ == "__main__":
    mcp.run()
