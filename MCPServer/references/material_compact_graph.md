# Material Compact Graph Usage

Use this reference when interpreting `material(action="read")` output or adding tools that consume compact material graphs.

## Shape

Compact material graphs are neutral graph data for AI context, not a semantic summary.

```json
{
  "asset": "TestMat",
  "scope": "asset",
  "nodes": {
    "n0": {"k": "const", "v": "[1,0.498069,0.527211]"},
    "n1": {"k": "1-x"},
    "root": {"k": "root"}
  },
  "edges": [
    ["n0", "out", "n1", "x"],
    ["n1", "out", "root", "EmissiveColor"]
  ]
}
```

## Aliases

- `n0`, `n1`, and similar aliases are temporary AI-facing node IDs.
- `root` is the material output node.
- Unreal GUIDs and UObject paths must stay inside plugin session state.
- Do not assume aliases are stable across separate export sessions.

## Node Kinds

Common normalized `k` values:

- `tex`: texture sample
- `param`: scalar parameter
- `vparam`: vector parameter
- `const`: constant value
- `*`: multiply
- `+`: add
- `-`: subtract
- `/`: divide
- `lerp`: linear interpolate
- `1-x`: one minus
- `clamp`: clamp
- `root`: material output root

Unknown expressions may use a shortened Unreal class name.

## Pin Normalization

Normalize pins to reduce token cost while preserving graph meaning.

General output rules:

- empty output name -> `out`
- `Output0` -> `out`
- `OutputN` -> `outN`
- `RGB` -> `rgb`
- `RGBA` -> `rgba`
- `R`, `G`, `B`, `A` -> `r`, `g`, `b`, `a`
- `Alpha` -> `alpha`

General input rules:

- `Input` -> `in`
- `A` -> `a`
- `B` -> `b`
- `Alpha` -> `alpha`

Node-specific input rules:

- unary nodes such as `1-x`: `Input` -> `x`
- binary math nodes such as `*`, `+`, `-`, `/`: `A` -> `a`, `B` -> `b`
- `lerp`: `A` -> `a`, `B` -> `b`, `Alpha` -> `alpha`

Material output property names such as `BaseColor`, `EmissiveColor`, and `Roughness` should remain readable.

## Orphan Nodes

Do not prune orphan nodes in the compact exporter by default. A graph-understanding tool or prompt may explicitly mention isolated nodes as likely unused or disconnected.
