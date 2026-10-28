"""Fit a floor-preserving SteamVR->Quest alignment from corresponding measured points.

Input JSON contains 'source_cm' and 'quest_cm': two or more matching [x,y,z]
points. source_cm is OpenVR converted to UE cm (-z, x, y), not raw OpenVR.
It also contains left_serial/right_serial. Output can be given to the bridge.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


def fit_alignment(source: list[list[float]], quest: list[list[float]]) -> dict:
    if len(source) != len(quest) or len(source) < 2:
        raise ValueError("at least two corresponding points required")
    if any(len(p) != 3 or any(not math.isfinite(x) for x in p) for p in source + quest):
        raise ValueError("points must contain three finite coordinates")
    count = len(source)
    center_source = [sum(p[i] for p in source) / count for i in range(3)]
    center_quest = [sum(p[i] for p in quest) / count for i in range(3)]
    cosine = sine = variance = 0.0
    for a, b in zip(source, quest):
        sx, sy = a[0] - center_source[0], a[1] - center_source[1]
        qx, qy = b[0] - center_quest[0], b[1] - center_quest[1]
        cosine += sx * qx + sy * qy
        sine += sx * qy - sy * qx
        variance += sx * sx + sy * sy
    if variance < 1250 or math.hypot(cosine, sine) < 1e-8:
        raise ValueError("use points separated by at least 50 cm horizontally")
    yaw = math.atan2(sine, cosine)
    c, s = math.cos(yaw), math.sin(yaw)
    translation = [center_quest[0] - (c * center_source[0] - s * center_source[1]),
                   center_quest[1] - (s * center_source[0] + c * center_source[1]),
                   center_quest[2] - center_source[2]]
    error_squared = 0.0
    for a, b in zip(source, quest):
        projected = [c * a[0] - s * a[1] + translation[0], s * a[0] + c * a[1] + translation[1], a[2] + translation[2]]
        error_squared += sum((projected[i] - b[i]) ** 2 for i in range(3))
    rms = math.sqrt(error_squared / count)
    if rms > 3:
        raise ValueError(f"alignment RMS {rms:.2f} cm exceeds 3 cm; repeat measurements")
    return {"yaw_degrees": math.degrees(yaw), "translation_cm": translation, "rms_error_cm": rms}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("measurements")
    parser.add_argument("output")
    args = parser.parse_args()
    data = json.loads(Path(args.measurements).read_text(encoding="utf-8"))
    result = fit_alignment(data["source_cm"], data["quest_cm"])
    result.update({key: data[key] for key in ("left_serial", "right_serial")})
    Path(args.output).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Alignment saved: RMS {result['rms_error_cm']:.2f} cm")


if __name__ == "__main__":
    main()
