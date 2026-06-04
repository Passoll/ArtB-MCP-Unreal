# Catalog Governance

ToolPlayMCP catalogs are AI-facing knowledge databases, not free-form notes. They compress Unreal editor concepts into stable context that tools can search, explain, and reuse.

Do not let an agent directly rewrite catalog files without following this workflow.

## Update Workflow

1. Identify the catalog owner and domain in `catalogs/catalog_domains.json`.
2. Decide whether the change is a correction, a new entry, or a schema migration.
3. Prefer small patches. Update one domain and one concept group at a time.
4. Keep raw Unreal object ids, transient graph aliases, GUIDs, editor-only pointer names, and local session ids out of catalogs.
5. Record only stable names, asset paths, input names, output names, written attributes, side effects, common edits, pitfalls, and usage notes.
6. Validate catalogs with `python Plugins/ToolPlayMCP/MCPServer/scripts/validate_catalogs.py`.
7. Run Python AST validation after changing MCP tool code.
8. If a schema changes, update the matching usage reference and tool merge/search code in the same patch.

## Approval Rules

- `allowed`: Add a missing stable module/node entry with known asset path and observed inputs.
- `allowed`: Fix wrong semantics after verifying through Unreal export, module input inspection, or official engine source.
- `allowed`: Add pitfalls discovered from actual failed edits.
- `review`: Add broad semantic claims such as performance cost, GPU behavior, or stack order requirements.
- `review`: Rename schema fields or remove legacy-compatible fields.
- `blocked`: Invent native module asset paths without `search_module` or AssetRegistry confirmation.
- `blocked`: Store transient aliases such as `e0.particle_update.m3`, `n14`, GUIDs, or UObject pointer strings.
- `blocked`: Replace a catalog wholesale when a targeted patch would work.

## Niagara Semantic Entry Shape

`niagara_modules.semantic.json` uses a stable AI-facing structure:

- `asset`: stable Niagara script asset path when known.
- `name`: display/search name.
- `summary`: one or two sentences explaining what the module is for.
- `semantic_tags`: compact searchable tags.
- `preferred_stacks`: likely stack locations, such as `particle_spawn` or `particle_update`.
- `inputs`: important inputs with compact type, purpose, and normal usage.
- `outputs`: values the module contributes to downstream execution.
- `writes`: Niagara attributes or parameter-map values the module mutates.
- `side_effects`: behavior not obvious from pin lists, such as force accumulation, event generation, or renderer-visible changes.
- `input_value_kinds`: accepted value forms, for example literal, linked user parameter, dynamic input, object asset, or data interface.
- `key_inputs`: legacy-compatible short input list for older prompts.
- `critical_inputs`: inputs that commonly decide whether the module actually does what the user asked.
- `critical_static_switches`: hidden or mode-selecting inputs that can route the module to a different internal branch.
- `stack_requirements`: ordering, stack, or companion-module requirements.
- `required_followups`: checks the AI should perform after adding or editing the module.
- `common_edits`: common user goals and which inputs normally implement them.
- `pitfalls`: common reasons edits appear to do nothing or produce misleading results.
- `notes`: compact operational guidance.

## Quality Bar

Each catalog entry should answer three questions without needing a full graph dump:

- What does this node/module change?
- Which inputs are safe or useful to edit?
- What can make the edit appear ineffective?

If an entry cannot answer those questions, leave it out or mark it as incomplete in notes instead of pretending the catalog knows more than it does.

For Niagara modules with hidden static switches, catalog entries should explicitly name the switch and the safe value families. Do not rely on generic names like "Sampling Mode" when the actual module commonly exposes "Mesh Sampling Type" or another version-specific branch selector.

## Relation To Runtime Tools

Catalogs are advisory. Runtime state still comes from Unreal through export, list-input, trace, and validation tools.

Use this order:

1. Catalog for discovery and semantics.
2. Export/list/trace tool for current asset state.
3. Mutation tool for the actual change.
4. Re-export or validation tool for confirmation.
