package com.mrfps.capture;

/** Worker-owned policy; intentionally independent of Android for local tests. */
final class MRFPSCaptureRecoveryPolicy {
    static final long BAD_OUTPUT_WINDOW_MS = 3_000;
    static final long BAD_OUTPUT_GAP_MS = 5_000;
    static final int MAX_BAD_OUTPUT_REBINDS = 3;

    enum Decision { WAIT, REBIND, STOP }

    private long badSinceMs = -1;
    private long lastBadAtMs = -1;
    private int badSamples;
    private volatile int badOutputRebinds;

    static boolean watchdogBusy(String stage) {
        return "acquire".equals(stage) || "pixels".equals(stage) || "jpeg".equals(stage)
                || "jni".equals(stage) || "projection_start".equals(stage)
                || "surface_rebind".equals(stage);
    }

    void badOutput(long now) {
        if (now < 0) return;
        if (lastBadAtMs < 0 || now < lastBadAtMs || now - lastBadAtMs > BAD_OUTPUT_GAP_MS) {
            badSinceMs = now;
            badSamples = 0;
        }
        lastBadAtMs = now;
        if (badSamples < Integer.MAX_VALUE) ++badSamples;
    }

    // Only a frame actually accepted by the current native mailbox counts.
    // In particular, ImageReader activity and a JNI return alone are not output.
    void acceptedOutput() {
        badSinceMs = lastBadAtMs = -1;
        badSamples = 0;
        badOutputRebinds = 0;
    }

    // Do not judge failures accumulated while system UI hides the capture.
    // Keep the attempt count until a real accepted frame proves recovery.
    void suspend() { badSinceMs = lastBadAtMs = -1; badSamples = 0; }

    int badOutputRebinds() { return badOutputRebinds; }

    // 'eligible' is the shared, backoff-limited surface-rebind deadline. A
    // paused/invisible/stopped session must neither rebind nor exhaust retries.
    Decision decide(long now, boolean active, boolean eligible) {
        if (!active || !eligible || badSamples < 2 || lastBadAtMs < 0 || now < lastBadAtMs ||
                now - lastBadAtMs > BAD_OUTPUT_GAP_MS || badSinceMs < 0 ||
                now - badSinceMs < BAD_OUTPUT_WINDOW_MS) return Decision.WAIT;
        if (badOutputRebinds >= MAX_BAD_OUTPUT_REBINDS) return Decision.STOP;
        ++badOutputRebinds;
        return Decision.REBIND;
    }
}
