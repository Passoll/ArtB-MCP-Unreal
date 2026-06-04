# Blueprint Graph Tools

`blueprint(action="read", params={"asset_path":"..."})` returns compact Blueprint graph context.

The exporter covers:

- Event graphs from `UbergraphPages`.
- Function graphs from `FunctionGraphs`.
- Macro graphs from `MacroGraphs`.
- Node aliases such as `g0.n3`.
- Graph aliases such as `g0`, with `type` set to `event`, `function`, or `macro`.
- Compact edges as `[from_node, from_pin, to_node, to_pin]`.
- Member variables with compact type/default/category metadata.

Aliases are editor-session handles. Re-export after structural edits.

## Workflow

1. Call `blueprint(action="read", params={"asset_path":"..."})`.
2. Inspect graph aliases and node aliases.
3. Add variables or add/edit/connect/remove nodes through Blueprint tools.
4. Re-export after structural edits to refresh aliases.
5. Call `compile_blueprint(asset_path)`.
6. Call `asset(action="save", params={"asset_path":"..."})`.

## Tools

`blueprint(action="read", params={"asset_path":"..."})`

Export compact graph data for event, function, and macro graphs.

`list_blueprint_variables(asset_path)`

List member variables and their compact type/default/category metadata.

`add_blueprint_member_variable(asset_path, variable_name, type, default="", category="")`

Add a Blueprint member variable. Supported type strings include `bool`, `byte`, `int`, `int64`, `float`, `double`, `name`, `string`, `text`, `vector`, `rotator`, `transform`, `linear_color`, `object:/Script/Engine.Actor`, `class:/Script/Engine.Actor`, and `[]` arrays such as `float[]`.

`set_blueprint_variable_default(asset_path, variable_name, default)`

Set the default value string stored on a Blueprint member variable.

`add_blueprint_function_call_node(session_id, graph, function_path, x=0, y=0)`

Add a `UK2Node_CallFunction` to a graph alias. `function_path` accepts forms such as `/Script/Engine.KismetSystemLibrary:PrintString`.

`add_blueprint_custom_event_node(session_id, graph, event_name, x=0, y=0)`

Add a custom event node to an event graph.

`add_blueprint_variable_get_node(session_id, graph, variable_name, x=0, y=0)`

Add a Get member variable node to a graph alias.

`add_blueprint_variable_set_node(session_id, graph, variable_name, x=0, y=0)`

Add a Set member variable node to a graph alias.

`set_blueprint_pin_default(session_id, node, pin, default)`

Set an input pin default string. Use exported pin names exactly.

`connect_blueprint_pins(session_id, from_node, from_pin, to_node, to_pin)`

Connect one output pin to one input pin. The engine schema validates the connection.

`disconnect_blueprint_pin(session_id, node, pin)`

Break all links on a pin.

`remove_blueprint_node(session_id, node)`

Remove one node by alias.

`compile_blueprint(asset_path)`

Compile after edits. Save separately with `asset(action="save")`.

## Editing Rules

- Do not invent aliases; export first.
- Re-export after adding or removing nodes.
- Add member variables before spawning Get/Set variable nodes.
- Use the engine schema to connect pins, not raw edge JSON.
- Compile before saving if behavior changed.
