from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp.server.fastmcp import FastMCP
else:
    FastMCP = Any

from .action_tools import register_action_tools


def register_tools(mcp: FastMCP) -> None:
    register_action_tools(mcp)
