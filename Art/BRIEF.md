# MRFPS Blender asset task

The user explicitly requests Claude Code with **Claude Opus 5.5**, effort **xhigh**, to handle Blender work for this project. Keep that model and effort; do not silently switch models.

Build a cohesive original 3D asset set for a UE5.8 online mixed-reality 1v1 shooter on Meta Quest 3. The authoritative gameplay already exists. It has a 10m x 10m floor-aligned arena, a compact right-hand gun, a left-hand rising wall, open-left-palm mini shield, and virtual player representations for independent inspectors.

Own **Art/** only. Other agents are building the project and hardware integrations. Do not edit Source, Config, Plugins, Scripts, .git or other directories. Do not stop any process or launch Unreal builds. Read README and Docs/Architecture.md if needed. No external assets, copyrighted game likenesses, downloads or new software installation.

Deliver `Art/create_assets.py`, a deterministic Blender 5.2 background script using bpy that actually builds, saves and exports this asset set. Blender exists at `C:/Program Files/Blender Foundation/Blender 5.2/blender.exe`. Root will run and inspect the script, render, and import results. You need not run shell commands in this first pass; the available tools allow reading/writing project files. Write source, not a narrative plan.

Visual direction: strong industrial design for lightweight MR training gear, matte graphite with pale metal edge accents and crisp cyan/blue energy details; orange opponent team. Distinctive silhouettes, intentional bevels, layered construction, readable parts at headset scale. Give it the polish of a coherent small game, while keeping standalone headset rendering practical. Avoid primitive placeholders, tiny unreadable surface details, external textures and dense topology.

Assets (origin, forward and dimensions are important):

| Name | Intended dimensions in metres | Origin/use |
|---|---|---|
| SM_MRPistol | length about .32 on +X, width .065, height .16 | Grip/hand attach origin (0,0,0); muzzle at about +X .22; +X is fire direction |
| SM_MRWall | .16 depth on X, 1.6 width on Y, 2.0 height on Z | Centre origin; forward/normal +X; virtual cover panel with purposeful frame and energy interior |
| SM_MRMiniShield | .04 on X, .44 on Y/Z | Centre origin; normal +X; clear compact silhouette and rim |
| SM_MRHead | roughly .26 on X/Y/Z | Head centre; +X facing, readable orientation/visor for inspector |
| SM_MRBody | .40 on X/Y, .80 on Z | Torso centre, +X forward; virtual torso that fits existing body hitbox |
| SM_MRHand | roughly .18 on X, .10 Y, .055 Z | Wrist origin; +X fingers, +Z dorsal, for tracked left hand |

At most about 6k triangles per asset, preferably much less, and under 25k total. No Nanite dependency. All meshes must have applied scale, sensible normals, no loose accidental geometry, and documented bounds. Keep any decorative wall holes/geometry inside the dimensions because gameplay uses a full box hit volume. The mini shield similarly blocks its full bounding box.

Use named material slots from this palette so UE can assign simple unlit materials: `M_Graphite`, `M_Trim`, `M_White`, `M_Blue`, `M_Orange`, `M_Wall`, `M_Shield`, `M_Tracer`. Use standard Blender node materials with reasonable preview values; export geometry with these slot names. Geometry readability should also survive unlit rendering. Do not rely on material transparency for most of the silhouette.

The generator must create:

- `Art/Generated/MRFPS_Assets.blend`, editable and organized with named collections and objects.
- Individual FBX files in `Art/Generated/Exports/` with each named static mesh, plus GLB if useful. Export only the relevant asset each time, with no cameras/lights, and document correct UE axis/unit import settings.
- `Art/Generated/manifest.json`: each asset file, triangle count, material slots, local bounds in metres, attach origin and forward convention.
- `Art/Preview/AssetBoard.png`: an attractive 1280x720 board showing all six actual meshes with good composition, soft lighting and readable captions. Use CPU/Eevee and modest resources because a UE compile is running; render threads <=2. Do not use a huge Cycles render.
- `Art/README.md`: exact reproduction command, bounds/axes/import instructions and what was actually executed versus still awaiting execution.

Use paths based on the script file, make generation idempotent within Art/Generated and Art/Preview only, and never recursively delete project or user directories. Do not fabricate proof of execution. Finish the generator and documentation so root can execute Blender, inspect the artifacts, and import them into the game.
