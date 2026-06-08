# Niagara System Tools

`export_niagara_system(asset_path)` returns an AI-oriented Niagara System summary.

The exporter prefers Niagara's authored structure over raw graph dumps:

- `system_stages`: System Spawn and System Update scripts.
- `emitters`: ordered emitter handles with enabled state, sim target, renderers, event handlers, and simulation stages.
- `stages`: per-emitter Emitter Spawn, Emitter Update, Particle Spawn, and Particle Update scripts.
- `modules`: ordered `UNiagaraNodeFunctionCall` entries found through graph traversal.
- `parameter_maps`: compact read/write traversal hints from Niagara Parameter Map history.
- `compact_graph`: bounded node/edge graph for debugging and future writeback.

The root includes a `session_id`. Module ids such as `e0.particle_update.m0` and graph node ids such as `e0.particle_update.m0.g0` are local aliases stored in plugin memory for future writeback. They do not expose Unreal GUIDs or UObject internals.

Module graphs are intentionally compressed. `Reroute` nodes are skipped in compact output. If a question only needs one visual output or parameter, prefer future trace tools over sending the full compact graph.

## Export Scope

- `UNiagaraSystem` assets only.
- Module internals are summarized as bounded compact graphs, not full raw editor graphs.
- Stack/module export and mutation are routed through `FToolPlayMCPNiagaraModuleService`. It currently uses graph traversal as a fallback where Niagara's stack utilities do not expose linkable public symbols.

## Common Workflow

1. Create or load a system with `create_niagara_system` or an existing asset path.
2. For a from-scratch effect, add an emitter with `niagara(action="add_default_emitter")`; it uses the Niagara editor Minimal Emitter and avoids template hunting.
3. Add emitters with `add_niagara_emitter` only when the user explicitly wants to reuse an existing emitter asset.
4. Call `export_niagara_system(asset_path)` to get `session_id`, stack aliases, and module aliases.
5. Before adding any module, call `niagara(action="search_module", params={"query":"...","usage":"module"})` and use only a `script_asset_path` returned by that search or by a documented recipe.
6. Read the search result's `summary`, `inputs`, `outputs`, `writes`, `side_effects`, `input_value_kinds`, `critical_inputs`, `critical_static_switches`, `stack_requirements`, `required_followups`, `common_edits`, `preferred_stacks`, `pitfalls`, and `notes` before adding the module.
7. Add/remove/move/enable modules by alias, then re-export because aliases are volatile after structural edits.
8. Inspect the added module with `niagara(action="list_module_inputs")`; do not set inputs that were not found by search semantics or by this input list.
9. Mutate parameters with set/bind tools. If `list_module_inputs` marks an input as `source="static_switch_pin"` or `is_static_switch=true`, use `niagara(action="set_static_switch")` instead of `set_module_input`.
10. Call `niagara(action="diagnostics", params={"asset_path":"...","force":true,"wait":true})` after edits and inspect `diagnostics.summary.ok_to_save_or_claim_success`, `blocking_reasons`, `compile`, and `stack`.
11. Use `compile_status` or `stack_issues` only when debugging one diagnostic layer in isolation.
12. Save explicitly with `asset(action="save", params={"asset_path":"..."})` only after explicit save approval.

For existing Niagara assets, steps that mutate the system require user approval first. State the exact asset path, intended edits, and why direct modification is needed. Prefer creating a new Niagara System or duplicating an existing one when the user asks for a new effect, prototype, or variant.

Example:

```text
export_niagara_system("/Game/Fx/MySystem.MySystem")
niagara(action="add_default_emitter", params={"system_asset_path":"/Game/Fx/MySystem.MySystem","emitter_name":"Emitter"})
niagara(action="export", params={"asset_path":"/Game/Fx/MySystem.MySystem"})
niagara(action="add_module", params={"session_id":session_id,"target_stack":"e0.particle_update","script_asset_path":"/Niagara/Modules/Particles/Update/Forces/GravityForce.GravityForce","target_index":-1})
niagara(action="export", params={"asset_path":"/Game/Fx/MySystem.MySystem"})
niagara(action="list_module_inputs", params={"session_id":session_id,"module":"e0.particle_update.m1"})
niagara(action="get_module_input_override", params={"session_id":session_id,"module":"e0.particle_update.m1","input":"Some Input"})
niagara(action="set_static_switch", params={"session_id":session_id,"module":"e0.particle_spawn.m1","input":"Mesh Sampling Type","value":"Surface (Triangles)"})
niagara(action="bind_module_input", params={"session_id":session_id,"module":"e0.particle_update.m1","input":"Some Input","user_parameter":"User.SomeInput"})
niagara(action="bind_module_input", params={"session_id":session_id,"module":"e0.particle_update.m1","input":"Noise Texture","user_parameter":"User.NoiseTexture","binding_kind":"volume_texture","default_asset_path":"/Engine/Path/T_Volume.T_Volume"})
niagara(action="diagnostics", params={"asset_path":"/Game/Fx/MySystem.MySystem","force":true,"wait":true})
asset(action="save", params={"asset_path":"/Game/Fx/MySystem.MySystem"})
```

## Tools

`niagara(action="search_module", params={"query":"","usage":"module","source":"all","limit":20})`

Discover addable Niagara scripts. The tool uses Niagara Editor's filtered script asset API, so default results are library-visible, non-deprecated scripts that should correspond to Niagara's Add menu. MCP search results are enriched with cached semantic notes, stack fit, normalized inputs, outputs, writes, side effects, critical inputs/static switches, stack requirements, required followups, common edits, pitfalls, and usage notes when available.

Always search before `niagara(action="add_module")`. Do not guess old module names or hand-write script asset paths. If a module is missing from search, do not force an unrelated module into the stack.

`export_niagara_system(asset_path)`

Export system stages, emitters, renderers, event handlers, simulation stages, ordered modules, compact module graphs, compile diagnostics, and session aliases.

Each exported module includes `input_summary`. If `input_summary.requires_input_review` is true, inspect `input_summary.hidden_static_switches` and call `niagara(action="list_module_inputs")` before changing or explaining the module. Hidden static switches can change which internal branch executes; for example, `SkeletalMeshLocation` may sample bones/sockets instead of mesh surface unless `Mesh Sampling Type` is explicitly set to a surface/triangle/vertex mode.

`niagara(action="diagnostics", params={"asset_path":"...","force":true,"wait":true})`

Read combined Niagara diagnostics. This is the recommended post-edit validation tool. It returns:

- `summary`: compact gate for AI workflows, including `ok_to_save_or_claim_success`, `has_errors`, `blocking_reasons`, compile counts, and stack issue counts.
- `compile`: VM/script compile diagnostics for system scripts, emitter scripts, event handlers, and simulation stages. GPU HLSL compiler failures are returned per script as `shader_compile_errors` and also counted in `compile_errors`.
- `stack`: stack/module dependency diagnostics, including unmet module dependencies that may not appear as VM compile errors.

Niagara mutation tools request compilation but do not wait for or include the final compile result in the mutation response. Treat mutation success as "the edit was applied and compile was requested", not as "the system compiled successfully". Always call diagnostics with `force=true` and `wait=true` before saving or claiming success.

`niagara(action="compile_status", params={"asset_path":"...","force":false,"wait":true})`

Read only Niagara System compile diagnostics in a compact, AI-readable shape. Prefer `diagnostics` after edits; use this when debugging compile state only. Set `wait=true` to wait for pending script/GPU compilation before reading status. Set `force=true` when the system may have stale compile data and you need Unreal to request a fresh compile.

The response includes:

- `diagnostics.status`: worst script status across system scripts, emitter scripts, event handlers, and simulation stages.
- `diagnostics.compile_ready`: false when scripts are still unknown, dirty, or being created.
- `diagnostics.has_errors`, `diagnostics.has_warnings`, and `diagnostics.has_blocking_status`: quick gates for automated repair loops.
- `diagnostics.scripts[]`: one entry per script with `scope`, `emitter`, `stage`, `usage`, `status`, `compile_errors`, and `shader_compile_errors`.

`niagara(action="stack_issues", params={"asset_path":"..."})`

Read only stack/module dependency diagnostics. This catches editor stack issues such as "The module has unmet dependencies" that are generated by Niagara's stack dependency validation and may not appear as VM compile errors. Prefer `diagnostics` after edits; use this when debugging stack state only.

`create_niagara_system(package_path, asset_name, template_asset_path="")`

Create a Niagara System asset. Without a template it creates the normal default system graph. With `template_asset_path`, it duplicates that Niagara System as the starting point.

Do not search project files for a template unless the user explicitly asks to reuse an existing system. For common effects, prefer a documented recipe; for custom rules, prefer local/scratch module tooling instead of ad-hoc template hunting.

`add_niagara_emitter(system_asset_path, emitter_asset_path, emitter_name="")`

Add an existing Niagara Emitter asset to a system. Re-export after adding to get `e0.*` stack aliases. This tool does not create emitters from scratch and should not trigger a project-wide search for "similar" emitters unless the user requested reuse.

`niagara(action="add_default_emitter", params={"system_asset_path":"...","emitter_name":"Emitter"})`

Add the Niagara editor Minimal Emitter configured in `UNiagaraEditorSettings::DefaultEmptyEmitter`. Use this for from-scratch systems instead of searching for a random emitter template. Re-export after adding to get `e0.*` stack aliases. If the project has no Minimal Emitter configured, the tool reports a clear error and the user should configure Project Settings > Plugins > Niagara > Minimal Emitter or explicitly provide an emitter asset.

`niagara(action="set_emitter_sim_target", params={"system_asset_path":"...","emitter":"e0","sim_target":"GPU"})`

Set one exported emitter's simulation target to CPU or GPU. Accepted `sim_target` values include `CPU`, `CPUSim`, `GPU`, and `GPUComputeSim`. This edits emitter data, uses an editor transaction, marks the system dirty, and requests a compile. Re-export and run diagnostics after changing sim target because module support, renderer behavior, and data access can differ between CPU and GPU simulation.

`niagara(action="remove_user_parameter", params={"system_asset_path":"...","user_parameter":"User.SourceMesh"})`

Remove one exposed `User.*` parameter from a Niagara System. The parameter name may be passed as `SourceMesh` or `User.SourceMesh`. This does not automatically disconnect module input links that reference the same `User.*` name; export or inspect module input overrides first if you need to clean references.

`niagara(action="list_renderers", params={"system_asset_path":"...","emitter":"e0"})`

List renderer objects on an emitter. The response includes renderer index, class, compact type, and editable property snapshots. Use this before changing renderer type or properties; renderer indexes are emitter-local and can change after add/remove/reorder operations.

`niagara(action="get_renderer_schema", params={"renderer_type":"mesh"})`

Describe the safe editable fields for a renderer type. Supported `renderer_type` values currently include `sprite`, `mesh`, `ribbon`, `light`, and `component`. Do not guess renderer C++ property names; call this first and only pass properties returned by the schema.

`niagara(action="add_renderer", params={"system_asset_path":"...","emitter":"e0","renderer_type":"mesh","target_index":-1,"mesh_asset_path":"/Engine/BasicShapes/Sphere.Sphere"})`

Add a renderer to an emitter. `renderer_type` can be `sprite`, `mesh`, `ribbon`, `light`, or `component`. `mesh_asset_path` is optional and only applies to Mesh Renderer; it initializes the first mesh slot with a `UStaticMesh`. Re-export and call diagnostics after adding a renderer.

`niagara(action="remove_renderer", params={"system_asset_path":"...","emitter":"e0","renderer_index":0})`

Remove one renderer by index. This is a structural edit and should be followed by re-export.

`niagara(action="set_renderer_property", params={"system_asset_path":"...","emitter":"e0","renderer_index":0,"property":"FacingMode","value":"Velocity"})`

Set one exposed renderer property. For reflected fields, `value` uses Unreal import-text strings, such as enum names (`Velocity`, `FaceCamera`) or struct strings (`(X=0.5,Y=1.0)`). Special property `mesh_asset` sets the first Mesh Renderer mesh slot from a `UStaticMesh` asset path.

`niagara(action="configure_sprite_renderer", params={...})`

Compatibility wrapper for older clients. Prefer the unified renderer workflow above.

`niagara(action="list_simulation_stages", params={"system_asset_path":"...","emitter":"e0"})`

List simulation stages on an emitter, including index, class, name, enabled state, script usage id, and editable properties.

`niagara(action="add_simulation_stage", params={"system_asset_path":"...","emitter":"e0","stage_name":"Advect","target_index":-1})`

Add a generic simulation stage to an emitter and create the corresponding `ParticleSimulationStageScript` graph output. Simulation stages are structure-level emitter objects, not ordinary modules. Re-export after adding so the new stage stack appears with fresh aliases.

`niagara(action="remove_simulation_stage", params={"system_asset_path":"...","emitter":"e0","stage_index":0})`

Remove a simulation stage by index.

`niagara(action="move_simulation_stage", params={"system_asset_path":"...","emitter":"e0","stage_index":0,"target_index":1})`

Move a simulation stage to a new index.

`niagara(action="set_simulation_stage_property", params={"system_asset_path":"...","emitter":"e0","stage_index":0,"property":"bEnabled","value":"False"})`

Set one exposed simulation stage property. Use `list_simulation_stages` first; values use Unreal import-text strings for reflected fields.

`niagara(action="add_module", params={"session_id":"...","target_stack":"...","script_asset_path":"...","target_index":-1,"suggested_name":""})`

Insert a Niagara module script into a stack alias such as `system.system_update`, `e0.particle_spawn`, or `e0.particle_update`. Use only script paths returned by `niagara(action="search_module")`, a documented recipe, or an existing exported system. Re-export after insertion to get the new module alias, then call `niagara(action="list_module_inputs")` before setting parameters.

`niagara(action="create_local_module", params={"session_id":"...","target_stack":"...","target_index":-1,"module_name":"MCP_LocalModule"})`

Create a Niagara scratch/local module script, add it to the owning system's scratch pad, and insert it into a stack alias. Use this when the requested behavior is custom and cannot be expressed cleanly with library-visible modules. Re-export immediately after creation to get stable module and graph aliases.

`niagara(action="patch_module_graph", params={"session_id":"...","module":"...","ops":[...]})`

Patch an exported Niagara module's internal graph. Supported operation types:

- `add_node`: Add `custom_hlsl`, `op`, or `function_call` nodes. `custom_hlsl` accepts `hlsl`; optional `script_usage` can be `function` or `dynamic_input`; optional `output_type` is used when `script_usage="dynamic_input"`. `op` accepts `op_name`; `function_call` accepts `script_asset_path`.
- `add_node`: Also supports `parameter_map_set` for writing values back to the Niagara parameter map.
- `add_dynamic_pin`: Add a named input or output pin to a Niagara dynamic-pin node, including `parameter_map_set` and `custom_hlsl`. Fields are `node`, `direction`, `name`, and `value_type`. Common `value_type` values are `float`, `bool`, `int`, `vec2`, `vec3`, `position`, `vec4`, `color`, `quat`, and `parameter_map`. For `parameter_map_set`, use fully namespaced attribute names such as `Particles.Color`; do not use bare names such as `Color`.
- `connect`: Connect one output pin to one input pin with `from_node`, `from_pin`, `to_node`, and `to_pin`.
- `disconnect`: Break one specified edge with the same pin fields as `connect`.
- `set_custom_hlsl`: Update a `custom_hlsl` node with `node` and `hlsl`.
- `remove_node`: Delete a graph node alias with `node`.

This is intentionally a small protocol, not a raw UObject editor. Re-export after patching so the next AI turn sees the current graph and fresh aliases. Use graph aliases from `export_niagara_system`; if you just created a local module, re-export before detailed internal edits.

Example local-module writeback pattern:

```json
{
  "ops": [
    {"type":"add_node","node_kind":"parameter_map_set","alias":"write","x":400,"y":0},
    {"type":"add_dynamic_pin","node":"write","direction":"input","name":"Particles.Color","value_type":"color"},
    {"type":"connect","from_node":"color_calc","from_pin":"Color","to_node":"write","to_pin":"Particles.Color"}
  ]
}
```

Use `parameter_map_set` when the goal is to assign Niagara attributes such as `Particles.Color`, `Particles.Position`, `Particles.Velocity`, custom `Particles.*` values, or `Emitter.*` values. Use `custom_hlsl` for computing values, then write the result through `parameter_map_set`; a custom HLSL node by itself does not mutate particle attributes.

For `custom_hlsl` nodes, every dynamic pin must be reflected in the node function signature. ToolPlayMCP refreshes this after `add_dynamic_pin`, but pin names still need to be valid HLSL identifiers such as `ColorOut`, `Fresnel`, or `FollowPosition`; do not use names with spaces, dots, or namespace separators on Custom HLSL pins. Namespaced Niagara attributes belong on `parameter_map_set` pins, not Custom HLSL pins.

Custom HLSL text is inserted into Niagara-generated shader code. Do not paste a full function definition such as `void fresnel_calc(...) { ... }` into a Custom HLSL node unless the engine context explicitly supports that form. Prefer assignment/body snippets such as `ColorOut = ...; SizeOut = ...;`. After editing GPU Niagara code, run diagnostics and inspect both `compile_errors` and `shader_compile_errors`; GPU HLSL errors may reference `/Engine/Generated/NiagaraEmitterInstance.ush`.

## Parameter Namespace Rules

Niagara parameter namespaces are semantic, not decorative. Do not move a name between namespaces because it "sounds close".

- Use `Particles.*` for per-particle attributes. Examples: `Particles.Age`, `Particles.NormalizedAge`, `Particles.Position`, `Particles.Velocity`, `Particles.Color`, `Particles.SpriteSize`, and `Particles.MeshScale`.
- Use `Emitter.*` for emitter-scope values shared by the emitter.
- Use `System.*` for system-scope values shared by the whole Niagara System.
- Use `User.*` only for exposed user parameters intended to be set from the system/component/user.
- Use `Engine.*` only for engine/script execution context values such as `Engine.DeltaTime` or execution metadata. `Engine.DeltaTime` works as a global context value, but `Engine.ExecutionIndex` is not a substitute for a particle attribute such as `Particles.Age` or `Particles.Position`.
- Use `Owner.*` only for owner/component context values.
- In module graphs and `parameter_map_set`, fully qualify particle attributes. Use `Particles.Age`, not `Age`; use `Particles.Position`, not `Engine.Position`, `Owner.Position`, or `System.Position`.
- If the user asks for "particle age", "particle position", "per-particle color", or "each particle's velocity", choose the `Particles.*` namespace unless an exported module input explicitly says otherwise.
- Stack context values adapt by script usage, but that does not mean `Engine`, `Owner`, or `System` can read per-particle attributes. For particle data, use `Particles.*` or an explicit module output/assignment that produces a particle attribute.
- `ExecutionIndex` is an execution/thread index-style context value, not the same as particle ID, particle age, or particle array index. Do not use it to fetch particle attributes unless the module/API explicitly documents that workflow.
- Do not read `Engine.ExecutionIndex` from Parameter Map with a parameter-map get node. That pattern is wrong for ToolPlayMCP-generated Niagara graphs and commonly produces missing/invalid values.
- If a module needs execution index, use Niagara's built-in `Execution Index` graph node/function in the module graph, or expose a module input such as `Module.ExecutionIndex`/`Input.ExecutionIndex` and set it from the stack or an outer caller. Treat this as a computed script-context value, not a namespaced parameter-map variable.
- If the goal is stable per-particle identity, ordering, or randomization, prefer explicit particle attributes such as `Particles.ID`, `Particles.UniqueID`, `Particles.RandomSeed`, or a custom `Particles.*` value when available. Do not substitute `Engine.ExecutionIndex` for those.

Wrong:

```text
ParameterMapGet("Engine.ExecutionIndex") -> use as particle index
```

Right:

```text
Execution Index node/function -> local calculation
Module input "ExecutionIndex" -> set externally when the stack/module API exposes it
Particles.ID or custom Particles.* attribute -> stable particle identity/randomization
```

`remove_niagara_module(session_id, module)`

Remove a Niagara module by exported module alias. This uses a minimal parameter-map chain edit and then requires re-export.

`move_niagara_module(session_id, module, target_stack, target_index)`

Move a module structurally. Current implementation inserts the same module script at the target and removes the old node, so input overrides may need to be reapplied after re-export.

`set_niagara_module_enabled(session_id, module, enabled)`

Enable or disable a Niagara module by exported module alias. Re-export to verify final stack state.

`niagara(action="list_module_inputs", params={"session_id":"...","module":"..."})`

List editable stack inputs for one exported module alias. Input names may include short names such as `Velocity` or full names such as `Module.Velocity`. The response includes `value_kind`, enum values when available, static-switch hints, hidden state, override pin defaults, link counts, and pin type metadata. For modules like `SampleSkeletalMesh`, inspect this output before changing mesh sampling behavior so inputs such as `Mesh Sampling Type` are not missed.

`get_niagara_module_input_override(session_id, module, input)`

Inspect the current override pin, linked nodes, object asset, data interface, and special fields such as `volume_texture` and `texture_user_parameter`.

`set_niagara_module_input(session_id, module, input, value)`

Set a simple default string on a module input override pin. Prefer binding tools for linked user parameters or object/data-interface values. The result includes `confirmed_override_pin`, `stored_default_value`, and `default_value_exact_match`; if the tool reports failure or cannot confirm the override pin, do not assume the value was applied.

`niagara(action="set_static_switch", params={"session_id":"...","module":"...","input":"Mesh Sampling Type","value":"Surface (Triangles)"})`

Set a Niagara module static switch pin. Use this for compile-time branch selectors returned by `list_module_inputs` with `source="static_switch_pin"` or `is_static_switch=true`, including hidden switches such as `Mesh Sampling Type`. Values may be enum display names, enum internal names, or numeric enum values when available. Re-export immediately after setting a static switch because the active internal branch can change.

`set_niagara_module_object_input(session_id, module, input, asset_path)`

Set an object/data-interface module input by asset path. For `UNiagaraDataInterfaceVolumeTexture`, this also updates the DI's internal `Texture` pointer when possible.

`bind_niagara_module_input_to_user_param(session_id, module, input, user_parameter, binding_kind="auto", default_asset_path="")`

Expose a module input as a linked `User.*` parameter. For ordinary scalar/vector/color values this creates the exposed user parameter if missing and links the module input through a `ParameterMapGet`. For `UNiagaraDataInterfaceVolumeTexture`, pass `binding_kind="volume_texture"` or provide `default_asset_path`; this keeps the module input as a Volume Texture DI, sets `TextureUserParameter`, and optionally assigns a default texture asset.

`search_niagara_module_semantics(query="", limit=10)` and `get_niagara_module_semantics(asset)`

Query cached semantic notes for common/native Niagara modules without expanding full module graphs.

Semantic catalog entries use the following AI-facing shape:

- `inputs`: editable or important module inputs with compact type, purpose, and normal usage.
- `outputs`: values the module contributes to downstream Niagara execution.
- `writes`: Niagara attributes or parameter-map values the module mutates.
- `side_effects`: runtime behavior that is not obvious from a pin list, such as spawning, event generation, or force accumulation.
- `input_value_kinds`: accepted value styles such as literal, linked user parameter, dynamic input, object asset, or data interface.
- `critical_inputs`: inputs that decide whether the module actually satisfies the user goal.
- `critical_static_switches`: hidden or mode-selecting inputs that may choose a completely different internal branch.
- `stack_requirements`: companion modules, stack placement, or ordering constraints.
- `required_followups`: checks to run after adding or mutating the module.
- `common_edits`: high-level editing recipes and the input names usually involved.
- `pitfalls`: common reasons an edit appears to do nothing or creates misleading output.

## Catalog Values

Supported `usage` values:

- `module`
- `dynamic_input`
- `function`

Supported `source` values:

- `all`
- `native`
- `project`
- `plugin`

## Editing Rules

- Existing Niagara Systems and Emitters are read-only until the user explicitly approves a write to that asset in the current conversation.
- Prefer creating a new Niagara System or duplicating/copying an existing system before edits. Do not modify an existing effect just because it can satisfy the request after changes.
- Export, diagnostics, compile status, stack issue checks, module search, semantic search, and input listing are read-only. Create/add/remove/move/patch/set/bind/save actions are writes and require approval for existing assets.
- Always export first; do not invent module aliases.
- For from-scratch systems, prefer `niagara(action="add_default_emitter")` over searching for an emitter template.
- Use `set_emitter_sim_target` for CPU/GPU simulation target changes instead of trying to patch module graphs.
- Use the unified renderer workflow for renderer edits: `list_renderers`, `get_renderer_schema`, `add_renderer`, `remove_renderer`, and `set_renderer_property`. Do not use renderer-specific one-off tools unless maintaining an old client.
- Use the simulation-stage workflow for stage edits: `list_simulation_stages`, `add_simulation_stage`, `remove_simulation_stage`, `move_simulation_stage`, and `set_simulation_stage_property`.
- Always search before adding a Niagara module; do not invent script asset paths.
- Treat `niagara(action="search_module")` as the Add-menu source of truth. If a module does not appear there, assume it is unavailable, hidden, obsolete, deprecated, or the wrong usage unless proven otherwise.
- Read `inputs`, `outputs`, `writes`, `side_effects`, `input_value_kinds`, `critical_inputs`, `critical_static_switches`, `stack_requirements`, `required_followups`, `common_edits`, `pitfalls`, and `notes` in search results before adding or setting parameters.
- Treat exported `module.input_summary.requires_input_review=true` as a hard stop: call `niagara(action="list_module_inputs")` and inspect hidden/static-switch inputs before choosing values.
- Treat `session_id` and aliases as volatile editor-memory handles.
- Re-export after any structural edit: add emitter, add module, remove module, move module, or enable/disable module.
- Re-export after `niagara(action="create_local_module")` and after `niagara(action="patch_module_graph")`.
- Mutation results may include `compile_requested=true` and `compile_result_included=false`. This means the tool requested Unreal compilation asynchronously; it does not mean compile succeeded.
- Niagara mutation tools must wrap user-visible edits in `FScopedTransaction`, call `Modify()` on the system, scripts, stack graphs, modules, pins, and data interfaces before mutation, and group multi-step stack/graph edits into one editor undo step.
- For risky Niagara edits, prefer creating a new system or backup copy because stack compilation, save, or editor restart can make ordinary editor undo insufficient.
- Call `niagara(action="diagnostics", params={"asset_path":"...","force":true,"wait":true})` after structural edits, module graph patches, and input/user parameter mutations. If `summary.ok_to_save_or_claim_success` is false, fix the reported compile or stack issue before saving or claiming success.
- Use `force=true` only when you need to refresh stale compile data; otherwise prefer `wait=true` to avoid unnecessary recompiles.
- Use stack aliases from export, such as `system.system_spawn`, `system.system_update`, `e0.emitter_spawn`, `e0.emitter_update`, `e0.particle_spawn`, and `e0.particle_update`.
- Do not set module inputs by guessing. After adding a module, call `niagara(action="list_module_inputs")` and inspect `value_kind`, `enum_values`, `is_static_switch`, `default_value`, and `link_count` before setting inputs.
- For modules that can branch between mesh surface and skeleton/bone/socket sampling, explicitly call `niagara(action="set_static_switch")` on the static switch shown by `list_module_inputs`; adding the module alone is not enough.
- Do not use `set_niagara_module_input` for `source="static_switch_pin"` inputs. It writes ordinary override pins, not compile-time static switch pins.
- Use `bind_niagara_module_input_to_user_param` for scalar/vector/color inputs.
- Use `bind_niagara_module_input_to_user_param(..., binding_kind="volume_texture", default_asset_path="...")` for Volume Texture DI texture exposure.
- Use `set_niagara_module_object_input` for direct object/data-interface asset assignment.
- Save explicitly with `asset(action="save")` after successful approved mutations, and only when the user approved saving.
- If a new bridge command returns `Unknown command`, rebuild succeeded but the editor is still running an old DLL; restart Unreal Editor or reload the plugin.
- If existing modules cannot directly express the requested behavior, do not force unrelated modules together. Prefer `niagara(action="create_local_module")` plus `niagara(action="patch_module_graph")`.

## Technical Patterns

Use this section as the stable Niagara editing playbook. Prefer adding precise, reusable patterns here over relying on one-off agent memory.

### Custom HLSL Snippets

Niagara Custom HLSL nodes receive code that is inserted into Niagara-generated shader code. Treat the `hlsl` field as a snippet/body, not as a standalone C/HLSL file.

Use assignment/body snippets:

```hlsl
float3 N = normalize(In_SpriteFacing);
float3 V = normalize(In_CameraPosition - In_Position);
float Fresnel = pow(saturate(1.0 - dot(N, V)), In_Power);
Out_ColorOut = lerp(In_Pink, In_Blue, Fresnel);
Out_SizeOut = float2(5.0, 15.0);
```

Do not paste full function definitions:

```hlsl
void fresnel_calc(float3 In_Position, out float4 Out_ColorOut)
{
    Out_ColorOut = float4(1, 0, 1, 1);
}
```

Why: full function definitions can be inserted inside an already-generated function body and trigger GPU shader errors such as `function definition is not allowed here` in `/Engine/Generated/NiagaraEmitterInstance.ush`.

When patching Custom HLSL through ToolPlayMCP:

- Add dynamic input/output pins with valid HLSL identifiers only, such as `In_Position`, `In_Power`, `Out_ColorOut`, and `Out_SizeOut`.
- Do not use namespaced Niagara attributes such as `Particles.Color` on Custom HLSL pins.
- Write Niagara attributes through a downstream `parameter_map_set` node, for example `CustomHlsl.Out_ColorOut -> ParameterMapSet.Particles.Color`.
- Run `niagara(action="diagnostics", params={"force":true,"wait":true})` after GPU Custom HLSL edits and inspect `shader_compile_errors` as well as `compile_errors`.

## Local Module Guidance

Use a local/scratch module instead of ad-hoc module stacking when the requested behavior is a custom rule, for example:

- Pairwise particle-to-particle attraction or nearest-neighbor logic.
- Custom assignment such as "sample mesh location, then write a derived value to `Particles.Position` and a custom attribute".
- Per-particle rules based on ID, sorted neighbor lists, custom HLSL, or simulation-stage data interfaces.
- Effects that require branching or loops that are not exposed by existing module inputs.

Use `niagara(action="create_local_module")` first, re-export to get the module alias, then use `niagara(action="patch_module_graph")` for internal graph edits. Keep patches small and re-export between batches when pin names or node aliases are uncertain.

## Interpretation Notes

- Niagara Fluids systems often hide most behavior in ordered simulation stages. Explain those as GPU compute passes, not as ordinary particle update nodes.
- Attribute Reader systems often store neighbor IDs during spawn and read positions/attributes in simulation stages.
- Compact graphs omit reroute nodes and truncate large module internals; use module inputs and semantic catalog notes before dumping full graphs into context.
