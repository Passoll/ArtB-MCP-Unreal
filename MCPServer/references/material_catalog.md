# Material Catalog And Function Interface Tools

ToolPlayMCP separates static node knowledge from project asset knowledge.

## Static Node Catalog

Prefer targeted lookup tools before planning material graph edits:

- `material(action="search_nodes", params={"query":"...","limit":10})`: fuzzy search by display name, kind, category, or notes.
- `get_material_node(kind)`: retrieve exactly one entry by class name, compact kind, or display name.
- `list_material_node_categories()`: inspect available categories without loading entries.

`material(action="search_nodes", params={"query":"","category":"","kind":"","limit":25})` can filter the catalog, but do not request the full catalog unless debugging the catalog itself.

The catalog returns stable AI-facing entries:

- `kind`: Unreal material expression class name, such as `MaterialExpressionMultiply`.
- `compact_kind`: compact graph kind, such as `*` or `lerp`.
- `display`: human-friendly node label.
- `create_class`: class path to use in future patch requests.
- `inputs`: real input pin names plus compact aliases.
- `outputs`: real output pin names plus compact aliases.
- `properties`: editable properties that are safe enough for patch planning.
- `notes`: deterministic usage notes and common caveats.

Do not invent class names, pin names, or property names when creating patch operations. Query the relevant catalog entry first.

## Material Function Interface

Use `describe_material_function_interface(function_path)` before creating, connecting, or interpreting a `MaterialExpressionMaterialFunctionCall`.

It returns:

- `asset`, `path`, `class`, `usage`
- `inputs`: function input names, compact names, input types, preview defaults, descriptions
- `outputs`: function output names, compact names, descriptions
- `dependencies`: dependent material function paths when UE exposes them

Use this when a compact graph contains a `func` node and the user asks what the function contributes.

## Future Domains

Niagara should use the same pattern:

- Static module catalog for common module metadata.
- Project asset interface/query tools for real Niagara Systems, Emitters, and Modules.
- Patch tools should validate against the relevant catalog before applying changes.
