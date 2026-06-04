# Material Tracing Tools

These tools return deterministic graph slices for AI context. They do not infer artistic intent or generate semantic explanations.

## Tools

- `list_material_functions(asset_path)`: lists compact nodes whose kind is `func`.
- `trace_material_output(asset_path, output="BaseColor")`: walks upstream from a material output pin such as `BaseColor`.
- `trace_material_parameter(asset_path, parameter)`: walks downstream from every compact node whose `label` matches the parameter name.

Use Unreal asset object paths or package object paths that UE can load, for example:

```text
/Game/LevelPrototyping/Materials/M_PrototypeGrid.M_PrototypeGrid
/Script/Engine.Material'/Game/LevelPrototyping/Materials/M_PrototypeGrid.M_PrototypeGrid'
```

## Output Shape

Trace tools return:

- `asset`: material asset name.
- `direction`: `upstream` for output traces or `downstream` for parameter traces.
- `nodes`: compact node subset keyed by aliases such as `n12` and `root`.
- `edges`: raw compact edges in the same tuple form as compact export: `[fromNode, fromPin, toNode, toPin]`.
- `chains`: ordered endpoint chains such as `["root.BaseColor", "n12.rgb", "n3.out"]`.

`chains` are deterministic path views over `edges`. They are meant to be easier to read than unordered edge lists while keeping the original edge facts available.

## Compact Node Rules

- `func` nodes are MaterialFunctionCall expressions. The `label` is the called function asset name when available.
- Ordinary reroute-like nodes are transparent in chains when possible, but may still appear in `nodes` and `edges` as factual graph data.
- No Unreal GUIDs are exposed in trace output. Alias-to-expression bindings live only in the editor session.
- Values under `v` are defaults or material instance overrides captured by the exporter, not AI interpretations.

## Suggested Usage

For broad inspection, call `material(action="read")`.

For targeted questions:

- Use `trace_material_output(..., "BaseColor")` when the user asks what controls color.
- Use `trace_material_output(..., "Roughness")` or another material property for property-specific questions.
- Use `trace_material_parameter(..., "Line Dimensions")` when the user asks why changing a named parameter does or does not affect the final material.
- Use `list_material_functions` before deciding whether a compact graph should be expanded in a future tool.
