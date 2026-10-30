# Flash game health

Flash runs the game as UID 10001 with capabilities removed and
`no_new_privileges`. In the observed gVisor Pod, `/proc/1/fd` is root-owned and
cannot be inspected by the game UID. The former FD-inode-only healthcheck
therefore fails even when the game is listening.

Linux dedicated servers now offer a bounded, non-blocking abstract Unix socket
that is serviced on the game thread. The response echoes a fresh nonce and
contains the live NetDriver port only when authenticated admission and the
NetDriver are ready. The Flash healthcheck requires the exact game executable
at PID 1, UID 10001, a bound UDP port, a matching `SO_PEERCRED` PID and UID,
the nonce, expected port, and both readiness flags. The abstract socket has no
filesystem path and creates no public TCP/UDP endpoint. Ordinary Docker keeps
the original strict FD-inode path; the local-socket fallback is enabled only by
the Flash healthcheck wrapper.

Run `Scripts/Cloud/Test-FlashHealth.ps1` on the Windows development host to
compile a small Linux fixture and run the tests under UID 10001 with no
capabilities and `no_new_privileges` in WSL. The command checks the existing
10 GiB / 75% memory thresholds and uses an ownership-marked test directory.
The tests exercise the compiled socket responder, fragmented requests, stale
and wrong nonces, wrong PID and port, stopped game-thread processing, missing
or stopped listeners, invalid auth state, and unrelated sockets. This fixture
does not itself build Unreal or prove the UE integration or a new Flash deployment.
The 2026-10-30 Flash generation 4 subsequently passed the real healthcheck twice;
wrong PID, UID and port were rejected on that actual gVisor Pod. Windows and
Quest also authenticated and joined that generation.
