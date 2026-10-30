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
