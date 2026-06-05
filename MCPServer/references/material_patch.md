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

Before applying a patch or setting a parameter on an existing Material or Material Instance, state the exact asset path and intended edits, then get user approval. Prefer duplicating/copying an existing material and editing the copy when the user asks for a new look, prototype, or experiment. Do not save an existing material unless the user explicitly approved saving that exact edit.

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

## Technical Patterns

Use this section as the stable Material graph editing playbook. Prefer adding precise, reusable patterns here over relying on one-off agent memory.

### Custom HLSL Nodes

`MaterialExpressionCustom` Code is inserted into Unreal's generated material shader. Treat the `Code`/`code` property as a snippet/body, not as a standalone `.usf` file.

Use expression/body snippets:

```hlsl
float edge = smoothstep(0.45, 0.5, UV.x);
return lerp(ColorA, ColorB, edge);
```

or, when using Additional Outputs, assign those output variables and still return the main output:

```hlsl
float n = sin(UV.x * Frequency);
OutMask = saturate(n * 0.5 + 0.5);
return float3(OutMask, OutMask, OutMask);
```

Do not paste full shader files, global includes, function definitions, entry points, or material attribute declarations into a Custom node unless the Unreal Material Custom node documentation for the target engine version explicitly supports that exact form:

```hlsl
float3 my_custom_func(float2 UV)
{
    return float3(UV, 0);
}
```

Why: Custom node code is embedded inside generated material HLSL. Full function definitions or global declarations can be emitted inside another function and trigger shader compile errors similar to `function definition is not allowed here`.

When patching Material Custom nodes through ToolPlayMCP:

- Query `material(action="get_node", params={"kind":"MaterialExpressionCustom"})` and `get_material_node_config_schema` before creating or mutating the node.
- Use valid HLSL identifiers for Custom input names and additional output names, such as `UV`, `Frequency`, `ColorA`, `OutMask`.
- Do not use material output names such as `BaseColor` or `Roughness` as Custom node input/output variable names.
- Connect the Custom node output to material pins such as `root.BaseColor`, `root.EmissiveColor`, or `root.Opacity`; the Custom node itself does not write material outputs by name.
- After Custom HLSL edits, compile/check the material in Unreal before claiming success. ToolPlayMCP material diagnostics are not yet as complete as Niagara shader diagnostics.

## Current Boundaries

- Existing Materials and Material Instances are read-only until the user explicitly approves a write to that asset in the current conversation.
- `patch_validate`, graph export, trace, catalog search, and config reads are safe read-only actions. `patch_apply`, `set_material_parameter`, and `asset(action="save")` are write actions and require approval for existing assets.
- Material Instance graph topology edits are not supported; use `set_parameter` on instances.
- Base Material graph edits support compact aliases from a fresh export plus `temp_id` nodes created in the same patch.
- Material mutation tools must keep using `FScopedTransaction` and call `Modify()` on changed assets, expressions, and parameters before mutation so editor `Ctrl+Z` can undo the full patch.
- Property assignment is reflection-based and intentionally limited to simple scalar, bool, string/name, object path, and `LinearColor` values.
- Use `get_material_node_config(asset_path, node)` to inspect a node's exposed current config.
- Use `get_material_node_config_schema(kind)` before setting unfamiliar node properties.
- Query node metadata with `material(action="search_nodes")` or `material(action="get_node")` before constructing `add_node`.
- Query `describe_material_function_interface` before creating or connecting `MaterialExpressionMaterialFunctionCall`.
