from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from .action_tools import register_action_tools


def register_tools(mcp: FastMCP) -> None:
    register_action_tools(mcp)
