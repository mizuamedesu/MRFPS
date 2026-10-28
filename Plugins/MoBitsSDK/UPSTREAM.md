# Upstream source

Source: [MoBits-Project/UE5_SDK](https://github.com/MoBits-Project/UE5_SDK), branch `codex/initial-sdk`, commit `6d657216b990948581684f61ae18ddbc9ca9116a` (2026-10-13), SDK 0.5.0.

The plugin source and documentation were obtained from the user's connected GitHub account. The upstream repository is private and did not contain a root license file at this revision; upstream ownership and distribution terms are retained.

MRFPS changes:

- Added Android and Linux to the plugin platform lists so the portable Sockets implementation can compile for Quest and the server target.
- Android discovery reads `Context.getFilesDir()/MoBits/SDK/connection-v2.json` through UE5.8's public `AndroidJavaEnv` JNI API. This is internal app-private storage; `FPaths::ProjectPersistentDownloadDir()` returns external storage on this engine and is not used for credentials. JNI failure returns no path, without an external-storage fallback.
- `tools/mobits/quest_bridge.py` validates `run-as <package> pwd` and transfers the desktop discovery bytes to internal `files/MoBits/SDK` through ADB stdin as the app UID, with directory mode 700, file mode 600 and atomic rename. It requires a development APK that permits `run-as` and reverses the same loopback TCP port. No protocol, token, loopback restriction, ARM, heartbeat, STOP, or preset behavior was changed.

The supported-platform extension is integration code, not evidence of an Android/Linux runtime test. See `Docs/MoBits.md` for the actual verification scope.
