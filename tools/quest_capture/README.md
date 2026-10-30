# Capture recovery checks

These checks need Python 3 and a JDK. They compile a small amount of Java into
temporary directories; they do not open a headset, invoke Unreal, or require
the Android SDK. `JAVA_HOME` or `java` / `javac` on `PATH` are used by default;
`--java` and `--javac` accept explicit executable paths.

```sh
python tools/quest_capture/test_recovery.py
python tools/quest_capture/test_service_fake.py
python tools/quest_capture/test_session_guards.py
```

The first command exercises the production retry policy. The second compiles
the production service and bridge against small platform fakes, checks the JNI
signature against the generated Java header, and runs the service with simulated
invalid frames, oversized JPEGs, native rejection, recovery, and a blocked
surface rebind. These checks are not a substitute for Android, JNI, GPU, or
Quest end-to-end testing.

The same fake service suite also blocks cleanup and the ready-state JNI call,
checks failure publication before cleanup returns, verifies the single-worker
gate across both thread exit and service destruction, and checks that old
Activity/stop callbacks and stale START / notification intents cannot terminate
a newer generation. The notification fake implements PendingIntent identity
and UPDATE_CURRENT to check generations differing in their upper 32 bits. A permanently
blocked worker intentionally prevents a new capture; restarting the app may be
needed. The watchdog no longer calls `MediaProjection.stop()` on the UI thread,
but Android foreground-service lifecycle operations still involve the OS.

The third command requires a C++17 compiler (`cl` in a Visual Studio developer
shell, or `c++` / `g++` / `clang++`). It tests native generation fencing and
nonblocking access while a simulated JNI worker holds the mailbox mutex. It
also reproduces a Busy response lost at that mutex while an older OS consent
dialog is pending, including draining the old dialog and protecting the next
generation.
