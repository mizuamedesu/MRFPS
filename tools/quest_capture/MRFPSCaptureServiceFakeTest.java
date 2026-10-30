package com.mrfps.capture;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.SystemClock;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Executes the real service against tiny in-process platform fakes, not Quest. */
public final class MRFPSCaptureServiceFakeTest {
    private static int assertions;
    private static void expect(boolean condition, String message) {
        ++assertions;
        if (!condition) throw new AssertionError(message);
    }
    private static Object get(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }
    private static void set(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(object, value);
    }
    private static Object call(Object object, String name, Class<?>[] types, Object... values) throws Exception {
        Method method = object.getClass().getDeclaredMethod(name, types);
        method.setAccessible(true);
        return method.invoke(object, values);
    }
    private static MRFPSViewCaptureService start() throws Exception {
        MRFPSViewCapture.requested = true;
        MRFPSViewCapture.desired = 1;
        MRFPSViewCapture.accepted = false;
        MRFPSViewCapture.lastState = -1;
        MRFPSViewCapture.frames = MRFPSViewCapture.ended = 0;
        MRFPSViewCapture.failed = 0;
        MRFPSViewCapture.stateHook = null;
        android.media.projection.MediaProjection.stops = 0;
        Bitmap.oversize = false;
        VirtualDisplay.afterSetSurface = null;
        VirtualDisplay.afterRelease = null;
        SystemClock.now = 1_000;
        MRFPSViewCaptureService service = new MRFPSViewCaptureService();
        set(service, "generation", 1L);
        set(service, "worker", new Handler());
        set(service, "startedAtMs", 1_000L);
        set(service, "workerAtMs", 1_000L);
        call(service, "beginProjection", new Class<?>[]{int.class, Intent.class}, Activity.RESULT_OK, new Intent());
        expect(MRFPSViewCapture.lastState == 2, "fake projection starts");
        return service;
    }
    private static void tick(MRFPSViewCaptureService service, long now, boolean validImage) throws Exception {
        SystemClock.now = now;
        ImageReader.image = validImage ? Image.valid() : Image.invalid();
        call(service, "onImage", new Class<?>[]{ImageReader.class}, get(service, "reader"));
        call(service, "checkProgress", new Class<?>[]{});
    }
    private static void persistent(boolean validImage, boolean oversize) throws Exception {
        MRFPSViewCaptureService service = start();
        Bitmap.oversize = oversize;
        for (long now = 1_000; now <= 25_000; now += 1_000) tick(service, now, validImage);
        expect((Integer)get(service, "surfaceRebinds") == 3, "persistent failure stops after three rebinds");
        expect((Boolean)get(service, "released"), "persistent failure releases service");
        expect(MRFPSViewCapture.lastState == 4, "persistent failure reports unavailable");
        expect(MRFPSViewCapture.ended > 0 && !MRFPSViewCapture.requested,
                "stopped service retires its session");
        if (oversize) expect(MRFPSViewCapture.frames == 0, "oversized JPEG was never submitted");
        if (validImage && !oversize) expect(MRFPSViewCapture.frames > 0, "native rejections really reached bridge");
    }
    private static void acceptedFrameReallyResetsRecovery() throws Exception {
        MRFPSViewCaptureService service = start();
        for (long now = 1_000; now <= 7_000; now += 1_000) tick(service, now, false);
        expect((Integer)get(service, "surfaceRebinds") == 2, "two bad-image rebinds occurred");
        MRFPSViewCapture.accepted = true;
        tick(service, 8_000, true);
        expect((Long)get(service, "jniAccepted") == 1, "native mailbox acceptance is counted");
        MRFPSCaptureRecoveryPolicy policy = (MRFPSCaptureRecoveryPolicy)get(service, "recovery");
        expect(policy.badOutputRebinds() == 0, "accepted native frame resets the attempt count");
        expect(!(Boolean)get(service, "released"), "accepted frame keeps session active");
    }
    private static void stopDuringSurfaceRebindIsRespected() throws Exception {
        MRFPSViewCaptureService service = start();
        VirtualDisplay.afterSetSurface = () -> {
            try { set(service, "stopRequested", true); }
            catch (Exception failure) { throw new RuntimeException(failure); }
        };
        for (long now = 1_000; now <= 4_000; now += 1_000) tick(service, now, false);
        expect((Boolean)get(service, "released"), "stop during rebind releases session");
        expect(MRFPSViewCapture.lastState == 0, "stop is not converted into a recovery failure");
        expect(!MRFPSViewCapture.requested, "stopped generation cannot continue");
    }
    private static void blockedRebindIsWatched() throws Exception {
        MRFPSViewCaptureService service = start();
        for (long now = 1_000; now <= 3_000; now += 1_000) tick(service, now, false);
        CountDownLatch entered = new CountDownLatch(1), unblock = new CountDownLatch(1);
        AtomicReference<Throwable> workerError = new AtomicReference<>();
        VirtualDisplay.afterSetSurface = () -> {
            entered.countDown();
            try { if (!unblock.await(5, TimeUnit.SECONDS)) throw new AssertionError("rebind fixture timed out"); }
            catch (InterruptedException failure) { throw new RuntimeException(failure); }
        };
        Thread worker = new Thread(() -> {
            try { tick(service, 4_000, false); }
            catch (Throwable failure) { workerError.set(failure); }
        });
        worker.start();
        expect(entered.await(5, TimeUnit.SECONDS), "worker entered the real rebind call");
        Runnable watchdog = (Runnable)get(service, "watchdog");
        SystemClock.now = 17_000;
        watchdog.run();
        expect(MRFPSViewCapture.ended == 0, "first observation does not kill a suspended app");
        SystemClock.now = 20_000;
        watchdog.run();
        expect(MRFPSViewCapture.ended == 1, "confirmed stuck rebind retires generation");
        expect((Boolean)get(service, "stopRequested"), "watchdog requests stop");
        expect(service.stopped, "watchdog stops the foreground service");
        expect(android.media.projection.MediaProjection.stops == 0,
                "watchdog does not invoke MediaProjection.stop on the UI thread");
        unblock.countDown();
        worker.join(5_000);
        expect(!worker.isAlive() && workerError.get() == null, "worker drains after platform call returns");
        expect((Boolean)get(service, "released"), "resuming old rebind completes cleanup");
        expect(!MRFPSViewCapture.requested, "old rebind cannot revive sharing");
    }
    private static void blockedCleanupPublishesFailureFirst() throws Exception {
        MRFPSViewCaptureService service = start();
        CountDownLatch entered = new CountDownLatch(1), unblock = new CountDownLatch(1);
        AtomicReference<Throwable> workerError = new AtomicReference<>();
        VirtualDisplay.afterRelease = () -> {
            entered.countDown();
            try { if (!unblock.await(5, TimeUnit.SECONDS)) throw new AssertionError("cleanup fixture timed out"); }
            catch (InterruptedException failure) { throw new RuntimeException(failure); }
        };
        int postsBefore = Handler.posts;
        Thread worker = new Thread(() -> {
            try { call(service, "release", new Class<?>[]{int.class}, 4); }
            catch (Throwable failure) { workerError.set(failure); }
        });
        worker.start();
        expect(entered.await(5, TimeUnit.SECONDS), "worker entered blocked platform cleanup");
        expect(MRFPSViewCapture.failed == 1 && !MRFPSViewCapture.requested,
                "generation failure is published before platform cleanup returns");
        expect(Handler.posts > postsBefore, "foreground shutdown was scheduled before cleanup blocks");
        expect(MRFPSViewCapture.lastState == 2, "fixture has not falsely delivered the later JNI status");
        unblock.countDown();
        worker.join(5_000);
        expect(!worker.isAlive() && workerError.get() == null, "cleanup finishes after platform unblocks");
        expect(MRFPSViewCapture.lastState == 4, "native failure is eventually reported");
    }
    private static void blockedReadyCallbackIsWatched() throws Exception {
        MRFPSViewCapture.requested = true;
        MRFPSViewCapture.ended = 0;
        MRFPSViewCapture.failed = 0;
        SystemClock.now = 1_000;
        MRFPSViewCaptureService service = new MRFPSViewCaptureService();
        set(service, "generation", 1L);
        set(service, "worker", new Handler());
        set(service, "startedAtMs", 1_000L);
        set(service, "workerAtMs", 1_000L);
        CountDownLatch entered = new CountDownLatch(1), unblock = new CountDownLatch(1);
        AtomicReference<Throwable> workerError = new AtomicReference<>();
        MRFPSViewCapture.stateHook = () -> {
            entered.countDown();
            try { if (!unblock.await(5, TimeUnit.SECONDS)) throw new AssertionError("JNI fixture timed out"); }
            catch (InterruptedException failure) { throw new RuntimeException(failure); }
        };
        Thread worker = new Thread(() -> {
            try { call(service, "beginProjection", new Class<?>[]{int.class, Intent.class}, Activity.RESULT_OK, new Intent()); }
            catch (Throwable failure) { workerError.set(failure); }
        });
        worker.start();
        expect(entered.await(5, TimeUnit.SECONDS), "worker entered blocked ready-state JNI callback");
        expect("jni".equals(get(service, "stage")), "ready-state callback is watched as JNI");
        Runnable watchdog = (Runnable)get(service, "watchdog");
        SystemClock.now = 17_000;
        watchdog.run();
        SystemClock.now = 20_000;
        watchdog.run();
        expect(MRFPSViewCapture.failed == 1 && service.stopped, "watchdog publishes failure without waiting for JNI");
        MRFPSViewCapture.stateHook = null;
        unblock.countDown();
        worker.join(5_000);
        expect(!worker.isAlive() && workerError.get() == null && (Boolean)get(service, "released"),
                "late JNI return drains the old session safely");
    }
    private static void onlyOneCaptureWorkerCanRemainLive() throws Exception {
        MRFPSViewCapture.requested = true;
        MRFPSViewCapture.desired = 1;
        MRFPSViewCapture.lastState = -1;
        Intent intent = new Intent().setAction(MRFPSViewCaptureService.ACTION_START)
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 1L)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_CODE, Activity.RESULT_OK)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_DATA, new Intent());
        MRFPSViewCaptureService first = new MRFPSViewCaptureService();
        first.onStartCommand(intent, 0, 1);
        expect(MRFPSCaptureWorkerGate.isBusy(), "service holds the worker lease");
        MRFPSViewCaptureService second = new MRFPSViewCaptureService();
        Intent secondIntent = new Intent().setAction(MRFPSViewCaptureService.ACTION_START)
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 1L)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_CODE, Activity.RESULT_OK)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_DATA, new Intent());
        second.onStartCommand(secondIntent, 0, 2);
        expect(second.stopped && MRFPSViewCapture.lastState == 4, "a second worker fails closed");
        expect(MRFPSCaptureWorkerGate.isBusy(), "rejected start cannot release the first worker's lease");
        ((android.os.HandlerThread)get(first, "thread")).run();
        expect(MRFPSCaptureWorkerGate.isBusy(), "thread exit alone cannot permit reuse before service destruction");
        first.onDestroy();
        expect(!MRFPSCaptureWorkerGate.isBusy(), "lease releases after the owning thread and service both end");
        MRFPSViewCapture.requested = true;
        MRFPSViewCaptureService third = new MRFPSViewCaptureService();
        Intent thirdIntent = new Intent().setAction(MRFPSViewCaptureService.ACTION_START)
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 1L)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_CODE, Activity.RESULT_OK)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_DATA, new Intent());
        third.onStartCommand(thirdIntent, 0, 3);
        third.onDestroy();
        expect(MRFPSCaptureWorkerGate.isBusy(), "service destruction does not hide a still-live worker");
        ((android.os.HandlerThread)get(third, "thread")).run();
        expect(!MRFPSCaptureWorkerGate.isBusy(), "gate opens after the remaining worker exits");
    }
    private static void staleStartCannotStopNewWorker() throws Exception {
        MRFPSViewCaptureService service = start();
        MRFPSViewCapture.desired = 2;
        set(service, "generation", 2L);
        set(service, "serviceStartId", 1);
        set(service, "thread", new android.os.HandlerThread("test-worker"));
        Intent duplicate = new Intent().setAction(MRFPSViewCaptureService.ACTION_START)
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 2L);
        service.onStartCommand(duplicate, 0, 98);
        expect(!service.stopped && (Integer)get(service, "serviceStartId") == 98,
                "duplicate current START leaves the worker alive");
        Intent old = new Intent().setAction(MRFPSViewCaptureService.ACTION_START)
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 1L);
        service.onStartCommand(old, 0, 99);
        expect(!service.stopped && MRFPSViewCapture.isRequested(2), "old START does not stop the new worker");
        expect((Integer)get(service, "serviceStartId") == 99, "ignored START still records Android's new startId");
        service.onStartCommand(null, 0, 100);
        service.onStartCommand(new Intent().setAction("invalid.action"), 0, 101);
        expect(!service.stopped && MRFPSViewCapture.isRequested(2), "null and invalid starts do not stop the active worker");
        Intent oldStop = new Intent().setAction("com.mrfps.capture.STOP")
                .setData(android.net.Uri.parse("mrfps-capture://stop/1"))
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 1L);
        service.onStartCommand(oldStop, 0, 102);
        expect(!service.stopped && MRFPSViewCapture.isRequested(2), "old notification STOP does not stop the new worker");
        service.onStartCommand(old, 0, 99);
        expect((Integer)get(service, "serviceStartId") == 102, "an older delivered ID cannot replace the latest startId");
        call(service, "release", new Class<?>[]{int.class}, 0);
        // Model an ignored onStartCommand which passed its active check just
        // before worker release, but published its newer ID just afterward.
        set(service, "serviceStartId", 103);
        Handler.lastPosted.run();
        expect(service.stopped && service.lastStopId == 103,
                "normal cleanup uses the latest startId after ignored intents");
        MRFPSViewCapture.requested = true;
        MRFPSViewCapture.desired = 2;
        MRFPSViewCaptureService empty = new MRFPSViewCaptureService();
        empty.onStartCommand(old, 0, 104);
        expect(empty.stopped && empty.lastStopId == 104, "invalid start still stops an inactive service");
        MRFPSViewCaptureService active = start();
        set(active, "thread", new android.os.HandlerThread("current-worker"));
        Intent currentStop = new Intent().setAction("com.mrfps.capture.STOP")
                .setData(android.net.Uri.parse("mrfps-capture://stop/1"))
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 1L);
        active.onStartCommand(currentStop, 0, 105);
        expect(active.stopped, "current notification STOP still stops its own service");
    }
    private static void notificationIdentityIsGenerationScoped() throws Exception {
        MRFPSViewCaptureService first = new MRFPSViewCaptureService();
        set(first, "generation", 1L);
        call(first, "createNotification", new Class<?>[]{});
        android.app.PendingIntent old = android.app.PendingIntent.lastCreated;

        long wideGeneration = 1L + (1L << 32);
        MRFPSViewCaptureService next = new MRFPSViewCaptureService();
        set(next, "generation", wideGeneration);
        set(next, "thread", new android.os.HandlerThread("new-worker"));
        call(next, "createNotification", new Class<?>[]{});
        android.app.PendingIntent newer = android.app.PendingIntent.lastCreated;
        expect(old != newer, "generations differing in high 32 bits get distinct PendingIntent tokens");
        expect(old.intent.getLongExtra(MRFPSViewCaptureService.EXTRA_GENERATION, 0) == 1L,
                "UPDATE_CURRENT for the new token does not rewrite the old token");
        MRFPSViewCaptureService highest = new MRFPSViewCaptureService();
        set(highest, "generation", Long.MAX_VALUE);
        call(highest, "createNotification", new Class<?>[]{});
        expect(android.app.PendingIntent.lastCreated != old && android.app.PendingIntent.lastCreated != newer,
                "the maximum supported signed 64-bit generation also has its own token");

        MRFPSViewCapture.desired = wideGeneration;
        MRFPSViewCapture.requested = true;
        next.onStartCommand(old.intent, 0, 200);
        expect(!next.stopped && MRFPSViewCapture.isRequested(wideGeneration),
                "delayed old notification token cannot stop the new generation");
        Intent mismatched = new Intent().setAction("com.mrfps.capture.STOP")
                .setData(android.net.Uri.parse("mrfps-capture://stop/1"))
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, wideGeneration);
        next.onStartCommand(mismatched, 0, 201);
        expect(!next.stopped, "mismatched identity and extra are rejected");
        next.onStartCommand(newer.intent, 0, 202);
        expect(next.stopped, "the matching current token can stop its own service");
    }
    public static void main(String[] args) throws Exception {
        persistent(false, false); // invalid ImageReader dimensions/planes
        persistent(true, true); // every JPEG attempt exceeds the byte cap
        persistent(true, false); // valid-looking JPEG rejected by native mailbox
        acceptedFrameReallyResetsRecovery();
        stopDuringSurfaceRebindIsRespected();
        blockedRebindIsWatched();
        blockedCleanupPublishesFailureFirst();
        blockedReadyCallbackIsWatched();
        onlyOneCaptureWorkerCanRemainLive();
        staleStartCannotStopNewWorker();
        notificationIdentityIsGenerationScoped();
        System.out.println("PASS MRFPSCaptureServiceFake assertions=" + assertions);
    }
}
