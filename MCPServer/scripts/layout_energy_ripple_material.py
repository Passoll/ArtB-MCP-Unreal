from __future__ import annotations

import json
import socket
from typing import Any


HOST = "127.0.0.1"
PORT = 55557
ASSET_PATH = "/Game/LevelPrototyping/Materials/M_TP_EnergyRipple.M_TP_EnergyRipple"


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


def main() -> None:
    # Current generated graph aliases. Re-export before use if the material is regenerated.
    layout = {
        "n0": [-1800, -160],  # UV
        "n1": [-1800, 40],    # center
        "n2": [-1560, -60],   # distance
        "n24": [-1560, -420], # warp frequency
        "n25": [-1560, -260], # warp speed
        "n26": [-980, -320],  # warp amount
        "n27": [-1280, -380], # distance * warp frequency
        "n28": [-1280, -220], # time * warp speed
        "n29": [-1040, -300], # warp phase
        "n30": [-800, -300],  # warp sine
        "n31": [-560, -280],  # warp offset
        "n32": [-320, -160],  # warped distance
        "n3": [-320, 20],     # ring density
        "n6": [-80, -80],     # warped distance * density
        "n4": [-820, 260],    # time
        "n5": [-820, 420],    # wave speed
        "n7": [-560, 340],    # time * speed
        "n8": [160, 80],      # phase subtract
        "n9": [400, 80],      # sine
        "n10": [400, 240],    # one
        "n11": [640, 380],    # half
        "n12": [640, 160],    # sine + one
        "n13": [880, 220],    # * 0.5
        "n14": [880, 420],    # sharpness
        "n15": [1120, 260],   # wave mask
        "n16": [1120, -140],  # energy color
        "n17": [1120, 20],    # emissive strength
        "n18": [1360, -60],   # color * mask
        "n19": [1600, -40],   # emissive
        "n20": [1120, 620],   # base opacity
        "n21": [1120, 780],   # wave opacity
        "n22": [1360, 700],   # mask * opacity
        "n23": [1600, 660],   # opacity add
        "root": [1900, 260],   # material output
    }

    ops = [
        {"op": "move_node", "node": alias, "position": position}
        for alias, position in layout.items()
    ]

    validate_response = call_unreal("validate_material_patch", {"asset_path": ASSET_PATH, "ops": ops})
    print("validate_material_patch", json.dumps(validate_response, indent=2)[:4000])
    if not validate_response.get("result", {}).get("ok"):
        raise SystemExit("validate_material_patch failed")

    apply_response = call_unreal("apply_material_patch", {"asset_path": ASSET_PATH, "ops": ops})
    print("apply_material_patch", json.dumps(apply_response, indent=2)[:4000])
    if not apply_response.get("result", {}).get("ok"):
        raise SystemExit("apply_material_patch failed")


if __name__ == "__main__":
    main()
