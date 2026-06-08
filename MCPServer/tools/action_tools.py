from __future__ import annotations

from pathlib import Path
import json
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp.server.fastmcp import FastMCP
else:
    FastMCP = Any

from toolplay_bridge import call_unreal_bridge

from .asset_tools import asset_exists, list_assets, resolve_asset_path, search_assets
from .tool_response import dispatch_action, error, ok, require


REFERENCE_DIR = Path(__file__).resolve().parents[1] / "references"
CATALOG_DIR = Path(__file__).resolve().parents[1] / "catalogs"
USAGE_TOPICS = {
    "tool_registry": REFERENCE_DIR / "tool_registry.md",
    "blueprint_graph": REFERENCE_DIR / "blueprint_graph.md",
    "material_compact_graph": REFERENCE_DIR / "material_compact_graph.md",
    "material_tracing": REFERENCE_DIR / "material_tracing.md",
    "material_catalog": REFERENCE_DIR / "material_catalog.md",
    "material_patch": REFERENCE_DIR / "material_patch.md",
    "niagara_system": REFERENCE_DIR / "niagara_system.md",
    "catalog_governance": REFERENCE_DIR / "catalog_governance.md",
}


def _call(command: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    return call_unreal_bridge(command, params or {})


def _with_result_meta(response: dict[str, Any], result_type: str, schema_version: int = 1) -> dict[str, Any]:
    if response.get("ok") is True:
        response.setdefault("result_type", result_type)
        response.setdefault("schema_version", schema_version)
    return response


def _call_typed(command: str, params: dict[str, Any] | None, result_type: str, schema_version: int = 1) -> dict[str, Any]:
    return _with_result_meta(_call(command, params), result_type, schema_version)


def _load_catalog(name: str) -> dict[str, Any]:
    return json.loads((CATALOG_DIR / name).read_text(encoding="utf-8"))


def _matches(entry: dict[str, Any], query: str, category: str, kind: str) -> bool:
    if category and entry.get("category") != category:
        return False

    if kind:
        candidates = {
            str(entry.get("kind", "")).lower(),
            str(entry.get("compact_kind", "")).lower(),
            str(entry.get("display", "")).lower(),
        }
        if kind.lower() not in candidates:
            return False

    if query:
        haystack = " ".join(
            str(entry.get(field, ""))
            for field in ("kind", "compact_kind", "display", "category", "notes")
        ).lower()
        return query.lower() in haystack

    return True


def _semantic_matches(entry: dict[str, Any], query: str) -> bool:
    if not query:
        return True

    def flatten(value: Any) -> str:
        if isinstance(value, dict):
            return " ".join(str(key) + " " + flatten(item) for key, item in value.items())
        if isinstance(value, list):
            return " ".join(flatten(item) for item in value)
        return str(value)

    haystack_parts = [
        str(entry.get("asset", "")),
        str(entry.get("name", "")),
        str(entry.get("summary", "")),
        str(entry.get("notes", "")),
        flatten(entry.get("semantic_tags", [])),
        flatten(entry.get("preferred_stacks", [])),
        flatten(entry.get("inputs", [])),
        flatten(entry.get("outputs", [])),
        flatten(entry.get("writes", [])),
        flatten(entry.get("side_effects", [])),
        flatten(entry.get("input_value_kinds", [])),
        flatten(entry.get("key_inputs", [])),
        flatten(entry.get("critical_inputs", [])),
        flatten(entry.get("critical_static_switches", [])),
        flatten(entry.get("stack_requirements", [])),
        flatten(entry.get("required_followups", [])),
        flatten(entry.get("common_edits", [])),
        flatten(entry.get("pitfalls", [])),
    ]
    haystack = " ".join(haystack_parts).lower()
    terms = [term for term in query.lower().replace("/", " ").replace("_", " ").split() if term]
    return all(term in haystack for term in terms)


def _semantic_key(entry: dict[str, Any]) -> tuple[str, str]:
    return (
        str(entry.get("asset", "")).lower(),
        str(entry.get("name", "")).lower(),
    )


def _merge_niagara_semantics(response: dict[str, Any]) -> dict[str, Any]:
    catalog = response.get("catalog") or response.get("result", {}).get("catalog")
    if not isinstance(catalog, dict):
        return response

    entries = catalog.get("entries")
    if not isinstance(entries, list):
        return response

    by_asset: dict[str, dict[str, Any]] = {}
    by_name: dict[str, dict[str, Any]] = {}
    for semantic in _load_catalog("niagara_modules.semantic.json").get("entries", []):
        asset, name = _semantic_key(semantic)
        if asset:
            by_asset[asset] = semantic
        if name:
            by_name[name] = semantic

    for entry in entries:
        if not isinstance(entry, dict):
            continue
        entry["addable"] = True
        entry["deprecated"] = False
        entry["hidden"] = False
        entry["visibility"] = "library"
        semantic = by_asset.get(str(entry.get("asset", "")).lower()) or by_name.get(str(entry.get("name", "")).lower())
        entry["semantic_status"] = "matched" if semantic else "missing"
        entry["add_workflow"] = "Use this search result's asset path with niagara(action='add_module'), then re-export, list inputs, and only then set input overrides."
        if semantic:
            for field in (
                "summary",
                "semantic_tags",
                "preferred_stacks",
                "inputs",
                "outputs",
                "writes",
                "side_effects",
                "input_value_kinds",
                "key_inputs",
                "critical_inputs",
                "critical_static_switches",
                "stack_requirements",
                "required_followups",
                "common_edits",
                "pitfalls",
                "notes",
                "local_module_guidance",
            ):
                if field in semantic:
                    entry[field] = semantic[field]
    return response


def _material_node_catalog(params: dict[str, Any]) -> dict[str, Any]:
    catalog = _load_catalog("material_nodes.json")
    query = str(params.get("query", ""))
    category = str(params.get("category", ""))
    kind = str(params.get("kind", ""))
    limit = int(params.get("limit", 25))
    entries = [
        entry
        for entry in catalog.get("entries", [])
        if _matches(entry, query=query, category=category, kind=kind)
    ]
    safe_limit = max(1, min(limit, 100))
    return ok(
        result_type="material_node_catalog_search",
        domain=catalog.get("domain"),
        schema_version=catalog.get("schema_version"),
        count=len(entries),
        entries=entries[:safe_limit],
        truncated=len(entries) > safe_limit,
        available_categories=sorted(
            {entry.get("category") for entry in catalog.get("entries", []) if entry.get("category")}
        ),
    )


def _material_get_node(params: dict[str, Any]) -> dict[str, Any]:
    if missing := require(params, "kind"):
        return missing
    catalog = _load_catalog("material_nodes.json")
    matches = [
        entry
        for entry in catalog.get("entries", [])
        if _matches(entry, query="", category="", kind=str(params["kind"]))
    ]
    if not matches:
        return error("ASSET_NOT_FOUND", f"Unknown material node kind: {params['kind']}")
    return ok(result_type="material_node_catalog_entry", schema_version=1, entry=matches[0])


def _material_categories(_: dict[str, Any]) -> dict[str, Any]:
    catalog = _load_catalog("material_nodes.json")
    return ok(
        result_type="material_node_categories",
        schema_version=1,
        categories=sorted(
            {entry.get("category") for entry in catalog.get("entries", []) if entry.get("category")}
        )
    )


def _niagara_semantic_search(params: dict[str, Any]) -> dict[str, Any]:
    catalog = _load_catalog("niagara_modules.semantic.json")
    query = str(params.get("query", ""))
    limit = int(params.get("limit", 10))
    entries = [entry for entry in catalog.get("entries", []) if _semantic_matches(entry, query)]
    safe_limit = max(1, min(limit, 50))
    return ok(
        result_type="niagara_module_semantic_search",
        domain=catalog.get("domain"),
        schema_version=catalog.get("schema_version"),
        count=len(entries),
        entries=entries[:safe_limit],
        truncated=len(entries) > safe_limit,
    )


def _niagara_semantic_get(params: dict[str, Any]) -> dict[str, Any]:
    if missing := require(params, "asset"):
        return missing
    catalog = _load_catalog("niagara_modules.semantic.json")
    asset_lower = str(params["asset"]).lower()
    for entry in catalog.get("entries", []):
        if entry.get("asset", "").lower() == asset_lower or entry.get("name", "").lower() == asset_lower:
            return ok(result_type="niagara_module_semantic_entry", schema_version=catalog.get("schema_version"), entry=entry)
    return error("ASSET_NOT_FOUND", f"Unknown Niagara module semantic entry: {params['asset']}")


def _system_usage(params: dict[str, Any]) -> dict[str, Any]:
    topic = str(params.get("topic", "material_compact_graph"))
    path = USAGE_TOPICS.get(topic)
    if not path:
        return error(
            "INVALID_PARAM",
            f"Unknown usage topic: {topic}",
            details={"available_topics": sorted(USAGE_TOPICS)},
        )
    return ok(result_type="usage_document", schema_version=1, topic=topic, content=path.read_text(encoding="utf-8"))


def _schema(properties: dict[str, Any] | None = None, required: list[str] | None = None) -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "object", "properties": properties or {}}
    if required:
        schema["required"] = required
    return schema


def _string(description: str = "") -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "string"}
    if description:
        schema["description"] = description
    return schema


def _integer(default: int | None = None) -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "integer"}
    if default is not None:
        schema["default"] = default
    return schema


def _boolean(default: bool | None = None) -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "boolean"}
    if default is not None:
        schema["default"] = default
    return schema


def _array() -> dict[str, Any]:
    return {"type": "array"}


def _number() -> dict[str, Any]:
    return {"type": "number"}


def _any() -> dict[str, Any]:
    return {}


def _output_schema(result_type: str, properties: dict[str, Any] | None = None, required: list[str] | None = None, schema_version: int = 1) -> dict[str, Any]:
    merged: dict[str, Any] = {
        "ok": {"type": "boolean", "const": True},
        "result_type": {"type": "string", "const": result_type},
        "schema_version": {"type": "integer", "const": schema_version},
    }
    merged.update(properties or {})
    return _schema(merged, required or ["ok", "result_type", "schema_version"])


def _counted_entries_schema(result_type: str, entry_description: str = "Result entry object.") -> dict[str, Any]:
    return _output_schema(
        result_type,
        {
            "count": _integer(),
            "entries": {"type": "array", "items": {"type": "object", "description": entry_description}},
            "truncated": _boolean(False),
        },
        ["ok", "result_type", "schema_version", "count", "entries", "truncated"],
    )


def _mutation_output_schema(result_type: str = "mutation_result") -> dict[str, Any]:
    return _output_schema(result_type, {"message": _string(), "asset_path": _string()})


def _spec(
    description: str,
    input_schema: dict[str, Any] | None = None,
    usage_topic: str = "",
    bridge_command: str = "",
    output_schema: dict[str, Any] | None = None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "description": description,
        "input_schema": input_schema or _schema(),
    }
    if output_schema:
        result["output_schema"] = output_schema
    if usage_topic:
        result["usage_topic"] = usage_topic
    if bridge_command:
        result["bridge_command"] = bridge_command
    return result


def _bridge_tools_by_name(bridge_registry: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    if not isinstance(bridge_registry, dict):
        return {}

    tools = bridge_registry.get("tools")
    if not isinstance(tools, list):
        toolset = bridge_registry.get("toolset")
        if isinstance(toolset, dict):
            tools = toolset.get("tools")
    if not isinstance(tools, list):
        return {}

    return {
        str(tool.get("name", "")): tool
        for tool in tools
        if isinstance(tool, dict) and tool.get("name")
    }


def _derive_specs_from_bridge(
    static_specs: dict[str, dict[str, dict[str, Any]]],
    bridge_registry: dict[str, Any] | None = None,
) -> dict[str, dict[str, dict[str, Any]]]:
    bridge_tools = _bridge_tools_by_name(bridge_registry)
    derived: dict[str, dict[str, dict[str, Any]]] = {}

    for domain, specs in static_specs.items():
        derived[domain] = {}
        for action, spec in specs.items():
            merged = dict(spec)
            bridge_command = str(merged.get("bridge_command", ""))
            bridge_spec = bridge_tools.get(bridge_command)
            if bridge_command:
                merged["registry_source"] = "cpp_bridge" if bridge_spec else "python_fallback"
            else:
                merged["registry_source"] = "python_local"

            if bridge_spec:
                merged["description"] = bridge_spec.get("description", merged.get("description", ""))
                if bridge_spec.get("input_schema"):
                    merged["input_schema"] = bridge_spec["input_schema"]
                if bridge_spec.get("usage_topic"):
                    merged["usage_topic"] = bridge_spec["usage_topic"]
                if bridge_spec.get("params_example"):
                    merged["params_example"] = bridge_spec["params_example"]
                if bridge_spec.get("domain"):
                    merged["bridge_domain"] = bridge_spec["domain"]

            derived[domain][action] = merged

    return derived


def _action_names() -> dict[str, list[str]]:
    return {
        "system": sorted(SYSTEM_ACTIONS),
        "asset": sorted(ASSET_ACTIONS),
        "material": sorted(MATERIAL_ACTIONS),
        "blueprint": sorted(BLUEPRINT_ACTIONS),
        "niagara": sorted(NIAGARA_ACTIONS),
    }


def _static_action_specs() -> dict[str, dict[str, dict[str, Any]]]:
    return {
        "system": SYSTEM_ACTION_SPECS,
        "asset": ASSET_ACTION_SPECS,
        "material": MATERIAL_ACTION_SPECS,
        "blueprint": BLUEPRINT_ACTION_SPECS,
        "niagara": NIAGARA_ACTION_SPECS,
    }


def _action_specs(bridge_registry: dict[str, Any] | None = None) -> dict[str, dict[str, dict[str, Any]]]:
    return _derive_specs_from_bridge(_static_action_specs(), bridge_registry)


def _system_list_tools(_: dict[str, Any]) -> dict[str, Any]:
    bridge_registry = _call("list_tools")
    return ok(
        result_type="toolplay_tool_catalog",
        schema_version=1,
        actions=_action_specs(bridge_registry),
        action_names=_action_names(),
        bridge_registry=bridge_registry,
        note="Bridge-backed action schemas are derived from the C++ bridge registry. Python-local actions keep their local schemas.",
    )


def _system_list_toolsets(_: dict[str, Any]) -> dict[str, Any]:
    return ok(
        result_type="toolplay_toolset_list",
        schema_version=1,
        toolsets=[
            {"domain": domain, "action_count": len(actions)}
            for domain, actions in _action_names().items()
        ],
        bridge_registry=_call("list_toolsets"),
    )


def _system_describe_toolset(params: dict[str, Any]) -> dict[str, Any]:
    if missing := require(params, "domain"):
        return missing

    domain = str(params["domain"])
    actions_by_domain = _action_names()
    if domain not in actions_by_domain:
        return error(
            "INVALID_PARAM",
            f"Unknown toolset/domain: {domain}",
            details={"available_domains": sorted(actions_by_domain)},
        )

    bridge_description = _call("describe_toolset", {"domain": domain})
    return ok(
        result_type="toolplay_toolset_description",
        schema_version=1,
        domain=domain,
        actions=_action_specs(bridge_description)[domain],
        action_names=actions_by_domain[domain],
        bridge_registry=bridge_description,
    )


SYSTEM_ACTIONS = {
    "ping": lambda params: _call_typed("ping", params, "bridge_ping"),
    "list_tools": _system_list_tools,
    "list_toolsets": _system_list_toolsets,
    "describe_toolset": _system_describe_toolset,
    "get_usage": _system_usage,
    "get_selection": lambda params: _call_typed("get_selection", params, "editor_selection"),
    "get_selected_graph_nodes": lambda params: _call_typed("get_selected_graph_nodes", params, "selected_graph_nodes"),
}


ASSET_ACTIONS = {
    "resolve_path": lambda params: resolve_asset_path(str(params.get("asset_path", ""))),
    "exists": lambda params: asset_exists(str(params.get("asset_path", ""))),
    "list": lambda params: list_assets(
        package_path=str(params.get("package_path", "/Game")),
        recursive=bool(params.get("recursive", True)),
        limit=int(params.get("limit", 100)),
        include_plugins=bool(params.get("include_plugins", False)),
    ),
    "search": lambda params: search_assets(
        query=str(params.get("query", "")),
        package_path=str(params.get("package_path", "/Game")),
        limit=int(params.get("limit", 50)),
        include_plugins=bool(params.get("include_plugins", False)),
    ),
    "save": lambda params: _call_typed("save_asset", {"asset_path": params.get("asset_path", "")}, "mutation_result"),
}


MATERIAL_ACTIONS = {
    "create": lambda params: _call_typed("create_material_asset", params, "mutation_result"),
    "read": lambda params: _call_typed("export_material_compact", params, "material_compact_graph"),
    "list_functions": lambda params: _call_typed("list_material_functions", params, "material_function_list"),
    "describe_function": lambda params: _call_typed("describe_material_function_interface", params, "material_function_interface"),
    "get_node_config": lambda params: _call_typed("get_material_node_config", params, "material_node_config"),
    "get_node_config_schema": lambda params: _call_typed("get_material_node_config_schema", params, "material_node_config_schema"),
    "trace_parameter": lambda params: _call_typed("trace_material_parameter", params, "material_parameter_trace"),
    "trace_output": lambda params: _call_typed("trace_material_output", params, "material_output_trace"),
    "set_parameter": lambda params: _call_typed("set_material_parameter", params, "mutation_result"),
    "patch_validate": lambda params: _call_typed("validate_material_patch", params, "material_patch_validation"),
    "patch_apply": lambda params: _call_typed("apply_material_patch", params, "mutation_result"),
    "search_nodes": _material_node_catalog,
    "get_node": _material_get_node,
    "list_node_categories": _material_categories,
}


BLUEPRINT_ACTIONS = {
    "read": lambda params: _call_typed("export_blueprint_compact", params, "blueprint_compact_graph"),
    "list_variables": lambda params: _call_typed("list_blueprint_variables", params, "blueprint_variable_list"),
    "add_variable": lambda params: _call_typed("add_blueprint_member_variable", params, "mutation_result"),
    "set_variable_default": lambda params: _call_typed("set_blueprint_variable_default", params, "mutation_result"),
    "add_call_node": lambda params: _call_typed("add_blueprint_function_call_node", params, "mutation_result"),
    "add_custom_event": lambda params: _call_typed("add_blueprint_custom_event_node", params, "mutation_result"),
    "add_variable_get": lambda params: _call_typed("add_blueprint_variable_get_node", params, "mutation_result"),
    "add_variable_set": lambda params: _call_typed("add_blueprint_variable_set_node", params, "mutation_result"),
    "set_pin_default": lambda params: _call_typed("set_blueprint_pin_default", params, "mutation_result"),
    "connect": lambda params: _call_typed("connect_blueprint_pins", params, "mutation_result"),
    "disconnect": lambda params: _call_typed("disconnect_blueprint_pin", params, "mutation_result"),
    "remove_node": lambda params: _call_typed("remove_blueprint_node", params, "mutation_result"),
    "compile": lambda params: _call_typed("compile_blueprint", params, "blueprint_compile_result"),
}


NIAGARA_ACTIONS = {
    "export": lambda params: _call_typed("export_niagara_system", params, "niagara_system_export"),
    "diagnostics": lambda params: _call_typed("get_niagara_diagnostics", params, "niagara_diagnostics"),
    "compile_status": lambda params: _call_typed("get_niagara_compile_status", params, "niagara_compile_status"),
    "stack_issues": lambda params: _call_typed("get_niagara_stack_issues", params, "niagara_stack_issues"),
    "create_system": lambda params: _call_typed("create_niagara_system", params, "mutation_result"),
    "add_emitter": lambda params: _call_typed("add_niagara_emitter", params, "mutation_result"),
    "add_default_emitter": lambda params: _call_typed("add_niagara_default_emitter", params, "mutation_result"),
    "remove_user_parameter": lambda params: _call_typed("remove_niagara_user_parameter", params, "mutation_result"),
    "set_emitter_sim_target": lambda params: _call_typed("set_niagara_emitter_sim_target", params, "mutation_result"),
    "list_renderers": lambda params: _call_typed("list_niagara_renderers", params, "niagara_renderer_list"),
    "get_renderer_schema": lambda params: _call_typed("get_niagara_renderer_schema", params, "niagara_renderer_schema"),
    "add_renderer": lambda params: _call_typed("add_niagara_renderer", params, "mutation_result"),
    "remove_renderer": lambda params: _call_typed("remove_niagara_renderer", params, "mutation_result"),
    "set_renderer_property": lambda params: _call_typed("set_niagara_renderer_property", params, "mutation_result"),
    "list_simulation_stages": lambda params: _call_typed("list_niagara_simulation_stages", params, "niagara_simulation_stage_list"),
    "add_simulation_stage": lambda params: _call_typed("add_niagara_simulation_stage", params, "mutation_result"),
    "remove_simulation_stage": lambda params: _call_typed("remove_niagara_simulation_stage", params, "mutation_result"),
    "move_simulation_stage": lambda params: _call_typed("move_niagara_simulation_stage", params, "mutation_result"),
    "set_simulation_stage_property": lambda params: _call_typed("set_niagara_simulation_stage_property", params, "mutation_result"),
    "configure_sprite_renderer": lambda params: _call_typed("configure_niagara_sprite_renderer", params, "mutation_result"),
    "search_module": lambda params: _with_result_meta(_merge_niagara_semantics(_call("search_niagara_modules", params)), "niagara_module_search"),
    "add_module": lambda params: _call_typed("add_niagara_module", params, "mutation_result"),
    "create_local_module": lambda params: _call_typed("create_niagara_local_module", params, "mutation_result"),
    "patch_module_graph": lambda params: _call_typed("apply_niagara_module_graph_patch", params, "mutation_result"),
    "remove_module": lambda params: _call_typed("remove_niagara_module", params, "mutation_result"),
    "move_module": lambda params: _call_typed("move_niagara_module", params, "mutation_result"),
    "set_module_enabled": lambda params: _call_typed("set_niagara_module_enabled", params, "mutation_result"),
    "list_module_inputs": lambda params: _call_typed("list_niagara_module_inputs", params, "niagara_module_inputs"),
    "get_module_input_override": lambda params: _call_typed("get_niagara_module_input_override", params, "niagara_module_input_override"),
    "set_module_input": lambda params: _call_typed("set_niagara_module_input", params, "mutation_result"),
    "set_static_switch": lambda params: _call_typed("set_niagara_static_switch", params, "mutation_result"),
    "set_module_object_input": lambda params: _call_typed("set_niagara_module_object_input", params, "mutation_result"),
    "bind_module_input": lambda params: _call_typed("bind_niagara_module_input_to_user_param", params, "mutation_result"),
    "search_module_semantics": _niagara_semantic_search,
    "get_module_semantics": _niagara_semantic_get,
}


SYSTEM_ACTION_SPECS = {
    "ping": _spec("Verify that the UE bridge is reachable.", bridge_command="ping"),
    "list_tools": _spec("List public MCP domain actions plus the underlying UE bridge registry.", usage_topic="tool_registry", bridge_command="list_tools"),
    "list_toolsets": _spec("List available ToolPlayMCP domains/toolsets.", usage_topic="tool_registry", bridge_command="list_toolsets"),
    "describe_toolset": _spec(
        "Describe one domain/toolset, including public actions and bridge command schemas.",
        _schema({"domain": _string("Domain such as material, blueprint, niagara, asset, or system.")}, ["domain"]),
        usage_topic="tool_registry",
        bridge_command="describe_toolset",
    ),
    "get_usage": _spec(
        "Return longer workflow notes for a usage topic.",
        _schema({"topic": _string("Usage topic such as material_patch or niagara_system.")}),
        output_schema=_output_schema("usage_document", {"topic": _string(), "content": _string()}, ["ok", "result_type", "schema_version", "topic", "content"]),
    ),
    "get_selection": _spec("Read focused graph node selection, falling back to Content Browser selected assets.", bridge_command="get_selection"),
    "get_selected_graph_nodes": _spec("Read currently selected nodes from the focused GraphEditor panel.", bridge_command="get_selected_graph_nodes"),
}


ASSET_ACTION_SPECS = {
    "resolve_path": _spec("Resolve or normalize an Unreal asset path.", _schema({"asset_path": _string()}, ["asset_path"])),
    "exists": _spec("Check whether an Unreal asset exists.", _schema({"asset_path": _string()}, ["asset_path"])),
    "list": _spec(
        "List assets under a package path.",
        _schema({
            "package_path": _string(),
            "recursive": _boolean(True),
            "limit": _integer(100),
            "include_plugins": _boolean(False),
        }),
    ),
    "search": _spec(
        "Search assets by name under a package path.",
        _schema({
            "query": _string(),
            "package_path": _string(),
            "limit": _integer(50),
            "include_plugins": _boolean(False),
        }),
    ),
    "save": _spec("Save a loaded Unreal asset package.", _schema({"asset_path": _string()}, ["asset_path"]), bridge_command="save_asset", output_schema=_mutation_output_schema()),
}


MATERIAL_ACTION_SPECS = {
    "create": _spec(
        "Create a new Material asset.",
        _schema({"package_path": _string(), "asset_name": _string()}, ["package_path", "asset_name"]),
        usage_topic="material_patch",
        bridge_command="create_material_asset",
        output_schema=_mutation_output_schema(),
    ),
    "read": _spec("Export compact Material graph context.", _schema({"asset_path": _string()}, ["asset_path"]), usage_topic="material_compact_graph", bridge_command="export_material_compact", output_schema=_output_schema("material_compact_graph", {"session_id": _string(), "nodes": _array(), "edges": _array()})),
    "list_functions": _spec("List Material Function calls used by a material or instance.", _schema({"asset_path": _string()}, ["asset_path"]), usage_topic="material_catalog", bridge_command="list_material_functions", output_schema=_output_schema("material_function_list", {"functions": _array()})),
    "describe_function": _spec("Describe a Material Function interface.", _schema({"function_path": _string()}, ["function_path"]), usage_topic="material_catalog", bridge_command="describe_material_function_interface", output_schema=_output_schema("material_function_interface", {"inputs": _array(), "outputs": _array()})),
    "get_node_config": _spec("Read editable config for a compact material node alias.", _schema({"asset_path": _string(), "node": _string()}, ["asset_path", "node"]), usage_topic="material_patch", bridge_command="get_material_node_config", output_schema=_output_schema("material_node_config", {"config": _any()})),
    "get_node_config_schema": _spec("Read reflected editable property schema for a MaterialExpression kind/class.", _schema({"kind": _string()}, ["kind"]), usage_topic="material_patch", bridge_command="get_material_node_config_schema", output_schema=_output_schema("material_node_config_schema", {"properties": _any()})),
    "trace_parameter": _spec("Trace graph usage from a named material parameter.", _schema({"asset_path": _string(), "parameter": _string()}, ["asset_path", "parameter"]), usage_topic="material_tracing", bridge_command="trace_material_parameter", output_schema=_output_schema("material_parameter_trace", {"trace": _any()})),
    "trace_output": _spec("Trace upstream graph feeding a material output.", _schema({"asset_path": _string(), "output": _string()} , ["asset_path"]), usage_topic="material_tracing", bridge_command="trace_material_output", output_schema=_output_schema("material_output_trace", {"trace": _any()})),
    "set_parameter": _spec(
        "Set a scalar, vector, or texture parameter on a Material or MaterialInstanceConstant.",
        _schema({
            "asset_path": _string(),
            "parameter": _string(),
            "type": {"type": "string", "enum": ["scalar", "vector", "texture"]},
            "value": _any(),
        }, ["asset_path", "parameter", "type", "value"]),
        usage_topic="material_patch",
        bridge_command="set_material_parameter",
        output_schema=_mutation_output_schema(),
    ),
    "patch_validate": _spec("Validate material patch operations without mutating the asset.", _schema({"asset_path": _string(), "ops": _array()}, ["asset_path", "ops"]), usage_topic="material_patch", bridge_command="validate_material_patch", output_schema=_output_schema("material_patch_validation", {"valid": _boolean(), "issues": _array()})),
    "patch_apply": _spec("Apply material patch operations through editor transactions.", _schema({"asset_path": _string(), "ops": _array()}, ["asset_path", "ops"]), usage_topic="material_patch", bridge_command="apply_material_patch", output_schema=_mutation_output_schema()),
    "search_nodes": _spec("Search the local Material node catalog.", _schema({"query": _string(), "category": _string(), "kind": _string(), "limit": _integer(25)}), usage_topic="material_catalog", output_schema=_counted_entries_schema("material_node_catalog_search")),
    "get_node": _spec("Get one Material node catalog entry by kind/display name.", _schema({"kind": _string()}, ["kind"]), usage_topic="material_catalog", output_schema=_output_schema("material_node_catalog_entry", {"entry": {"type": "object"}})),
    "list_node_categories": _spec("List Material node catalog categories.", usage_topic="material_catalog", output_schema=_output_schema("material_node_categories", {"categories": _array()})),
}


BLUEPRINT_ACTION_SPECS = {
    "read": _spec("Export compact Blueprint event/function/macro graphs.", _schema({"asset_path": _string()}, ["asset_path"]), usage_topic="blueprint_graph", bridge_command="export_blueprint_compact", output_schema=_output_schema("blueprint_compact_graph", {"session_id": _string(), "graphs": _array()})),
    "list_variables": _spec("List Blueprint member variables and defaults.", _schema({"asset_path": _string()}, ["asset_path"]), usage_topic="blueprint_graph", bridge_command="list_blueprint_variables", output_schema=_output_schema("blueprint_variable_list", {"variables": _array()})),
    "add_variable": _spec("Add a Blueprint member variable.", _schema({"asset_path": _string(), "variable_name": _string(), "type": _string(), "default": _string(), "category": _string()}, ["asset_path", "variable_name", "type"]), usage_topic="blueprint_graph", bridge_command="add_blueprint_member_variable", output_schema=_mutation_output_schema()),
    "set_variable_default": _spec("Set the default value string for a Blueprint member variable.", _schema({"asset_path": _string(), "variable_name": _string(), "default": _string()}, ["asset_path", "variable_name", "default"]), usage_topic="blueprint_graph", bridge_command="set_blueprint_variable_default"),
    "add_call_node": _spec("Add a function call node to an exported Blueprint graph alias.", _schema({"session_id": _string(), "graph": _string(), "function_path": _string(), "x": _integer(0), "y": _integer(0)}, ["session_id", "graph", "function_path"]), usage_topic="blueprint_graph", bridge_command="add_blueprint_function_call_node"),
    "add_custom_event": _spec("Add a custom event node to an exported Blueprint graph alias.", _schema({"session_id": _string(), "graph": _string(), "event_name": _string(), "x": _integer(0), "y": _integer(0)}, ["session_id", "graph", "event_name"]), usage_topic="blueprint_graph", bridge_command="add_blueprint_custom_event_node"),
    "add_variable_get": _spec("Add a Get variable node to an exported Blueprint graph alias.", _schema({"session_id": _string(), "graph": _string(), "variable_name": _string(), "x": _integer(0), "y": _integer(0)}, ["session_id", "graph", "variable_name"]), usage_topic="blueprint_graph", bridge_command="add_blueprint_variable_get_node"),
    "add_variable_set": _spec("Add a Set variable node to an exported Blueprint graph alias.", _schema({"session_id": _string(), "graph": _string(), "variable_name": _string(), "x": _integer(0), "y": _integer(0)}, ["session_id", "graph", "variable_name"]), usage_topic="blueprint_graph", bridge_command="add_blueprint_variable_set_node"),
    "set_pin_default": _spec("Set an input pin default value by node alias and pin name.", _schema({"session_id": _string(), "node": _string(), "pin": _string(), "default": _string()}, ["session_id", "node", "pin", "default"]), usage_topic="blueprint_graph", bridge_command="set_blueprint_pin_default"),
    "connect": _spec("Connect one Blueprint output pin to one input pin.", _schema({"session_id": _string(), "from_node": _string(), "from_pin": _string(), "to_node": _string(), "to_pin": _string()}, ["session_id", "from_node", "from_pin", "to_node", "to_pin"]), usage_topic="blueprint_graph", bridge_command="connect_blueprint_pins"),
    "disconnect": _spec("Break all links on one Blueprint pin.", _schema({"session_id": _string(), "node": _string(), "pin": _string()}, ["session_id", "node", "pin"]), usage_topic="blueprint_graph", bridge_command="disconnect_blueprint_pin"),
    "remove_node": _spec("Remove one Blueprint node by alias.", _schema({"session_id": _string(), "node": _string()}, ["session_id", "node"]), usage_topic="blueprint_graph", bridge_command="remove_blueprint_node"),
    "compile": _spec(
        "Compile a Blueprint asset after edits and return compiler diagnostics.",
        _schema({"asset_path": _string()}, ["asset_path"]),
        usage_topic="blueprint_graph",
        bridge_command="compile_blueprint",
        output_schema=_output_schema(
            "blueprint_compile_result",
            {
                "asset_path": _string(),
                "compiled": _boolean(),
                "success": _boolean(),
                "status": _string(),
                "error_count": _integer(),
                "warning_count": _integer(),
                "messages": _array(),
            },
        ),
    ),
}


NIAGARA_ACTION_SPECS = {
    "export": _spec("Export a Niagara System summary plus compact module graphs.", _schema({"asset_path": _string()}, ["asset_path"]), usage_topic="niagara_system", bridge_command="export_niagara_system", output_schema=_output_schema("niagara_system_export", {"session_id": _string(), "emitters": _array(), "system_stages": _array(), "compile": {"type": "object"}})),
    "diagnostics": _spec(
        "Read combined Niagara diagnostics: VM/script compile status plus stack/module dependency issues. Prefer this after edits.",
        _schema({"asset_path": _string(), "force": _boolean(True), "wait": _boolean(True)}, ["asset_path"]),
        usage_topic="niagara_system",
        bridge_command="get_niagara_diagnostics",
        output_schema=_output_schema("niagara_diagnostics", {"diagnostics": {"type": "object"}}),
    ),
    "compile_status": _spec(
        "Compile or read Niagara System script diagnostics after edits.",
        _schema({"asset_path": _string(), "force": _boolean(False), "wait": _boolean(True)}, ["asset_path"]),
        usage_topic="niagara_system",
        bridge_command="get_niagara_compile_status",
        output_schema=_output_schema("niagara_compile_status", {"diagnostics": {"type": "object"}}),
    ),
    "stack_issues": _spec(
        "Read Niagara stack/module dependency issues such as unmet module dependencies that may not appear as VM compile errors.",
        _schema({"asset_path": _string()}, ["asset_path"]),
        usage_topic="niagara_system",
        bridge_command="get_niagara_stack_issues",
        output_schema=_output_schema("niagara_stack_issues", {"diagnostics": {"type": "object"}}),
    ),
    "create_system": _spec("Create a Niagara System asset, optionally from a template system.", _schema({"package_path": _string(), "asset_name": _string(), "template_asset_path": _string()}, ["package_path", "asset_name"]), usage_topic="niagara_system", bridge_command="create_niagara_system", output_schema=_mutation_output_schema()),
    "add_emitter": _spec("Add an emitter asset to a Niagara System.", _schema({"system_asset_path": _string(), "emitter_asset_path": _string(), "emitter_name": _string()}, ["system_asset_path", "emitter_asset_path"]), usage_topic="niagara_system", bridge_command="add_niagara_emitter", output_schema=_mutation_output_schema()),
    "add_default_emitter": _spec("Add the Niagara editor Minimal Emitter to a system without searching for a template.", _schema({"system_asset_path": _string(), "emitter_name": _string()}, ["system_asset_path"]), usage_topic="niagara_system", bridge_command="add_niagara_default_emitter", output_schema=_mutation_output_schema()),
    "remove_user_parameter": _spec("Remove one exposed User parameter from a Niagara System. Does not automatically disconnect module links that reference the same User.* name.", _schema({"system_asset_path": _string(), "user_parameter": _string()}, ["system_asset_path", "user_parameter"]), usage_topic="niagara_system", bridge_command="remove_niagara_user_parameter", output_schema=_mutation_output_schema()),
    "set_emitter_sim_target": _spec("Set a Niagara emitter simulation target to CPU or GPU.", _schema({"system_asset_path": _string(), "emitter": _string(), "sim_target": _string()}, ["system_asset_path", "emitter", "sim_target"]), usage_topic="niagara_system", bridge_command="set_niagara_emitter_sim_target", output_schema=_mutation_output_schema()),
    "list_renderers": _spec("List renderer objects on an emitter with indexes, types, classes, and editable property snapshots.", _schema({"system_asset_path": _string(), "emitter": _string()}, ["system_asset_path", "emitter"]), usage_topic="niagara_system", bridge_command="list_niagara_renderers", output_schema=_output_schema("niagara_renderer_list", {"renderers": _array()})),
    "get_renderer_schema": _spec("Describe editable fields for a renderer type. Supported renderer_type values include sprite, mesh, ribbon, light, and component.", _schema({"renderer_type": _string()}, ["renderer_type"]), usage_topic="niagara_system", bridge_command="get_niagara_renderer_schema", output_schema=_output_schema("niagara_renderer_schema", {"editable_properties": _array()})),
    "add_renderer": _spec("Add a renderer to an emitter. Use renderer_type=sprite/mesh/ribbon/light/component; mesh renderers can pass mesh_asset_path.", _schema({"system_asset_path": _string(), "emitter": _string(), "renderer_type": _string(), "target_index": _integer(-1), "mesh_asset_path": _string()}, ["system_asset_path", "emitter", "renderer_type"]), usage_topic="niagara_system", bridge_command="add_niagara_renderer", output_schema=_mutation_output_schema()),
    "remove_renderer": _spec("Remove a renderer from an emitter by renderer_index.", _schema({"system_asset_path": _string(), "emitter": _string(), "renderer_index": _integer(0)}, ["system_asset_path", "emitter", "renderer_index"]), usage_topic="niagara_system", bridge_command="remove_niagara_renderer", output_schema=_mutation_output_schema()),
    "set_renderer_property": _spec("Set one exposed renderer property. Call get_renderer_schema first; value should use Unreal import-text for reflected properties.", _schema({"system_asset_path": _string(), "emitter": _string(), "renderer_index": _integer(0), "property": _string(), "value": _any()}, ["system_asset_path", "emitter", "renderer_index", "property", "value"]), usage_topic="niagara_system", bridge_command="set_niagara_renderer_property", output_schema=_mutation_output_schema()),
    "list_simulation_stages": _spec("List simulation stages on an emitter with indexes and editable property snapshots.", _schema({"system_asset_path": _string(), "emitter": _string()}, ["system_asset_path", "emitter"]), usage_topic="niagara_system", bridge_command="list_niagara_simulation_stages", output_schema=_output_schema("niagara_simulation_stage_list", {"simulation_stages": _array()})),
    "add_simulation_stage": _spec("Add a generic simulation stage to an emitter and create the simulation-stage graph output.", _schema({"system_asset_path": _string(), "emitter": _string(), "stage_name": _string(), "target_index": _integer(-1)}, ["system_asset_path", "emitter"]), usage_topic="niagara_system", bridge_command="add_niagara_simulation_stage", output_schema=_mutation_output_schema()),
    "remove_simulation_stage": _spec("Remove a simulation stage from an emitter by stage_index.", _schema({"system_asset_path": _string(), "emitter": _string(), "stage_index": _integer(0)}, ["system_asset_path", "emitter", "stage_index"]), usage_topic="niagara_system", bridge_command="remove_niagara_simulation_stage", output_schema=_mutation_output_schema()),
    "move_simulation_stage": _spec("Move a simulation stage to a target index.", _schema({"system_asset_path": _string(), "emitter": _string(), "stage_index": _integer(0), "target_index": _integer(0)}, ["system_asset_path", "emitter", "stage_index", "target_index"]), usage_topic="niagara_system", bridge_command="move_niagara_simulation_stage", output_schema=_mutation_output_schema()),
    "set_simulation_stage_property": _spec("Set one exposed simulation stage property. Use Unreal import-text values for reflected fields.", _schema({"system_asset_path": _string(), "emitter": _string(), "stage_index": _integer(0), "property": _string(), "value": _any()}, ["system_asset_path", "emitter", "stage_index", "property", "value"]), usage_topic="niagara_system", bridge_command="set_niagara_simulation_stage_property", output_schema=_mutation_output_schema()),
    "configure_sprite_renderer": _spec(
        "Compatibility wrapper for older clients. Prefer list_renderers/get_renderer_schema/add_renderer/remove_renderer/set_renderer_property.",
        _schema(
            {
                "system_asset_path": _string(),
                "emitter": _string(),
                "renderer_index": _integer(0),
                "facing_mode": _string(),
                "alignment": _string(),
                "pivot_u": _number(),
                "pivot_v": _number(),
            },
            ["system_asset_path", "emitter", "renderer_index", "facing_mode", "alignment", "pivot_u", "pivot_v"],
        ),
        usage_topic="niagara_system",
        bridge_command="configure_niagara_sprite_renderer",
        output_schema=_mutation_output_schema(),
    ),
    "search_module": _spec("Search native/plugin/project Niagara module scripts and merge semantic catalog notes.", _schema({"query": _string(), "usage": _string(), "source": _string(), "limit": _integer(20)}), usage_topic="niagara_system", bridge_command="search_niagara_modules", output_schema=_counted_entries_schema("niagara_module_search")),
    "add_module": _spec("Insert a Niagara module script into an exported stack alias.", _schema({"session_id": _string(), "target_stack": _string(), "script_asset_path": _string(), "target_index": _integer(-1), "suggested_name": _string()}, ["session_id", "target_stack", "script_asset_path"]), usage_topic="niagara_system", bridge_command="add_niagara_module", output_schema=_mutation_output_schema()),
    "create_local_module": _spec("Create a scratch/local Niagara module and insert it into an exported stack alias.", _schema({"session_id": _string(), "target_stack": _string(), "target_index": _integer(-1), "module_name": _string()}, ["session_id", "target_stack", "module_name"]), usage_topic="niagara_system", bridge_command="create_niagara_local_module"),
    "patch_module_graph": _spec("Patch an exported Niagara module internal graph. Supports add_node, add_dynamic_pin, connect, disconnect, set_custom_hlsl, and remove_node.", _schema({"session_id": _string(), "module": _string(), "ops": _array()}, ["session_id", "module", "ops"]), usage_topic="niagara_system", bridge_command="apply_niagara_module_graph_patch"),
    "remove_module": _spec("Remove a Niagara module by exported module alias.", _schema({"session_id": _string(), "module": _string()}, ["session_id", "module"]), usage_topic="niagara_system", bridge_command="remove_niagara_module"),
    "move_module": _spec("Move a Niagara module to a stack/index.", _schema({"session_id": _string(), "module": _string(), "target_stack": _string(), "target_index": _integer(0)}, ["session_id", "module", "target_stack"]), usage_topic="niagara_system", bridge_command="move_niagara_module"),
    "set_module_enabled": _spec("Enable or disable a Niagara module by alias.", _schema({"session_id": _string(), "module": _string(), "enabled": _boolean()}, ["session_id", "module", "enabled"]), usage_topic="niagara_system", bridge_command="set_niagara_module_enabled"),
    "list_module_inputs": _spec("List editable inputs for an exported Niagara module alias.", _schema({"session_id": _string(), "module": _string()}, ["session_id", "module"]), usage_topic="niagara_system", bridge_command="list_niagara_module_inputs", output_schema=_output_schema("niagara_module_inputs", {"inputs": _array()})),
    "get_module_input_override": _spec("Inspect a Niagara module input override.", _schema({"session_id": _string(), "module": _string(), "input": _string()}, ["session_id", "module", "input"]), usage_topic="niagara_system", bridge_command="get_niagara_module_input_override", output_schema=_output_schema("niagara_module_input_override", {"input": _string(), "override": _any()})),
    "set_module_input": _spec("Set a simple Niagara module input override.", _schema({"session_id": _string(), "module": _string(), "input": _string(), "value": _string()}, ["session_id", "module", "input", "value"]), usage_topic="niagara_system", bridge_command="set_niagara_module_input"),
    "set_static_switch": _spec("Set a Niagara module static switch pin, including hidden compile-time branch inputs such as Mesh Sampling Type.", _schema({"session_id": _string(), "module": _string(), "input": _string(), "value": _string()}, ["session_id", "module", "input", "value"]), usage_topic="niagara_system", bridge_command="set_niagara_static_switch", output_schema=_mutation_output_schema()),
    "set_module_object_input": _spec("Set a Niagara module object/data-interface input asset.", _schema({"session_id": _string(), "module": _string(), "input": _string(), "asset_path": _string()}, ["session_id", "module", "input", "asset_path"]), usage_topic="niagara_system", bridge_command="set_niagara_module_object_input"),
    "bind_module_input": _spec("Bind a Niagara module input to a User parameter.", _schema({"session_id": _string(), "module": _string(), "input": _string(), "user_parameter": _string(), "binding_kind": _string(), "default_asset_path": _string()}, ["session_id", "module", "input", "user_parameter"]), usage_topic="niagara_system", bridge_command="bind_niagara_module_input_to_user_param"),
    "search_module_semantics": _spec("Search the cached Niagara module semantic catalog.", _schema({"query": _string(), "limit": _integer(10)}), usage_topic="niagara_system", output_schema=_counted_entries_schema("niagara_module_semantic_search")),
    "get_module_semantics": _spec("Get one Niagara module semantic catalog entry by asset or name.", _schema({"asset": _string()}, ["asset"]), usage_topic="niagara_system", output_schema=_output_schema("niagara_module_semantic_entry", {"entry": {"type": "object"}})),
}


def register_action_tools(mcp: FastMCP) -> None:
    @mcp.tool()
    def system(action: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        """Unified ToolPlayMCP system tool. Actions: ping, list_tools, list_toolsets, describe_toolset, get_usage, get_selection, get_selected_graph_nodes."""
        return dispatch_action("system", action, params or {}, SYSTEM_ACTIONS)

    @mcp.tool()
    def asset(action: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        """Unified ToolPlayMCP asset/project tool. Actions: resolve_path, exists, list, search, save."""
        return dispatch_action("asset", action, params or {}, ASSET_ACTIONS)

    @mcp.tool()
    def material(action: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        """Unified Material tool. Actions include read, trace_output, set_parameter, patch_validate, patch_apply."""
        return dispatch_action("material", action, params or {}, MATERIAL_ACTIONS)

    @mcp.tool()
    def blueprint(action: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        """Unified Blueprint tool. Actions include read, add_call_node, connect, remove_node, compile."""
        return dispatch_action("blueprint", action, params or {}, BLUEPRINT_ACTIONS)

    @mcp.tool()
    def niagara(action: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        """Unified Niagara tool. Actions include export, search_module, add_module, create_local_module, patch_module_graph."""
        return dispatch_action("niagara", action, params or {}, NIAGARA_ACTIONS)
