# MRFPS art assets

`Art/create_assets.py` builds the six MRFPS static meshes with bpy/bmesh. It uses no external assets, textures or downloads. It also saves an editable `.blend`, exports one FBX (plus a GLB) per mesh, writes `manifest.json` and renders a 1280x720 asset board.

The generator was authored through Claude Code with Opus 5.5 / xhigh. Root executed and verified the Blender runs below; this documentation update does not change the generator.

## Execution status

### Verified: root run of generator v1 (Blender 5.2.0 LTS)

The run exited with code 0 and produced `MRFPS_Assets.blend`, all six FBX/GLB pairs, `manifest.json` and `AssetBoard.png`. Its manifest was recovered from the original read-tool result into `Saved/Validation/art-v1-manifest.json`, and was compared with the actual v2 manifest below.

| Asset | Tris | Measured min → max (m) | Hit-volume fit |
|---|---|---|---|
| `SM_MRPistol` | 544 | (-0.1, -0.0325, -0.1245) → (0.22, 0.0325, 0.0355); size 0.32 x 0.065 x 0.16 | — (muzzle face at X = 0.22) |
| `SM_MRWall` | 652 | (-0.08, -0.8, -1.0) → (0.08, 0.8, 1.0) | inside its box (excess 0.0) |
| `SM_MRMiniShield` | 652 | (-0.02, -0.22, -0.22) → (0.02, 0.22, 0.22) | inside its box (excess 0.0) |
| `SM_MRHead` | 1212 | (-0.1375, -0.128, -0.122) → (0.12392, 0.128, 0.132) | **19.3 mm outside** the r = 13 cm sphere (the only warning) |
| `SM_MRBody` | 1268 | (-0.2, -0.19682, -0.4) → (0.194, 0.19682, 0.4) | inside the capsule with 13.8 mm margin |
| `SM_MRHand` | 880 | (-0.012, -0.05835, -0.0275) → (0.16696, 0.04231, 0.0275) | — |

Other v1 results:
- **Totals:** 5,208 triangles; `errors` was empty.
- **Mesh validation:** all six meshes had 0 loose verts/edges, 0 non-manifold edges, 0 zero-area triangles and 0 inverted parts.
- **FBX round-trip:** passed for all six. Slot order was preserved, the bounds error was ≤ 1e-7 m, and `SOCKET_Muzzle` came back at (0.22, 0, 0).
- **Export options:** Blender 5.2 accepted every FBX/GLB option.
- **Board:** rendered with EEVEE, 64 samples, 2 threads.
- **QA defect:** the source meshes also rendered as a small stack in the gutter around x 640, y 200–355. They were left render-visible at their `.blend` layout positions near the world origin.

The earlier UE integration record for the v1 outputs reported:
- the meshes are integrated at scale 1;
- the gun offset is 0;
- the left hand has a rotation;
- all nine UE palette materials exist.

### Verified: root run of generator v2 (2026-10-28 01:36:49 JST)

The real Blender 5.2.0 LTS run completed successfully. `Saved/Validation/blender-assets-v2.log` reports **5,256 triangles, 0 errors and 0 warnings**. The current `Art/Generated/manifest.json` has `generator_version: 2`, six FBX/GLB pairs, a saved editable `.blend`, and a rendered board.

| Asset | v1 → v2 tris | Measured v2 size (m) | Measured containment |
|---|---|---|---|
| `SM_MRPistol` | 544 → 544 | 0.32 × 0.065 × 0.16 | muzzle socket at X = 0.22 m |
| `SM_MRWall` | 652 → 652 | 0.16 × 1.6 × 2.0 | inside box; excess 0.0 |
| `SM_MRMiniShield` | 652 → 652 | 0.04 × 0.44 × 0.44 | inside box; excess 0.0 |
| `SM_MRHead` | 1212 → 1260 | **0.25192 × 0.251 × 0.249** | inside r = 0.13 m sphere, **1.67 mm margin** |
| `SM_MRBody` | 1268 → 1268 | 0.394 × 0.39364 × 0.8 | inside capsule, 13.84 mm margin |
| `SM_MRHand` | 880 → 880 | 0.17896 × 0.10066 × 0.055 | wrist origin retained |

The head bounds are (-0.128, -0.1255, -0.122) → (0.12392, 0.1255, 0.127) m. Its previous 19.31 mm sphere excess is gone. The rear plate, crest, brow and side pods were revised to fit the hit sphere. Sphere, capsule and box containment failures are now hard errors.

All six meshes have zero loose vertices/edges, non-manifold edges, zero-area triangles and inverted parts. All six FBX round-trips succeeded with the same triangle counts and material-slot order; the maximum bounds error is 1e-7 m, and the pistol socket returns at (0.22, 0, 0).

The 1280×720 EEVEE board was rendered with 64 samples and two threads. `preview.render_leaks` is empty, and visual inspection confirms the old source-mesh stack in the gutter has been removed. Only the preview collection renders; the editable source and hitbox collections stay in the `.blend` with `hide_render` enabled. This board is an exported-asset preview, not a UE gameplay screenshot.

#### Actual v1/v2 comparison

The restored v1 and actual v2 manifests were compared directly. For the five assets other than the intentionally revised head, triangle counts, vertex counts, min/max bounds, dimensions, per-slot triangle counts and material-slot order all match. Wall/shield/body containment values also match.

Their ordered `geometry_sha256` values **do not match**:

| Asset | v1 hash prefix | v2 hash prefix |
|---|---|---|
| Pistol | `c845623f23d6` | `689e4508e07c` |
| Wall | `4fdc94a2ddb9` | `8221a418800f` |
| Shield | `241381f75e63` | `9e9ee58de883` |
| Body | `b7b57916fe54` | `a0429791bf4a` |
| Hand | `c72664563175` | `81a024297ce4` |

The digest hashes coordinates in vertex order, followed by indexed triangle/material order. It is not independent of Blender mesh ordering. An ordering change could alter it without changing shape, but that cause has not been proven for these runs. The matching measurements establish those specific contracts; complete geometry identity remains unproven. A hash difference alone is not evidence of a visible geometry regression.

**Verified UE integration:** root re-imported all six v2 FBXs at 2026-10-27 16:40:20 UTC. `Saved/Validation/art-v2-setup.log` records six `MRFPS_ART_IMPORTED` entries, `MRFPS_SETUP_PASS`, and commandlet exit 0. Imported sizes in centimetres were pistol 32.000 × 6.500 × 16.000, wall 16.000 × 160.000 × 200.000, shield 4.000 × 44.000 × 44.000, head **25.192 × 25.100 × 24.900**, body 39.400 × 39.364 × 80.000, and hand 17.896 × 10.066 × 5.500. These agree with the v2 manifest. Gameplay appearance and Quest rendering remain separate checks.

## Reproduce

Run from the project root in PowerShell:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' --background --factory-startup --threads 2 --python-exit-code 1 --python Art/create_assets.py
```

Optional arguments go after `--`, for example `... --python Art/create_assets.py -- --no-render`:

| Option | Effect |
|---|---|
| `--no-render` | Skip `AssetBoard.png` (everything else is still produced) |
| `--engine cycles` | Render the board with CPU Cycles (48 samples, denoised) instead of EEVEE |
| `--samples N` | Override the render samples (EEVEE default 64) |

Rendering is limited to 2 threads at 1280x720. Exit code 0 means every check passed. Exit code 1 means a check failed; look for `[MRFPS-ART] ERROR` lines in the log and the `errors` list in the manifest.

## Outputs (all overwritten on each run)

| Path | Contents |
|---|---|
| `Art/Generated/MRFPS_Assets.blend` | Three collection groups (see below) |
| `Art/Generated/Exports/SM_*.fbx` | One static mesh per file, written at the origin. The only extra node is `SOCKET_Muzzle` in the pistol FBX. No cameras or lights |
| `Art/Generated/Exports/SM_*.glb` | The same mesh, for quick inspection only |
| `Art/Generated/manifest.json` | Files, triangles (total and per slot), slots, bounds, origin/forward contract, sockets, hit-volume fit, validation, `geometry_sha256`, FBX round-trip, preview status and render leaks |
| `Art/Preview/AssetBoard.png` | 1280x720 board of linked-duplicate display copies of the six real meshes |

The `.blend` collections:
- **`MRFPS_Assets/{Weapon,Abilities,Avatar}`:** the editable source meshes at layout positions. From v2 these are hidden in renders.
- **`MRFPS_HitboxReference`:** wireframe gameplay volumes parented to their asset. Never rendered or exported.
- **`MRFPS_PreviewBoard`:** camera, lights, cards, captions and display copies. This is the only collection that renders.

Safety: paths are resolved from the script's own location. The script creates `Generated`, `Generated/Exports` and `Preview` if they are missing. It deletes only its own exact output files before rewriting them (`SM_*.fbx/.glb`, `AssetBoard.png`, `MRFPS_Assets.blend1`) and never deletes a directory. FBX headers contain a timestamp, so file bytes change between runs. The manifest digest also depends on mesh ordering; compare it together with dimensions, topology counts, slot counts and containment rather than treating it as an ordering-independent shape fingerprint.

The `.blend` is a build product. Make lasting edits in `create_assets.py`, because the next run overwrites the `.blend`.

## Conventions

- Units: metres in Blender and in the manifest. UE uses centimetres, so multiply by 100.
- Blender axes: +X forward, +Y left, +Z up (right-handed).
- UE axes: +X forward, +Y right, +Z up (left-handed).
- The importer maps Blender `(x, y, z)` m to UE `(100x, -100y, 100z)` cm. Forward and up are kept, and the Y flip keeps shapes from being mirrored, so the left hand stays a left hand.
- Every mesh has object scale 1 and is built at its contract origin. Each one is a set of closed, outward-facing solids (layered parts may intersect). The script deletes loose verts and edges and checks for inverted parts.
- Each mesh has one UV map (box projection), which unlit materials don't need. Edges are marked sharp by angle (>33°) or where the material changes.
- A Triangulate modifier (Beauty) is applied on export, so triangle counts match what UE receives. No Nanite dependency.

## Asset contract

Slot order matters. `MRFPS::Material(Component, Name)` in `MRFPSGame.cpp` replaces **material slot 0 only**:
- Head, body, hand and pistol use slot 0 for team accents. Swapping `M_Blue` to `M_Orange` at runtime recolours only the accents.
- The wall and shield put `M_Wall` and `M_Shield` in slot 0, matching the existing calls.

| Asset | Slots (0 first) | Origin / axes / hit volume |
|---|---|---|
| `SM_MRPistol` | Blue, Graphite, Trim, White, Tracer | Grip/hand attach point on the bore axis. +X = fire. Muzzle face at (0.22, 0, 0), matching `Muzzle()` = Right + forward·22 cm. The FBX carries `SOCKET_Muzzle` there |
| `SM_MRWall` | Wall, Graphite, Trim, White | Box centre = `AMRFPSBarrier` extent (8, 80, 100) cm. +X = front/normal. Light bars and the base strip are on the front only. Must stay inside its box (error otherwise) |
| `SM_MRMiniShield` | Shield, Graphite, Trim, White | Box centre = MiniShield extent (2, 22, 22) cm. +X = normal. Must stay inside its box (error otherwise) |
| `SM_MRHead` | Blue, Graphite, Trim, White | Head centre (HMD pose). +X = visor. From v2, every vertex must lie inside the r = 13 cm HeadHitbox sphere (error otherwise) |
| `SM_MRBody` | Blue, Graphite, Trim, White | Torso centre (head − 58 cm). +X = chest. Must stay inside the body capsule (r 22, half-height 50 cm) (error otherwise) |
| `SM_MRHand` | Blue, Graphite, Trim, White | Left hand, wrist centre. +X = fingers, +Z = dorsal, palm faces −Z. Thumb is on Blender −Y (UE +Y). Palm emitter centre is about (0.054, 0.002, −0.0195) |

Budget: 6,000 triangles per asset and 25,000 in total. The script fails the run if either is exceeded. v2 measured **5,256** in total, with 1,268 the largest single asset.

## Visual language

- Matte graphite shells with pale-metal (`M_Trim`) chamfers on the feature edges. The chamfer faces carry their own slot, so outlines still read with flat unlit colour.
- White armour plates, cyan/blue team energy (slot 0) and orange for the opponent.
- Wall and shield share a design: frame, energy field, struts and a hub.
- The pistol's tracer-yellow lens marks where shots originate.

The board shows the pistol and hand in blue and the head and body with the orange slot-0 override (an object-level override on the display copies only). The mesh data itself always holds `M_Blue`.

## Unreal Engine 5.8 import

Import each `Exports/SM_*.fbx` (legacy FBX or Interchange) to a folder such as `/Game/Art/Meshes`:

| Setting | Value |
|---|---|
| Import As | Static Mesh (no skeleton or animation) |
| Import Uniform Scale | 1.0 |
| Convert Scene | on |
| Force Front X Axis | **off** |
| Convert Scene Unit | on |
| Import translation/rotation offsets | zero |
| Normal Import Method | Import Normals |
| Generate Lightmap UVs | off (unlit) |
| Auto Generate Collision | off (gameplay uses its own box/sphere/capsule components) |
| Nanite | off |
| Sockets | on (`SOCKET_Muzzle` on the pistol at (22, 0, 0) cm) |
| Materials | Don't create new ones. Bind slot names to `/Game/Materials/M_<Name>` |

After import, check that the Static Mesh Editor bounds equal manifest `bounds_m` x 100 (for example, the wall should be 16 x 160 x 200 cm) and that the pistol muzzle sits at +X.

Root reports that all nine unlit materials exist in `/Game/Materials`:
- the five from `MRFPSSetupCommandlet` used by these meshes: `M_Blue`, `M_Orange`, `M_Wall`, `M_Shield`, `M_Tracer`;
- the commandlet's `M_Floor`;
- `M_Graphite`, `M_Trim` and `M_White`.

The suggested colours for all eight mesh slots are in `manifest.json` under `materials`. For example, `M_Graphite` (0.035, 0.040, 0.050), `M_Trim` (0.55, 0.60, 0.66), `M_White` (0.85, 0.88, 0.92).

## Integration (outside `Art/`)

The prior integration record reports:
- meshes at scale (1, 1, 1);
- the pistol's +8 cm forward offset removed, so the muzzle meets `Muzzle()`;
- a rotation on the left-hand visual.

The subsequent v2 re-import and imported measurements are verified above. A successful import does not establish gameplay appearance or Quest rendering.

Still open as a suggestion: `BodyVisual` gets only a location. Applying the head yaw would keep its +X following the player.

Keep calling `MRFPS::Material(...)` for team colour. Slot 0 is laid out for exactly that.
