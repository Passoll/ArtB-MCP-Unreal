#include "ToolRegistry/ToolPlayMCPToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const FString EmptyObjectSchema = TEXT("{\"type\":\"object\",\"properties\":{}}");

	FString Schema(const FString& Properties, const FString& Required = FString())
	{
		FString Result = TEXT("{\"type\":\"object\",\"properties\":{") + Properties + TEXT("}");
		if (!Required.IsEmpty())
		{
			Result += TEXT(",\"required\":[") + Required + TEXT("]");
		}
		Result += TEXT("}");
		return Result;
	}

	FString StringProp()
	{
		return TEXT("{\"type\":\"string\"}");
	}

	FString NumberProp()
	{
		return TEXT("{\"type\":\"number\"}");
	}

	FString IntegerProp()
	{
		return TEXT("{\"type\":\"integer\"}");
	}

	FString BooleanProp()
	{
		return TEXT("{\"type\":\"boolean\"}");
	}

	FString ObjectProp()
	{
		return TEXT("{\"type\":\"object\"}");
	}

	FString ArrayProp()
	{
		return TEXT("{\"type\":\"array\"}");
	}

	FString AnyProp()
	{
		return TEXT("{}");
	}

	FString Required(std::initializer_list<const TCHAR*> Names)
	{
		TArray<FString> Quoted;
		for (const TCHAR* Name : Names)
		{
			Quoted.Add(FString::Printf(TEXT("\"%s\""), Name));
		}
		return FString::Join(Quoted, TEXT(","));
	}

	FString Prop(const TCHAR* Name, const FString& Json)
	{
		return FString::Printf(TEXT("\"%s\":%s"), Name, *Json);
	}
}

const TArray<FToolPlayMCPBridgeCommandSpec>& FToolPlayMCPToolRegistry::GetCommandSpecs()
{
	static const TArray<FToolPlayMCPBridgeCommandSpec> Specs = {
		{TEXT("ping"), TEXT("system"), TEXT("Verify that the UE bridge is responding."), TEXT("{}"), EmptyObjectSchema, TEXT("")},
		{TEXT("list_tools"), TEXT("system"), TEXT("List bridge commands with descriptions and input schemas."), TEXT("{}"), EmptyObjectSchema, TEXT("tool_registry")},
		{TEXT("list_toolsets"), TEXT("system"), TEXT("List available ToolPlayMCP toolsets/domains."), TEXT("{}"), EmptyObjectSchema, TEXT("tool_registry")},
		{TEXT("describe_toolset"), TEXT("system"), TEXT("Describe one ToolPlayMCP toolset/domain and its tools."), TEXT("{\"domain\":\"material\"}"), Schema(Prop(TEXT("domain"), StringProp()), Required({TEXT("domain")})), TEXT("tool_registry")},
		{TEXT("get_selection"), TEXT("graph"), TEXT("Read focused graph node selection, falling back to Content Browser selected assets."), TEXT("{}"), EmptyObjectSchema, TEXT("")},
		{TEXT("get_selected_graph_nodes"), TEXT("graph"), TEXT("Read currently selected nodes from the focused GraphEditor panel."), TEXT("{}"), EmptyObjectSchema, TEXT("")},
		{TEXT("save_asset"), TEXT("asset"), TEXT("Save a loaded Unreal asset package by asset path."), TEXT("{\"asset_path\":\"/Game/Path/Asset.Asset\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("")},

		{TEXT("export_blueprint_compact"), TEXT("blueprint"), TEXT("Export compact Blueprint graphs including event, function, and macro graphs."), TEXT("{\"asset_path\":\"/Game/Path/BP_Actor.BP_Actor\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("blueprint_graph")},
		{TEXT("list_blueprint_variables"), TEXT("blueprint"), TEXT("List Blueprint member variables and default values."), TEXT("{\"asset_path\":\"/Game/Path/BP_Actor.BP_Actor\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("blueprint_graph")},
		{TEXT("add_blueprint_member_variable"), TEXT("blueprint"), TEXT("Add a Blueprint member variable. Supported types include bool, int, float, string, vector, rotator, transform, linear_color, object:/Script/Engine.Actor, and [] arrays."), TEXT("{\"asset_path\":\"/Game/Path/BP_Actor.BP_Actor\",\"variable_name\":\"Speed\",\"type\":\"float\",\"default\":\"600.0\",\"category\":\"AI\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("variable_name"), StringProp()), Prop(TEXT("type"), StringProp()), Prop(TEXT("default"), StringProp()), Prop(TEXT("category"), StringProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("variable_name"), TEXT("type")})), TEXT("blueprint_graph")},
		{TEXT("set_blueprint_variable_default"), TEXT("blueprint"), TEXT("Set the default value string for a Blueprint member variable."), TEXT("{\"asset_path\":\"/Game/Path/BP_Actor.BP_Actor\",\"variable_name\":\"Speed\",\"default\":\"900.0\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("variable_name"), StringProp()), Prop(TEXT("default"), StringProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("variable_name"), TEXT("default")})), TEXT("blueprint_graph")},
		{TEXT("add_blueprint_function_call_node"), TEXT("blueprint"), TEXT("Add a function call node to an exported Blueprint graph alias."), TEXT("{\"session_id\":\"...\",\"graph\":\"g0\",\"function_path\":\"/Script/Engine.KismetSystemLibrary:PrintString\",\"x\":0,\"y\":0}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("graph"), StringProp()), Prop(TEXT("function_path"), StringProp()), Prop(TEXT("x"), IntegerProp()), Prop(TEXT("y"), IntegerProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("graph"), TEXT("function_path")})), TEXT("blueprint_graph")},
		{TEXT("add_blueprint_custom_event_node"), TEXT("blueprint"), TEXT("Add a custom event node to an exported Blueprint graph alias."), TEXT("{\"session_id\":\"...\",\"graph\":\"g0\",\"event_name\":\"MyEvent\",\"x\":0,\"y\":0}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("graph"), StringProp()), Prop(TEXT("event_name"), StringProp()), Prop(TEXT("x"), IntegerProp()), Prop(TEXT("y"), IntegerProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("graph"), TEXT("event_name")})), TEXT("blueprint_graph")},
		{TEXT("add_blueprint_variable_get_node"), TEXT("blueprint"), TEXT("Add a Get member variable node to an exported Blueprint graph alias."), TEXT("{\"session_id\":\"...\",\"graph\":\"g0\",\"variable_name\":\"Speed\",\"x\":0,\"y\":0}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("graph"), StringProp()), Prop(TEXT("variable_name"), StringProp()), Prop(TEXT("x"), IntegerProp()), Prop(TEXT("y"), IntegerProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("graph"), TEXT("variable_name")})), TEXT("blueprint_graph")},
		{TEXT("add_blueprint_variable_set_node"), TEXT("blueprint"), TEXT("Add a Set member variable node to an exported Blueprint graph alias."), TEXT("{\"session_id\":\"...\",\"graph\":\"g0\",\"variable_name\":\"Speed\",\"x\":0,\"y\":0}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("graph"), StringProp()), Prop(TEXT("variable_name"), StringProp()), Prop(TEXT("x"), IntegerProp()), Prop(TEXT("y"), IntegerProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("graph"), TEXT("variable_name")})), TEXT("blueprint_graph")},
		{TEXT("set_blueprint_pin_default"), TEXT("blueprint"), TEXT("Set an input pin default value by node alias and pin name."), TEXT("{\"session_id\":\"...\",\"node\":\"g0.n1\",\"pin\":\"In String\",\"default\":\"Hello\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("node"), StringProp()), Prop(TEXT("pin"), StringProp()), Prop(TEXT("default"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("node"), TEXT("pin"), TEXT("default")})), TEXT("blueprint_graph")},
		{TEXT("connect_blueprint_pins"), TEXT("blueprint"), TEXT("Connect one Blueprint output pin to one input pin."), TEXT("{\"session_id\":\"...\",\"from_node\":\"g0.n0\",\"from_pin\":\"then\",\"to_node\":\"g0.n1\",\"to_pin\":\"execute\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("from_node"), StringProp()), Prop(TEXT("from_pin"), StringProp()), Prop(TEXT("to_node"), StringProp()), Prop(TEXT("to_pin"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("from_node"), TEXT("from_pin"), TEXT("to_node"), TEXT("to_pin")})), TEXT("blueprint_graph")},
		{TEXT("disconnect_blueprint_pin"), TEXT("blueprint"), TEXT("Break all links on one Blueprint pin."), TEXT("{\"session_id\":\"...\",\"node\":\"g0.n1\",\"pin\":\"execute\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("node"), StringProp()), Prop(TEXT("pin"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("node"), TEXT("pin")})), TEXT("blueprint_graph")},
		{TEXT("remove_blueprint_node"), TEXT("blueprint"), TEXT("Remove a Blueprint node by exported node alias."), TEXT("{\"session_id\":\"...\",\"node\":\"g0.n1\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("node"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("node")})), TEXT("blueprint_graph")},
		{TEXT("compile_blueprint"), TEXT("blueprint"), TEXT("Compile a Blueprint asset after edits and return compiler diagnostics."), TEXT("{\"asset_path\":\"/Game/Path/BP_Actor.BP_Actor\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("blueprint_graph")},

		{TEXT("export_material_compact"), TEXT("material"), TEXT("Export an AI-ready compact graph for a Material asset."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("material_compact_graph")},
		{TEXT("create_material_asset"), TEXT("material"), TEXT("Create a new Material asset."), TEXT("{\"package_path\":\"/Game/Materials\",\"asset_name\":\"M_New\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("package_path"), StringProp()), Prop(TEXT("asset_name"), StringProp())}, TEXT(",")), Required({TEXT("package_path"), TEXT("asset_name")})), TEXT("material_patch")},
		{TEXT("list_material_functions"), TEXT("material"), TEXT("List Material Function calls used by a material or instance."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("material_catalog")},
		{TEXT("describe_material_function_interface"), TEXT("material"), TEXT("Describe a Material Function's inputs and outputs."), TEXT("{\"function_path\":\"/Game/Path/MF_Function.MF_Function\"}"), Schema(Prop(TEXT("function_path"), StringProp()), Required({TEXT("function_path")})), TEXT("material_catalog")},
		{TEXT("get_material_node_config"), TEXT("material"), TEXT("Read editable config for a compact material node alias."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\",\"node\":\"n0\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("node"), StringProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("node")})), TEXT("material_patch")},
		{TEXT("get_material_node_config_schema"), TEXT("material"), TEXT("Read editable property schema for a material node kind/class."), TEXT("{\"kind\":\"TextureSample\"}"), Schema(Prop(TEXT("kind"), StringProp()), Required({TEXT("kind")})), TEXT("material_patch")},
		{TEXT("trace_material_parameter"), TEXT("material"), TEXT("Trace graph usage from a named material parameter."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\",\"parameter\":\"Color\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("parameter"), StringProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("parameter")})), TEXT("material_tracing")},
		{TEXT("trace_material_output"), TEXT("material"), TEXT("Trace upstream graph feeding a material output."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\",\"output\":\"BaseColor\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("output"), StringProp())}, TEXT(",")), Required({TEXT("asset_path")})), TEXT("material_tracing")},
		{TEXT("set_material_parameter"), TEXT("material"), TEXT("Set a scalar, vector, or texture parameter on a Material or MaterialInstanceConstant."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\",\"parameter\":\"Tint\",\"type\":\"vector\",\"value\":[1,0,0,1]}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("parameter"), StringProp()), TEXT("\"type\":{\"type\":\"string\",\"enum\":[\"scalar\",\"vector\",\"texture\"]}"), Prop(TEXT("value"), AnyProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("parameter"), TEXT("type"), TEXT("value")})), TEXT("material_patch")},
		{TEXT("validate_material_patch"), TEXT("material"), TEXT("Validate material patch operations without mutating the asset."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\",\"ops\":[]}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("ops"), ArrayProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("ops")})), TEXT("material_patch")},
		{TEXT("apply_material_patch"), TEXT("material"), TEXT("Apply material patch operations through editor transactions."), TEXT("{\"asset_path\":\"/Game/Path/M_Material.M_Material\",\"ops\":[]}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("ops"), ArrayProp())}, TEXT(",")), Required({TEXT("asset_path"), TEXT("ops")})), TEXT("material_patch")},

		{TEXT("export_niagara_system"), TEXT("niagara"), TEXT("Export a Niagara System summary plus compact module graphs."), TEXT("{\"asset_path\":\"/Game/Path/NS_System.NS_System\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("niagara_system")},
		{TEXT("get_niagara_compile_status"), TEXT("niagara"), TEXT("Compile or read Niagara System script diagnostics."), TEXT("{\"asset_path\":\"/Game/Path/NS_System.NS_System\",\"force\":false,\"wait\":true}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("force"), BooleanProp()), Prop(TEXT("wait"), BooleanProp())}, TEXT(",")), Required({TEXT("asset_path")})), TEXT("niagara_system")},
		{TEXT("get_niagara_stack_issues"), TEXT("niagara"), TEXT("Read Niagara stack/module dependency issues such as unmet module dependencies that may not appear as VM compile errors."), TEXT("{\"asset_path\":\"/Game/Path/NS_System.NS_System\"}"), Schema(Prop(TEXT("asset_path"), StringProp()), Required({TEXT("asset_path")})), TEXT("niagara_system")},
		{TEXT("get_niagara_diagnostics"), TEXT("niagara"), TEXT("Read combined Niagara diagnostics: VM/script compile status plus stack/module dependency issues."), TEXT("{\"asset_path\":\"/Game/Path/NS_System.NS_System\",\"force\":true,\"wait\":true}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("asset_path"), StringProp()), Prop(TEXT("force"), BooleanProp()), Prop(TEXT("wait"), BooleanProp())}, TEXT(",")), Required({TEXT("asset_path")})), TEXT("niagara_system")},
		{TEXT("create_niagara_system"), TEXT("niagara"), TEXT("Create a Niagara System asset, optionally from a template system."), TEXT("{\"package_path\":\"/Game/FX\",\"asset_name\":\"NS_New\",\"template_asset_path\":\"\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("package_path"), StringProp()), Prop(TEXT("asset_name"), StringProp()), Prop(TEXT("template_asset_path"), StringProp())}, TEXT(",")), Required({TEXT("package_path"), TEXT("asset_name")})), TEXT("niagara_system")},
		{TEXT("add_niagara_emitter"), TEXT("niagara"), TEXT("Add an emitter asset to a Niagara System."), TEXT("{\"system_asset_path\":\"/Game/FX/NS_System.NS_System\",\"emitter_asset_path\":\"/Game/FX/NE_Emitter.NE_Emitter\",\"emitter_name\":\"\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("system_asset_path"), StringProp()), Prop(TEXT("emitter_asset_path"), StringProp()), Prop(TEXT("emitter_name"), StringProp())}, TEXT(",")), Required({TEXT("system_asset_path"), TEXT("emitter_asset_path")})), TEXT("niagara_system")},
		{TEXT("add_niagara_default_emitter"), TEXT("niagara"), TEXT("Add the Niagara editor Minimal Emitter to a system without requiring the AI to search for an emitter template."), TEXT("{\"system_asset_path\":\"/Game/FX/NS_System.NS_System\",\"emitter_name\":\"Emitter\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("system_asset_path"), StringProp()), Prop(TEXT("emitter_name"), StringProp())}, TEXT(",")), Required({TEXT("system_asset_path")})), TEXT("niagara_system")},
		{TEXT("set_niagara_emitter_sim_target"), TEXT("niagara"), TEXT("Set a Niagara emitter sim target to CPU or GPU."), TEXT("{\"system_asset_path\":\"/Game/FX/NS_System.NS_System\",\"emitter\":\"e0\",\"sim_target\":\"GPU\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("system_asset_path"), StringProp()), Prop(TEXT("emitter"), StringProp()), Prop(TEXT("sim_target"), StringProp())}, TEXT(",")), Required({TEXT("system_asset_path"), TEXT("emitter"), TEXT("sim_target")})), TEXT("niagara_system")},
		{TEXT("configure_niagara_sprite_renderer"), TEXT("niagara"), TEXT("Configure a Niagara sprite renderer's facing, alignment, and pivot UV."), TEXT("{\"system_asset_path\":\"/Game/FX/NS_System.NS_System\",\"emitter\":\"e0\",\"renderer_index\":0,\"facing_mode\":\"FaceCamera\",\"alignment\":\"VelocityAligned\",\"pivot_u\":0.5,\"pivot_v\":1.0}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("system_asset_path"), StringProp()), Prop(TEXT("emitter"), StringProp()), Prop(TEXT("renderer_index"), IntegerProp()), Prop(TEXT("facing_mode"), StringProp()), Prop(TEXT("alignment"), StringProp()), Prop(TEXT("pivot_u"), NumberProp()), Prop(TEXT("pivot_v"), NumberProp())}, TEXT(",")), Required({TEXT("system_asset_path"), TEXT("emitter"), TEXT("renderer_index"), TEXT("facing_mode"), TEXT("alignment"), TEXT("pivot_u"), TEXT("pivot_v")})), TEXT("niagara_system")},
		{TEXT("search_niagara_modules"), TEXT("niagara"), TEXT("Search native/plugin/project Niagara module scripts."), TEXT("{\"query\":\"spawn\",\"usage\":\"module\",\"source\":\"all\",\"limit\":20}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("query"), StringProp()), Prop(TEXT("usage"), StringProp()), Prop(TEXT("source"), StringProp()), Prop(TEXT("limit"), IntegerProp())}, TEXT(","))), TEXT("niagara_system")},
		{TEXT("add_niagara_module"), TEXT("niagara"), TEXT("Insert a Niagara module script into an exported stack alias."), TEXT("{\"session_id\":\"...\",\"target_stack\":\"e0.particle_update\",\"script_asset_path\":\"/Niagara/Modules/Particles/...\",\"target_index\":-1,\"suggested_name\":\"\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("target_stack"), StringProp()), Prop(TEXT("script_asset_path"), StringProp()), Prop(TEXT("target_index"), IntegerProp()), Prop(TEXT("suggested_name"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("target_stack"), TEXT("script_asset_path")})), TEXT("niagara_system")},
		{TEXT("create_niagara_local_module"), TEXT("niagara"), TEXT("Create a scratch/local Niagara module and insert it into an exported stack alias."), TEXT("{\"session_id\":\"...\",\"target_stack\":\"e0.particle_update\",\"target_index\":-1,\"module_name\":\"MCP_LocalRule\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("target_stack"), StringProp()), Prop(TEXT("target_index"), IntegerProp()), Prop(TEXT("module_name"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("target_stack"), TEXT("module_name")})), TEXT("niagara_system")},
		{TEXT("apply_niagara_module_graph_patch"), TEXT("niagara"), TEXT("Patch an exported Niagara module's internal graph. Supports add_node, add_dynamic_pin, connect, disconnect, set_custom_hlsl, and remove_node."), TEXT("{\"session_id\":\"...\",\"module\":\"e0.particle_update.m1\",\"ops\":[]}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("ops"), ArrayProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("ops")})), TEXT("niagara_system")},
		{TEXT("remove_niagara_module"), TEXT("niagara"), TEXT("Remove a Niagara module by exported module alias."), TEXT("{\"session_id\":\"...\",\"module\":\"e0.particle_update.m1\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module")})), TEXT("niagara_system")},
		{TEXT("move_niagara_module"), TEXT("niagara"), TEXT("Move a Niagara module to a stack/index."), TEXT("{\"session_id\":\"...\",\"module\":\"e0.particle_update.m1\",\"target_stack\":\"e0.particle_update\",\"target_index\":0}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("target_stack"), StringProp()), Prop(TEXT("target_index"), IntegerProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("target_stack")})), TEXT("niagara_system")},
		{TEXT("set_niagara_module_enabled"), TEXT("niagara"), TEXT("Enable or disable a Niagara module by alias."), TEXT("{\"session_id\":\"...\",\"module\":\"e0.particle_update.m1\",\"enabled\":true}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("enabled"), BooleanProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("enabled")})), TEXT("niagara_system")},
		{TEXT("list_niagara_module_inputs"), TEXT("niagara"), TEXT("List editable inputs for an exported Niagara module alias."), TEXT("{\"session_id\":\"...\",\"module\":\"m0\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module")})), TEXT("niagara_system")},
		{TEXT("get_niagara_module_input_override"), TEXT("niagara"), TEXT("Inspect a Niagara module input override."), TEXT("{\"session_id\":\"...\",\"module\":\"m0\",\"input\":\"Color\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("input"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("input")})), TEXT("niagara_system")},
		{TEXT("set_niagara_module_input"), TEXT("niagara"), TEXT("Set a simple Niagara module input override."), TEXT("{\"session_id\":\"...\",\"module\":\"m0\",\"input\":\"Color\",\"value\":\"(R=1,G=0,B=0,A=1)\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("input"), StringProp()), Prop(TEXT("value"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("input"), TEXT("value")})), TEXT("niagara_system")},
		{TEXT("set_niagara_static_switch"), TEXT("niagara"), TEXT("Set a Niagara module static switch pin, including hidden compile-time branch inputs such as Mesh Sampling Type."), TEXT("{\"session_id\":\"...\",\"module\":\"m0\",\"input\":\"Mesh Sampling Type\",\"value\":\"Surface (Triangles)\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("input"), StringProp()), Prop(TEXT("value"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("input"), TEXT("value")})), TEXT("niagara_system")},
		{TEXT("set_niagara_module_object_input"), TEXT("niagara"), TEXT("Set a Niagara module object/data-interface input asset."), TEXT("{\"session_id\":\"...\",\"module\":\"m0\",\"input\":\"Texture\",\"asset_path\":\"/Game/Path/T_Texture.T_Texture\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("input"), StringProp()), Prop(TEXT("asset_path"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("input"), TEXT("asset_path")})), TEXT("niagara_system")},
		{TEXT("bind_niagara_module_input_to_user_param"), TEXT("niagara"), TEXT("Bind a Niagara module input to a User parameter. Use binding_kind=volume_texture for Volume Texture inputs or binding_kind=skeletal_mesh for Skeletal Mesh data-interface inputs."), TEXT("{\"session_id\":\"...\",\"module\":\"m0\",\"input\":\"Texture\",\"user_parameter\":\"User.Texture\",\"binding_kind\":\"auto\",\"default_asset_path\":\"\"}"), Schema(FString::Join(TArray<FString>{Prop(TEXT("session_id"), StringProp()), Prop(TEXT("module"), StringProp()), Prop(TEXT("input"), StringProp()), Prop(TEXT("user_parameter"), StringProp()), Prop(TEXT("binding_kind"), StringProp()), Prop(TEXT("default_asset_path"), StringProp())}, TEXT(",")), Required({TEXT("session_id"), TEXT("module"), TEXT("input"), TEXT("user_parameter")})), TEXT("niagara_system")}
	};
	return Specs;
}

TArray<FToolPlayMCPToolsetSpec> FToolPlayMCPToolRegistry::GetToolsets()
{
	return {
		{TEXT("system"), TEXT("Bridge, registry, usage, and runtime introspection commands.")},
		{TEXT("asset"), TEXT("Asset-level project operations such as saving loaded packages.")},
		{TEXT("graph"), TEXT("Focused editor graph and Content Browser selection queries.")},
		{TEXT("material"), TEXT("Material graph export, tracing, patching, parameter edits, and catalog-oriented inspection.")},
		{TEXT("blueprint"), TEXT("Blueprint graph export, variable operations, node edits, pin edits, connection edits, and compile.")},
		{TEXT("niagara"), TEXT("Niagara system export, catalog search, stack/module edits, module graph patching, and input/user parameter operations.")}
	};
}

TSharedRef<FJsonObject> FToolPlayMCPToolRegistry::CommandSpecToJson(const FToolPlayMCPBridgeCommandSpec& Spec)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("name"), Spec.Name);
	Object->SetStringField(TEXT("domain"), Spec.Domain);
	Object->SetStringField(TEXT("description"), Spec.Description);
	Object->SetStringField(TEXT("params_example"), Spec.ParamsExampleJson);
	if (!Spec.UsageTopic.IsEmpty())
	{
		Object->SetStringField(TEXT("usage_topic"), Spec.UsageTopic);
	}
	if (TSharedPtr<FJsonObject> InputSchema = ParseObject(Spec.InputSchemaJson))
	{
		Object->SetObjectField(TEXT("input_schema"), InputSchema.ToSharedRef());
	}
	return Object;
}

TSharedRef<FJsonObject> FToolPlayMCPToolRegistry::BuildToolsetsJson()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Toolsets;
	for (const FToolPlayMCPToolsetSpec& Toolset : GetToolsets())
	{
		int32 Count = 0;
		for (const FToolPlayMCPBridgeCommandSpec& Spec : GetCommandSpecs())
		{
			if (Spec.Domain == Toolset.Domain)
			{
				Count++;
			}
		}

		TSharedRef<FJsonObject> ToolsetObject = MakeShared<FJsonObject>();
		ToolsetObject->SetStringField(TEXT("domain"), Toolset.Domain);
		ToolsetObject->SetStringField(TEXT("description"), Toolset.Description);
		ToolsetObject->SetNumberField(TEXT("tool_count"), Count);
		Toolsets.Add(MakeShared<FJsonValueObject>(ToolsetObject));
	}
	Root->SetArrayField(TEXT("toolsets"), Toolsets);
	return Root;
}

bool FToolPlayMCPToolRegistry::BuildToolsetDescriptionJson(const FString& Domain, TSharedRef<FJsonObject>& OutJson, FString& OutError)
{
	FString Description;
	for (const FToolPlayMCPToolsetSpec& Toolset : GetToolsets())
	{
		if (Toolset.Domain.Equals(Domain, ESearchCase::IgnoreCase))
		{
			Description = Toolset.Description;
			break;
		}
	}

	if (Description.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Unknown toolset/domain: %s"), *Domain);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const FToolPlayMCPBridgeCommandSpec& Spec : GetCommandSpecs())
	{
		if (Spec.Domain.Equals(Domain, ESearchCase::IgnoreCase))
		{
			Tools.Add(MakeShared<FJsonValueObject>(CommandSpecToJson(Spec)));
		}
	}

	OutJson->SetStringField(TEXT("domain"), Domain);
	OutJson->SetStringField(TEXT("description"), Description);
	OutJson->SetArrayField(TEXT("tools"), Tools);
	OutJson->SetNumberField(TEXT("tool_count"), Tools.Num());
	return true;
}

TSharedPtr<FJsonObject> FToolPlayMCPToolRegistry::ParseObject(const FString& Json)
{
	if (Json.IsEmpty())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
	{
		return Object;
	}
	return nullptr;
}
