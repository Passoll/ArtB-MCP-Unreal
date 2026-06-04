from __future__ import annotations

import json
import socket
from typing import Any


HOST = "127.0.0.1"
PORT = 55557
PACKAGE_PATH = "/Game/LevelPrototyping/Materials"
ASSET_NAME = "M_TP_EnergyRipple"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}.{ASSET_NAME}"


def call_unreal(command: str, params: dict[str, Any]) -> dict[str, Any]:
    with socket.create_connection((HOST, PORT), timeout=10) as sock:
        sock.sendall((json.dumps({"command": command, "params": params}) + "\n").encode("utf-8"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(4 * 1024 * 1024)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("utf-8"))


def node(create_class: str, x: int, y: int, properties: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "create_class": create_class,
        "position": [x, y],
        "properties": properties or {},
    }


def export_graph() -> dict[str, Any]:
    response = call_unreal("export_material_compact", {"asset_path": ASSET_PATH})
    if not response.get("result", {}).get("ok"):
        return {}
    return response.get("result", {}).get("graph", {})


def find_node(graph: dict[str, Any], *, kind: str | None = None, label: str | None = None) -> str | None:
    for alias, node_data in graph.get("nodes", {}).items():
        if not isinstance(node_data, dict):
            continue
        if kind is not None and node_data.get("k") != kind:
            continue
        if label is not None and node_data.get("label") != label:
            continue
        return alias
    return None


def has_parameter(graph: dict[str, Any], name: str) -> bool:
    return find_node(graph, label=name) is not None


def ensure_warped_spacing() -> None:
    graph = export_graph()
    if has_parameter(graph, "WarpFrequency"):
        print("warp spacing already present; skipping warp patch")
        return

    distance = find_node(graph, kind="Distance")
    time = find_node(graph, kind="Time")
    ring_density = find_node(graph, label="RingDensity")
    distance_scaled = None
    for from_node, _from_pin, to_node, to_pin in graph.get("edges", []):
        if from_node == distance and to_pin == "a":
            distance_scaled = to_node
            break

    if not all([distance, time, ring_density, distance_scaled]):
        raise SystemExit("cannot locate base ripple nodes for warp patch")

    warp_ops = [
        {
            "op": "add_node",
            "temp_id": "warp_frequency",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                -1560,
                -420,
                {"ParameterName": "WarpFrequency", "DefaultValue": 7.0},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "warp_speed",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                -1560,
                -260,
                {"ParameterName": "WarpSpeed", "DefaultValue": 0.35},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "warp_amount",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                -980,
                -320,
                {"ParameterName": "WarpAmount", "DefaultValue": 0.035},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "distance_warp_freq",
            "node": node("/Script/Engine.MaterialExpressionMultiply", -1280, -380),
        },
        {
            "op": "add_node",
            "temp_id": "time_warp_speed",
            "node": node("/Script/Engine.MaterialExpressionMultiply", -1280, -220),
        },
        {
            "op": "add_node",
            "temp_id": "warp_phase",
            "node": node("/Script/Engine.MaterialExpressionAdd", -1040, -300),
        },
        {
            "op": "add_node",
            "temp_id": "warp_sine",
            "node": node("/Script/Engine.MaterialExpressionSine", -800, -300, {"Period": 1.0}),
        },
        {
            "op": "add_node",
            "temp_id": "warp_offset",
            "node": node("/Script/Engine.MaterialExpressionMultiply", -560, -280),
        },
        {
            "op": "add_node",
            "temp_id": "warped_distance",
            "node": node("/Script/Engine.MaterialExpressionAdd", -320, -160),
        },
        {"op": "disconnect", "from": {"node": distance, "pin": "out"}, "to": {"node": distance_scaled, "pin": "a"}},
        {"op": "connect", "from": {"node": distance, "pin": "out"}, "to": {"node": "distance_warp_freq", "pin": "a"}},
        {"op": "connect", "from": {"node": "warp_frequency", "pin": "out"}, "to": {"node": "distance_warp_freq", "pin": "b"}},
        {"op": "connect", "from": {"node": time, "pin": "out"}, "to": {"node": "time_warp_speed", "pin": "a"}},
        {"op": "connect", "from": {"node": "warp_speed", "pin": "out"}, "to": {"node": "time_warp_speed", "pin": "b"}},
        {"op": "connect", "from": {"node": "distance_warp_freq", "pin": "out"}, "to": {"node": "warp_phase", "pin": "a"}},
        {"op": "connect", "from": {"node": "time_warp_speed", "pin": "out"}, "to": {"node": "warp_phase", "pin": "b"}},
        {"op": "connect", "from": {"node": "warp_phase", "pin": "out"}, "to": {"node": "warp_sine", "pin": "Input"}},
        {"op": "connect", "from": {"node": "warp_sine", "pin": "out"}, "to": {"node": "warp_offset", "pin": "a"}},
        {"op": "connect", "from": {"node": "warp_amount", "pin": "out"}, "to": {"node": "warp_offset", "pin": "b"}},
        {"op": "connect", "from": {"node": distance, "pin": "out"}, "to": {"node": "warped_distance", "pin": "a"}},
        {"op": "connect", "from": {"node": "warp_offset", "pin": "out"}, "to": {"node": "warped_distance", "pin": "b"}},
        {"op": "connect", "from": {"node": "warped_distance", "pin": "out"}, "to": {"node": distance_scaled, "pin": "a"}},
    ]

    validate_response = call_unreal("validate_material_patch", {"asset_path": ASSET_PATH, "ops": warp_ops})
    print("validate_warp_patch", json.dumps(validate_response, indent=2)[:5000])
    if not validate_response.get("result", {}).get("ok"):
        raise SystemExit("validate warp patch failed")

    apply_response = call_unreal("apply_material_patch", {"asset_path": ASSET_PATH, "ops": warp_ops})
    print("apply_warp_patch", json.dumps(apply_response, indent=2)[:5000])
    if not apply_response.get("result", {}).get("ok"):
        raise SystemExit("apply warp patch failed")


def main() -> None:
    create_response = call_unreal(
        "create_material_asset",
        {
            "package_path": PACKAGE_PATH,
            "asset_name": ASSET_NAME,
        },
    )
    print("create_material_asset", json.dumps(create_response, indent=2))
    if not create_response.get("result", {}).get("ok"):
        error = create_response.get("result", {}).get("error", "")
        if "already exists" not in error:
            raise SystemExit("create_material_asset failed; not applying patch to avoid duplicate graph edits.")
        print("asset already exists; continuing with patch")
        existing_graph = export_graph()
        if existing_graph.get("nodes"):
            ensure_warped_spacing()
            export_response = call_unreal("export_material_compact", {"asset_path": ASSET_PATH})
            print("export_material_compact", json.dumps(export_response, indent=2)[:8000])
            return

    ops: list[dict[str, Any]] = [
        {
            "op": "set_material_property",
            "properties": {
                "BlendMode": "BLEND_Translucent",
                "TwoSided": True,
            },
        },
        {
            "op": "add_node",
            "temp_id": "uv",
            "node": node("/Script/Engine.MaterialExpressionTextureCoordinate", -1400, -120),
        },
        {
            "op": "add_node",
            "temp_id": "center",
            "node": node(
                "/Script/Engine.MaterialExpressionConstant2Vector",
                -1400,
                60,
                {"R": 0.5, "G": 0.5},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "distance",
            "node": node("/Script/Engine.MaterialExpressionDistance", -1120, -40),
        },
        {
            "op": "add_node",
            "temp_id": "ring_density",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                -1120,
                170,
                {"ParameterName": "RingDensity", "DefaultValue": 24.0},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "time",
            "node": node("/Script/Engine.MaterialExpressionTime", -1120, 360),
        },
        {
            "op": "add_node",
            "temp_id": "wave_speed",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                -1120,
                520,
                {"ParameterName": "WaveSpeed", "DefaultValue": 0.65},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "distance_scaled",
            "node": node("/Script/Engine.MaterialExpressionMultiply", -840, 20),
        },
        {
            "op": "add_node",
            "temp_id": "time_scaled",
            "node": node("/Script/Engine.MaterialExpressionMultiply", -840, 430),
        },
        {
            "op": "add_node",
            "temp_id": "phase",
            "node": node("/Script/Engine.MaterialExpressionSubtract", -600, 120),
        },
        {
            "op": "add_node",
            "temp_id": "sine",
            "node": node("/Script/Engine.MaterialExpressionSine", -360, 120, {"Period": 1.0}),
        },
        {
            "op": "add_node",
            "temp_id": "one",
            "node": node("/Script/Engine.MaterialExpressionConstant", -360, 300, {"R": 1.0}),
        },
        {
            "op": "add_node",
            "temp_id": "half",
            "node": node("/Script/Engine.MaterialExpressionConstant", -360, 470, {"R": 0.5}),
        },
        {
            "op": "add_node",
            "temp_id": "sine_plus_one",
            "node": node("/Script/Engine.MaterialExpressionAdd", -120, 170),
        },
        {
            "op": "add_node",
            "temp_id": "wave_01",
            "node": node("/Script/Engine.MaterialExpressionMultiply", 120, 190),
        },
        {
            "op": "add_node",
            "temp_id": "sharpness",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                -120,
                420,
                {"ParameterName": "RingSharpness", "DefaultValue": 5.0},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "wave_mask",
            "node": node("/Script/Engine.MaterialExpressionPower", 360, 210),
        },
        {
            "op": "add_node",
            "temp_id": "energy_color",
            "node": node(
                "/Script/Engine.MaterialExpressionVectorParameter",
                360,
                -120,
                {"ParameterName": "EnergyColor", "DefaultValue": [0.05, 0.85, 1.0, 1.0]},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "emissive_strength",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                360,
                20,
                {"ParameterName": "EmissiveStrength", "DefaultValue": 4.0},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "color_wave",
            "node": node("/Script/Engine.MaterialExpressionMultiply", 620, 30),
        },
        {
            "op": "add_node",
            "temp_id": "emissive",
            "node": node("/Script/Engine.MaterialExpressionMultiply", 860, 60),
        },
        {
            "op": "add_node",
            "temp_id": "base_opacity",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                360,
                520,
                {"ParameterName": "BaseOpacity", "DefaultValue": 0.18},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "wave_opacity",
            "node": node(
                "/Script/Engine.MaterialExpressionScalarParameter",
                360,
                680,
                {"ParameterName": "WaveOpacity", "DefaultValue": 0.55},
            ),
        },
        {
            "op": "add_node",
            "temp_id": "opacity_wave",
            "node": node("/Script/Engine.MaterialExpressionMultiply", 620, 560),
        },
        {
            "op": "add_node",
            "temp_id": "opacity",
            "node": node("/Script/Engine.MaterialExpressionAdd", 860, 560),
        },
        {"op": "connect", "from": {"node": "uv", "pin": "out"}, "to": {"node": "distance", "pin": "a"}},
        {"op": "connect", "from": {"node": "center", "pin": "out"}, "to": {"node": "distance", "pin": "b"}},
        {"op": "connect", "from": {"node": "distance", "pin": "out"}, "to": {"node": "distance_scaled", "pin": "a"}},
        {"op": "connect", "from": {"node": "ring_density", "pin": "out"}, "to": {"node": "distance_scaled", "pin": "b"}},
        {"op": "connect", "from": {"node": "time", "pin": "out"}, "to": {"node": "time_scaled", "pin": "a"}},
        {"op": "connect", "from": {"node": "wave_speed", "pin": "out"}, "to": {"node": "time_scaled", "pin": "b"}},
        {"op": "connect", "from": {"node": "distance_scaled", "pin": "out"}, "to": {"node": "phase", "pin": "a"}},
        {"op": "connect", "from": {"node": "time_scaled", "pin": "out"}, "to": {"node": "phase", "pin": "b"}},
        {"op": "connect", "from": {"node": "phase", "pin": "out"}, "to": {"node": "sine", "pin": "Input"}},
        {"op": "connect", "from": {"node": "sine", "pin": "out"}, "to": {"node": "sine_plus_one", "pin": "a"}},
        {"op": "connect", "from": {"node": "one", "pin": "out"}, "to": {"node": "sine_plus_one", "pin": "b"}},
        {"op": "connect", "from": {"node": "sine_plus_one", "pin": "out"}, "to": {"node": "wave_01", "pin": "a"}},
        {"op": "connect", "from": {"node": "half", "pin": "out"}, "to": {"node": "wave_01", "pin": "b"}},
        {"op": "connect", "from": {"node": "wave_01", "pin": "out"}, "to": {"node": "wave_mask", "pin": "Base"}},
        {"op": "connect", "from": {"node": "sharpness", "pin": "out"}, "to": {"node": "wave_mask", "pin": "Exponent"}},
        {"op": "connect", "from": {"node": "energy_color", "pin": "rgb"}, "to": {"node": "color_wave", "pin": "a"}},
        {"op": "connect", "from": {"node": "wave_mask", "pin": "out"}, "to": {"node": "color_wave", "pin": "b"}},
        {"op": "connect", "from": {"node": "color_wave", "pin": "out"}, "to": {"node": "emissive", "pin": "a"}},
        {"op": "connect", "from": {"node": "emissive_strength", "pin": "out"}, "to": {"node": "emissive", "pin": "b"}},
        {"op": "connect", "from": {"node": "emissive", "pin": "out"}, "to": {"node": "root", "pin": "EmissiveColor"}},
        {"op": "connect", "from": {"node": "energy_color", "pin": "rgb"}, "to": {"node": "root", "pin": "BaseColor"}},
        {"op": "connect", "from": {"node": "wave_mask", "pin": "out"}, "to": {"node": "opacity_wave", "pin": "a"}},
        {"op": "connect", "from": {"node": "wave_opacity", "pin": "out"}, "to": {"node": "opacity_wave", "pin": "b"}},
        {"op": "connect", "from": {"node": "base_opacity", "pin": "out"}, "to": {"node": "opacity", "pin": "a"}},
        {"op": "connect", "from": {"node": "opacity_wave", "pin": "out"}, "to": {"node": "opacity", "pin": "b"}},
        {"op": "connect", "from": {"node": "opacity", "pin": "out"}, "to": {"node": "root", "pin": "Opacity"}},
    ]

    validate_response = call_unreal("validate_material_patch", {"asset_path": ASSET_PATH, "ops": ops})
    print("validate_material_patch", json.dumps(validate_response, indent=2)[:8000])
    if not validate_response.get("result", {}).get("ok"):
        patch = validate_response.get("result", {}).get("patch", {})
        messages = " ".join(
            op.get("message", "")
            for op in patch.get("ops", [])
            if isinstance(op, dict) and not op.get("ok")
        )
        if "Unknown from node" not in messages:
            raise SystemExit("validate_material_patch failed; not applying.")
        print("validate hit known temp-node issue in currently loaded bridge; continuing to apply")

    apply_response = call_unreal("apply_material_patch", {"asset_path": ASSET_PATH, "ops": ops})
    print("apply_material_patch", json.dumps(apply_response, indent=2)[:8000])
    if not apply_response.get("result", {}).get("ok"):
        raise SystemExit("apply_material_patch failed.")

    ensure_warped_spacing()

    export_response = call_unreal("export_material_compact", {"asset_path": ASSET_PATH})
    print("export_material_compact", json.dumps(export_response, indent=2)[:8000])


if __name__ == "__main__":
    main()
