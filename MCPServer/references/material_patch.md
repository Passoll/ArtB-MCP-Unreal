# Material Patch Protocol

Material patch tools use one operation array:

```json
{
  "asset_path": "/Game/Materials/M_Test.M_Test",
  "ops": []
}
```

Use `material(action="patch_validate", params={"asset_path":"...","ops":[...]})` before `material(action="patch_apply", params={"asset_path":"...","ops":[...]})`.

Validation does not modify assets. Apply runs through the Unreal editor transaction system and marks the asset dirty when successful.

## Supported First-Version Ops

### set_parameter

Works on base `UMaterial` and `UMaterialInstanceConstant`.

```json
{
  "op": "set_parameter",
  "parameter": "Roughness",
  "value_type": "scalar",
  "value": 0.5
}
```

Vector parameter:

```json
{
  "op": "set_parameter",
  "parameter": "Tint",
  "value_type": "vector",
  "value": [1, 0.2, 0.1, 1]
}
```

Texture parameter:

```json
{
  "op": "set_parameter",
  "parameter": "NoiseTexture",
  "value_type": "texture",
  "value": "/Game/Textures/T_Noise.T_Noise"
}
```

For one-off parameter edits, prefer `set_material_parameter(asset_path, parameter, type, value)`. The direct tool builds and applies the same `set_parameter` patch internally. Use `type` values such as `scalar`, `vector`, or `texture`.

### add_node

Works on base `UMaterial` only. Use `get_material_node` first and pass `create_class`.

```json
{
  "op": "add_node",
  "temp_id": "new_multiply",
  "node": {
    "create_class": "/Script/Engine.MaterialExpressionMultiply",
    "position": [400, 200],
    "properties": {}
  }
}
```

`temp_id` is only valid inside the current patch request, so later ops can connect the new node.

### connect

Connects compact aliases or `temp_id` nodes. `root` means material output.

```json
{
  "op": "connect",
  "from": {"node": "new_multiply", "pin": "out"},
  "to": {"node": "root", "pin": "BaseColor"}
}
```

Input connections are single-owner in material graphs; connecting to an occupied input replaces that input connection.

### disconnect

Disconnects an input endpoint.

```json
{
  "op": "disconnect",
  "to": {"node": "root", "pin": "BaseColor"}
}
```

### move_node

Moves an existing compact alias or same-patch `temp_id` node in the material editor.

```json
{
  "op": "move_node",
  "node": "n12",
  "position": [600, 120]
}
```

### set_node_property

Sets reflected editable properties on an existing node alias or same-patch `temp_id`.

Call `get_material_node_config_schema(kind)` first when you do not know the exact property names or types.

```json
{
  "op": "set_node_property",
  "node": "n9",
  "properties": {
    "Period": 0.75
  }
}
```

## Current Boundaries

- Material Instance graph topology edits are not supported; use `set_parameter` on instances.
- Base Material graph edits support compact aliases from a fresh export plus `temp_id` nodes created in the same patch.
- Property assignment is reflection-based and intentionally limited to simple scalar, bool, string/name, object path, and `LinearColor` values.
- Use `get_material_node_config(asset_path, node)` to inspect a node's exposed current config.
- Use `get_material_node_config_schema(kind)` before setting unfamiliar node properties.
- Query node metadata with `material(action="search_nodes")` or `material(action="get_node")` before constructing `add_node`.
- Query `describe_material_function_interface` before creating or connecting `MaterialExpressionMaterialFunctionCall`.
