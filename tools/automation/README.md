# UE local control

`control.py` submits a JSON operation to a named, running protocol-13 UE client. Windows and Mac use a user-restricted local directory. A debuggable Quest build accepts the same requests in its app-private directory via `adb shell -T run-as`, with the request sent over stdin. The bridge runs on the normal game tick and does not expose a new network port.

Each request requires an immutable UUIDv4. Repeating the same UUID and payload returns or waits for the original result, and the same UUID with different parameters is rejected. If a client restarts after dispatch but before its reply is recorded, the result is `unknown`: it does not silently run the operation again. A server keeps up to 8192 operation receipts for its lifetime; this is not a durable transaction across a server restart or a machine power failure. A repeated UUID returns the original receipt; use a new UUID to apply the desired state again after other changes.

Admin mutations are sent only by the authenticated admin controller to the authoritative server. They check the supplied layout and control revisions, and wall edits additionally compare the target actor's previous full shape. Target IDs in `objects.list` refer to client-side Actor lifetimes and expire when the client reconnects or the Actor disappears. List the objects again after reconnect. No actor ID grants additional server permission.

## Commands

Each operation takes an explicit JSON parameters object. `status` returns view state, role, layout and control revisions. `objects.list` returns editable objects with their local `target`, kind, position, yaw, and `shape` (`height` in 50 cm rows and a 24-bit `mask` for the 3×8 grid). It also returns the current revisions.

| Operation | Parameters | Effect |
|---|---|---|
| `status`, `objects.list` | `{}` | Read current state or objects |
| `view.set` | `{"view":"free|player1|player2"}` | Select a spectator view |
| `multiview.set`, `reconstructed.set`, `focus.set`, `build-mode.set` | `{"enabled":true}` | Set tiles, reconstructed view, released mouse, or building mode |
| `window.set` | `{"window":"player1|player2|inspector","open":true}` | Open or close one separate window |
| `menu.set` | `{"menu":"session|admin|login","open":true}` | Open or close a UI panel; opening login while connected requests a disconnect |
| `connection.set` | `{"connected":false}` | Request disconnection and the login screen; connect with `tools/client/device_login.py` and a new code |
| `camera.set` | `{"camera":"main|inspector","position":[0,0,650],"rotation":[-30,0,0]}` | Set the free camera or the separate inspector window's camera; rotation is pitch/yaw/roll in degrees |
| `admin.action` | `{"action":"pause", "layout_revision":1, "control_revision":1}` | `start`, `pause`, `resume`, `reset`, `confirm_floor`, `calibrate_floor`, `clear_objects`, or `set_dimensions`; the latter also takes `width_cm` and `length_cm` |
| `object.ensure` | `{"kind":"wall|crate","surface":[x,y,z],"origin":[x,y,z],"direction":[x,y,z],"yaw":0,"layout_revision":1,"control_revision":1}` | Ensure a snapped object exists; the normal authoritative ray, bounds and collision checks still apply |
| `wall.set` | `{"target":"<id>","expected":{"height":4,"mask":16777215},"desired":{"height":4,"mask":16777199},"layout_revision":1,"control_revision":1}` | Atomically set the whole wall shape if its expected shape still matches |
| `object.remove` | `{"target":"<id>","expected":{"height":4,"mask":16777215},"layout_revision":1,"control_revision":1}` | Remove precisely the referenced object when its expected shape still matches |
| `player.calibrate` | `{}` | Submit the player's existing calibration action |
| `haptics.set` | `{"state":"connected|armed|stopped|disconnected"}` | Submit local MoBits connection/arm/stop/disconnect; arming must be deliberate |
| `capture.set` | `{"enabled":true}` | Request Quest view sharing or stop it; the Quest OS consent still requires user approval |

`status` is a snapshot, so use a new operation UUID for a fresh snapshot. The layout/control revisions and expected wall shape must come from a recent result; a stale request is rejected rather than overwriting a concurrent change. For `object.ensure`, `surface` is the expected placement surface, while `origin` and the unit `direction` are the authoritative placement ray. World distances are centimetres. A matching existing object is reused; a new one uses the same placement limits as the GUI.

On Windows PowerShell 5.1, native argument quoting can remove quotes inside JSON. For nonempty parameters, create a UTF-8 JSON file with `@{view='free'} | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 <path>` and pass `--parameters-file <path>`; UTF-8 BOM is accepted. On Mac or Linux, `--parameters` also accepts a JSON string.

Windows example, run from the repository root:

```powershell
$id = [guid]::NewGuid().ToString()
.\.local\auth-tools\Scripts\python.exe tools\automation\control.py --name MRFPS-Client `
  --operation-id $id --operation status --parameters '{}'
```

Quest example, using the development APK and its connected serial:

```powershell
$id = [guid]::NewGuid().ToString()
.\.local\auth-tools\Scripts\python.exe tools\automation\control.py --platform quest --serial '<adb serial>' `
  --operation-id $id --operation status --parameters '{}'
```

`ok` / `already_applied` mean the operation completed; `submitted` means an asynchronous request was sent but device, capture or calibration completion was not confirmed; `pending` means no result has arrived. `error` and `unknown` are not success. Reuse the original UUID to check the same operation; `--wait-seconds` can be set from 0 to 60. The command does not print bearer credentials. This implementation has unit tests and still requires compiled-client, real-server, Quest and Mac integration testing.

Completed `ok`, `already_applied`, and `error` receipts can be moved out of UE's active directory scan without losing their retry history:

```powershell
.\.local\auth-tools\Scripts\python.exe tools\automation\control.py --name MRFPS-Client --archive-completed
# For a Quest development APK, also pass --platform quest --serial <adb serial>.
```

The command moves at most 100 entries by default; use `--archive-limit` (1–500) and repeat it to process more. Repeating it when there is nothing completed is safe. Pending, `submitted`, `unknown`, malformed and conflicting entries stay in place. Archived results remain available for the same UUID and payload; a different payload with the same UUID is rejected. Both desktop submission and archiving use the same local file lock. On Quest, both use the same app-private per-UUID lock across ADB hosts, and an unavailable lock fails closed. If an ADB shell is interrupted while holding a Quest lock, inspect it and confirm no publisher or archiver is active before manually removing the empty `.lock-<UUID>` directory; the CLI never guesses that the operation is safe to repeat. Archiving reduces the game tick's scan, but retains files and therefore does not reclaim disk space.
