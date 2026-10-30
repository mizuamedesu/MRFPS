package com.mrfps.capture;

import android.app.Activity;
import java.lang.reflect.Field;
import java.util.concurrent.atomic.AtomicLong;

/** Tests the actual Java generation guards without invoking native JNI. */
public final class MRFPSCaptureBridgeFakeTest {
    private static int assertions;
    private static void expect(boolean condition, String message) {
        ++assertions;
        if (!condition) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        Field field = MRFPSViewCapture.class.getDeclaredField("desiredGeneration");
        field.setAccessible(true);
        AtomicLong desired = (AtomicLong)field.get(null);
        expect(!MRFPSViewCapture.hasCaptureFailure(1), "unstarted request is not a failure");
        desired.set(1);
        MRFPSViewCapture.failSession(1);
        expect(MRFPSViewCapture.hasCaptureFailure(1), "watchdog marks the matching generation");
        expect(!MRFPSViewCapture.isRequested(1), "failed generation is retired");
        desired.set(2);
        MRFPSViewCapture.failSession(1);
        expect(MRFPSViewCapture.isRequested(2), "late old watchdog does not retire new generation");
        expect(!MRFPSViewCapture.hasCaptureFailure(2), "old failure does not taint new generation");
        Activity activity = new Activity();
        activity.queueUi = true;
        MRFPSViewCapture.stopForGeneration(activity, 1);
        activity.queued.run();
        expect(MRFPSViewCapture.isRequested(2) && activity.stops == 0, "late old stop cannot stop new service");
        MRFPSViewCapture.failSession(2);
        expect(MRFPSViewCapture.hasCaptureFailure(2), "new failure is tracked");
        MRFPSViewCapture.failSession(1);
        expect(MRFPSViewCapture.hasCaptureFailure(2), "late failure cannot overwrite the newer failure");
        desired.set(3);
        MRFPSViewCapture.stopForGeneration(activity, 3);
        activity.queued.run();
        expect(!MRFPSViewCapture.isRequested(3) && activity.stops == 1, "matching stop ends its own generation");
        Field pending = MRFPSViewCapture.class.getDeclaredField("pendingGeneration");
        pending.setAccessible(true);
        pending.setLong(null, 0);
        Activity oldActivity = new Activity(), newActivity = new Activity();
        MRFPSViewCapture.request(oldActivity, 4);
        expect(MRFPSViewCapture.isRequested(4), "old activity owns its request");
        MRFPSViewCapture.stopForGeneration(oldActivity, 4);
        pending.setLong(null, 0); // fixture simulates the old OS activity result draining
        MRFPSViewCapture.request(newActivity, 5);
        expect(MRFPSViewCapture.isRequested(5), "new activity owns the replacement request");
        MRFPSViewCapture.stop(oldActivity);
        expect(MRFPSViewCapture.isRequested(5) && oldActivity.stops == 1,
                "old Activity destruction cannot stop the new Activity's request");
        MRFPSViewCapture.stop(newActivity);
        expect(!MRFPSViewCapture.isRequested(5) && newActivity.stops == 1,
                "current Activity destruction stops its own request");
        expect(MRFPSCaptureWorkerGate.tryStart(), "first worker obtains the lease");
        expect(MRFPSCaptureWorkerGate.isBusy(), "a live or stuck worker keeps the lease");
        expect(!MRFPSCaptureWorkerGate.tryStart(), "a second worker is refused");
        MRFPSCaptureWorkerGate.finish();
        expect(MRFPSCaptureWorkerGate.tryStart(), "worker may restart after cleanup actually returns");
        MRFPSCaptureWorkerGate.finish();
        System.out.println("PASS MRFPSCaptureBridgeFake assertions=" + assertions);
    }
}
