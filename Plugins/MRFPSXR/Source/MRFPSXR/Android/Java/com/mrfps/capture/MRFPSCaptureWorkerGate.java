package com.mrfps.capture;

import java.util.concurrent.atomic.AtomicBoolean;

/** One lease spans the actual lifetime of a capture HandlerThread. */
final class MRFPSCaptureWorkerGate {
    private static final AtomicBoolean busy = new AtomicBoolean();
    static boolean tryStart() { return busy.compareAndSet(false, true); }
    static boolean isBusy() { return busy.get(); }
    static void finish() { busy.set(false); }
    private MRFPSCaptureWorkerGate() {}
}
