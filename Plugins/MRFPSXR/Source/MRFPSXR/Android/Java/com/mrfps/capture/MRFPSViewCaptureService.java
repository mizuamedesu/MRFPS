package com.mrfps.capture;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.drawable.Icon;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.FileDescriptor;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

/** One foreground, one-use MediaProjection session; no audio and no recording. */
public final class MRFPSViewCaptureService extends Service {
    static final String ACTION_START = "com.mrfps.capture.START";
    private static final String ACTION_STOP = "com.mrfps.capture.STOP";
    static final String EXTRA_GENERATION = "generation";
    static final String EXTRA_RESULT_CODE = "resultCode";
    static final String EXTRA_RESULT_DATA = "resultData";
    private static final int WIDTH = 640;
    private static final int HEIGHT = 360;
    private static final int MAX_JPEG_BYTES = 40 * 1024;
    private static final long FRAME_PERIOD_NS = 33_333_333L;
    private static final int[] JPEG_QUALITIES = {65, 50, 35, 20, 12, 8};
    private static final String CHANNEL = "mrfps_headset_view";
    private static final int NOTIFICATION_ID = 58481;
    private static final String TAG = "MRFPSViewCapture";
    private static final long SURFACE_STALL_MS = 3_000;
    private static final long WORKER_STALL_MS = 15_000;
    private static final long DIAGNOSTIC_PERIOD_MS = 10_000;

    private final Handler main = new Handler(Looper.getMainLooper());
    private HandlerThread thread;
    private Handler worker;
    private volatile boolean stopRequested;
    // Fields below are owned by the worker, except generation assigned before start.
    private volatile long generation;
    private volatile boolean released;
    private volatile MediaProjection projection;
    private MediaProjection.Callback projectionCallback;
    private VirtualDisplay display;
    private ImageReader reader;
    private Bitmap bitmap;
    private int[] pixels;
    private BoundedJpegOutput jpeg;
    private long nextPumpDeadlineNs;
    private int jpegQualityIndex, qualityRecoveryFrames;
    // Safe metadata only. Volatile snapshots also support Service.dump while
    // the capture worker is stalled; no pixels, tokens, Intents or paths appear.
    private volatile String stage = "created";
    private volatile long startedAtMs, workerAtMs, lastImageAtMs, lastAcceptedAtMs;
    private volatile long acquiredImages, invalidImages, jpegDrops, jniCalls, jniReturns, skippedGeneration;
    private volatile long jniAccepted, nativeRejected;
    private volatile long nullImages, duplicateImages, lastImageTimestampNs;
    private volatile int lastWidth, lastHeight, lastPlanes, lastPixelStride, lastRowStride, lastJpegBytes, lastQuality;
    private volatile int surfaceRebinds;
    private long nextSurfaceRecoveryAtMs;
    private long surfaceRecoveryDelayMs = SURFACE_STALL_MS;
    private final MRFPSCaptureRecoveryPolicy recovery = new MRFPSCaptureRecoveryPolicy();
    private volatile boolean contentVisible = true;
    private volatile boolean waitingForOutput;
    private volatile int serviceStartId;
    private volatile boolean ownsWorkerGate, workerExited, serviceDestroyed;
    private final AtomicBoolean workerGateFinished = new AtomicBoolean();
    private long lastDiagnosticMs;
    private long observedWorkerAtMs, stalledObservedAtMs;

    // Exactly one delayed worker task. Polling avoids relying on an ImageReader
    // listener event that can stop arriving, and avoids a callback-message backlog.
    private final Runnable pump = new Runnable() {
        @Override public void run() {
            if (released) return;
            if (nextPumpDeadlineNs == 0) nextPumpDeadlineNs = SystemClock.elapsedRealtimeNanos();
            workerAtMs = SystemClock.elapsedRealtime();
            onImage(reader);
            workerAtMs = SystemClock.elapsedRealtime();
            if (released) return;
            stage = "idle";
            checkProgress();
            if (!released && worker != null) {
                // Deadlines include processing time. A slow encoder drops elapsed
                // slots and immediately samples latest data; it never queues old
                // frames or adds a further 33ms to the processing duration.
                nextPumpDeadlineNs += FRAME_PERIOD_NS;
                long nowNs = SystemClock.elapsedRealtimeNanos();
                if (nextPumpDeadlineNs < nowNs - FRAME_PERIOD_NS) nextPumpDeadlineNs = nowNs;
                long delayMs = Math.max(0L, (nextPumpDeadlineNs - nowNs + 999_999L) / 1_000_000L);
                worker.postDelayed(this, delayMs);
            }
        }
    };

    // The main-thread observer does not read/close a worker-owned image/bitmap.
    // A wedged encoder/JNI must not leave projection/FGS silently active forever.
    private final Runnable watchdog = new Runnable() {
        @Override public void run() {
            if (released || stopRequested) return;
            long now = SystemClock.elapsedRealtime();
            if (now - lastDiagnosticMs >= DIAGNOSTIC_PERIOD_MS) {
                lastDiagnosticMs = now;
                Log.i(TAG, diagnostics("heartbeat", now));
            }
            boolean busy = MRFPSCaptureRecoveryPolicy.watchdogBusy(stage);
            if (!busy || workerAtMs == 0 || now - workerAtMs < WORKER_STALL_MS) {
                stalledObservedAtMs = 0;
            } else if (stalledObservedAtMs == 0 || observedWorkerAtMs != workerAtMs) {
                // A process can be suspended while the headset is unworn. Require
                // several live main-thread observations before calling it wedged.
                observedWorkerAtMs = workerAtMs;
                stalledObservedAtMs = now;
            } else if (now - stalledObservedAtMs >= 3_000) {
                Log.w(TAG, diagnostics("worker_timeout", now));
                stopRequested = true;
                // No JNI and no MediaProjection.stop() Binder call on this UI
                // thread: either can be blocked by the worker/platform.
                MRFPSViewCapture.failSession(generation);
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf();
                // Native freshness naturally expires. Do not block the main
                // thread in JNI if the worker is itself wedged inside JNI.
                return;
            }
            main.postDelayed(this, 1_000);
        }
    };

    @Override public IBinder onBind(Intent intent) { return null; }

    private boolean keepActiveOnInvalidStart(int startId) {
        if (thread == null || stopRequested || released || !MRFPSViewCapture.isRequested(generation)) return false;
        // Even an ignored START advances Android's startId. Remember it so the
        // eventual cleanup can stop this same service after the worker ends.
        serviceStartId = Math.max(serviceStartId, startId);
        Log.i(TAG, "MRFPS_CAPTURE_IGNORED_STALE_START generation=" + generation);
        return true;
    }

    private static Uri stopIdentity(long generation) {
        // Intent extras are excluded from PendingIntent identity. The full
        // 64-bit generation in data keeps an old notification token separate.
        return Uri.parse("mrfps-capture://stop/" + generation);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            if (!keepActiveOnInvalidStart(startId)) stopSelf(startId);
            return START_NOT_STICKY;
        }
        if (ACTION_STOP.equals(intent.getAction())) {
            final long requestedStop = intent.getLongExtra(EXTRA_GENERATION, 0);
            if (requestedStop == generation && MRFPSViewCapture.isRequested(generation) &&
                    stopIdentity(generation).equals(intent.getData())) stopSelf();
            else if (!keepActiveOnInvalidStart(startId)) stopSelf(startId);
            return START_NOT_STICKY;
        }
        final long requested = intent.getLongExtra(EXTRA_GENERATION, 0);
        if (!ACTION_START.equals(intent.getAction()) || !MRFPSViewCapture.isRequested(requested)) {
            if (!keepActiveOnInvalidStart(startId)) stopSelf(startId);
            return START_NOT_STICKY;
        }
        if (thread != null) {
            if (requested == generation && !stopRequested && !released) {
                serviceStartId = Math.max(serviceStartId, startId);
                Log.i(TAG, "MRFPS_CAPTURE_DUPLICATE_START generation=" + requested);
                return START_NOT_STICKY;
            }
            // Never call getMediaProjection/createVirtualDisplay twice on a grant.
            MRFPSViewCapture.reportState(requested, 4);
            MRFPSViewCapture.sessionEnded(requested);
            stopSelf();
            return START_NOT_STICKY;
        }
        generation = requested;
        serviceStartId = startId;
        Log.i(TAG, "MRFPS_CAPTURE_SERVICE_START generation=" + generation);
        final int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED);
        final Intent resultData = readResultData(intent);
        // Drop service Intent references immediately; no token persistence/reuse.
        intent.removeExtra(EXTRA_RESULT_DATA);
        if (resultCode != Activity.RESULT_OK || resultData == null) {
            MRFPSViewCapture.reportState(generation, 4);
            MRFPSViewCapture.sessionEnded(generation);
            stopSelf();
            return START_NOT_STICKY;
        }
        if (!MRFPSCaptureWorkerGate.tryStart()) {
            MRFPSViewCapture.reportState(generation, 4);
            MRFPSViewCapture.sessionEnded(generation);
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        ownsWorkerGate = true;
        boolean threadStarted = false;
        try {
            Notification notification = createNotification();
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, notification,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
            } else {
                startForeground(NOTIFICATION_ID, notification);
            }
            thread = new HandlerThread("MRFPSHeadsetView") {
                @Override public void run() {
                    try { super.run(); }
                    finally { workerExited = true; finishWorkerGateIfComplete(); }
                }
            };
            thread.start();
            threadStarted = true;
            worker = new Handler(thread.getLooper());
            startedAtMs = workerAtMs = SystemClock.elapsedRealtime();
            main.postDelayed(watchdog, 1_000);
            worker.post(() -> beginProjection(resultCode, resultData));
        } catch (RuntimeException failure) {
            if (threadStarted && thread != null) thread.quitSafely();
            else { workerExited = true; finishWorkerGateIfComplete(); }
            MRFPSViewCapture.reportState(generation, 4);
            MRFPSViewCapture.sessionEnded(generation);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @SuppressWarnings("deprecation") // API 32 requires the untyped Parcelable overload.
    private static Intent readResultData(Intent intent) {
        return Build.VERSION.SDK_INT >= 33
                ? intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent.class)
                : intent.getParcelableExtra(EXTRA_RESULT_DATA);
    }

    private Notification createNotification() {
        // The project minimum is API 32, so notification channels are mandatory.
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                "Headset screen sharing", NotificationManager.IMPORTANCE_LOW));
        Intent stop = new Intent(this, MRFPSViewCaptureService.class).setAction(ACTION_STOP)
                .setData(stopIdentity(generation))
                .putExtra(EXTRA_GENERATION, generation);
        PendingIntent stopIntent = PendingIntent.getService(this, NOTIFICATION_ID, stop,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = new Notification.Builder(this, CHANNEL);
        return builder.setSmallIcon(android.R.drawable.ic_menu_camera)
                .setContentTitle("MRFPS headset view")
                .setContentText("Your headset view is being shared. Tap Stop to end sharing.")
                .setOngoing(true).setCategory(Notification.CATEGORY_SERVICE)
                .setVisibility(Notification.VISIBILITY_PRIVATE)
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_media_pause),
                        "Stop", stopIntent).build())
                .build();
    }

    private void beginProjection(int resultCode, Intent resultData) {
        if (stopRequested || !MRFPSViewCapture.isRequested(generation)) {
            release(0);
            return;
        }
        try {
            stage = "projection_start";
            MediaProjectionManager manager = (MediaProjectionManager)
                    getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            projection = manager.getMediaProjection(resultCode, resultData); // Exactly once.
            if (projection == null) { release(4); return; }
            if (stopRequested || !MRFPSViewCapture.isRequested(generation)) { release(0); return; }
            projectionCallback = new MediaProjection.Callback() {
                @Override public void onStop() {
                    Log.i(TAG, "MRFPS_CAPTURE_PROJECTION_STOP generation=" + generation);
                    release(5);
                }
                @Override public void onCapturedContentVisibilityChanged(boolean visible) {
                    contentVisible = visible;
                    if (visible) nextSurfaceRecoveryAtMs = 0;
                    Log.i(TAG, "MRFPS_CAPTURE_VISIBILITY generation=" + generation + " visible=" + visible);
                }
            };
            projection.registerCallback(projectionCallback, worker);
            reader = ImageReader.newInstance(WIDTH, HEIGHT, PixelFormat.RGBA_8888, 2);
            pixels = new int[WIDTH * HEIGHT];
            bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
            jpeg = new BoundedJpegOutput(MAX_JPEG_BYTES);
            // Horizon OS's compositor writes this Surface directly. Do not rely on
            // VirtualDisplay.resize or Android's usual letterboxing semantics.
            display = projection.createVirtualDisplay("MRFPSHeadsetView", WIDTH, HEIGHT,
                    getResources().getConfiguration().densityDpi,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                    reader.getSurface(), new VirtualDisplay.Callback() {
                        @Override public void onPaused() { Log.i(TAG, "MRFPS_CAPTURE_DISPLAY_PAUSED generation=" + generation); }
                        @Override public void onResumed() { Log.i(TAG, "MRFPS_CAPTURE_DISPLAY_RESUMED generation=" + generation); }
                        @Override public void onStopped() { release(5); }
                    }, worker); // One display per consent.
            if (display == null) { release(4); return; }
            if (stopRequested || !MRFPSViewCapture.isRequested(generation)) { release(0); return; }
            stage = "idle";
            workerAtMs = SystemClock.elapsedRealtime();
            Log.i(TAG, diagnostics("projection_ready", workerAtMs));
            stage = "jni";
            MRFPSViewCapture.reportState(generation, 2);
            workerAtMs = SystemClock.elapsedRealtime();
            if (stopRequested || !MRFPSViewCapture.isRequested(generation)) { release(0); return; }
            stage = "idle";
            worker.post(pump);
        } catch (RuntimeException failure) {
            Log.w(TAG, diagnostics("projection_error", SystemClock.elapsedRealtime()));
            release(4);
        }
    }

    private void onImage(ImageReader source) {
        if (released) return;
        if (stopRequested || !MRFPSViewCapture.isRequested(generation)) { release(0); return; }
        stage = "acquire";
        try (Image image = source == null ? null : source.acquireLatestImage()) {
            // Always drain the two-image reader, even when rate-limiting.
            if (image == null) { ++nullImages; return; }
            ++acquiredImages;
            long timestamp = image.getTimestamp();
            if (timestamp > 0 && timestamp <= lastImageTimestampNs) { ++duplicateImages; return; }
            if (timestamp > 0) lastImageTimestampNs = timestamp;
            lastImageAtMs = SystemClock.elapsedRealtime();
            lastWidth = image.getWidth(); lastHeight = image.getHeight(); lastPlanes = image.getPlanes().length;
            if (lastWidth != WIDTH || lastHeight != HEIGHT || lastPlanes != 1) {
                ++invalidImages; recovery.badOutput(lastImageAtMs); return;
            }
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer rgba = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int rowStride = plane.getRowStride();
            lastPixelStride = pixelStride; lastRowStride = rowStride;
            int base = rgba.position();
            long lastByte = (long) base + (HEIGHT - 1L) * rowStride + (WIDTH - 1L) * pixelStride + 3;
            if (pixelStride != 4 || rowStride < WIDTH * 4 || lastByte >= rgba.limit()) {
                ++invalidImages; recovery.badOutput(lastImageAtMs); return;
            }
            stage = "pixels";
            rgba.order(ByteOrder.BIG_ENDIAN);
            for (int y = 0; y < HEIGHT; ++y) {
                int row = base + y * rowStride;
                int out = y * WIDTH;
                for (int x = 0; x < WIDTH; ++x) {
                    int position = row + x * pixelStride;
                    // RGBA bytes -> opaque ARGB in one packed read per pixel.
                    pixels[out + x] = 0xff000000 | (rgba.getInt(position) >>> 8);
                }
            }
            bitmap.setPixels(pixels, 0, WIDTH, 0, 0, WIDTH, HEIGHT);
            stage = "jpeg";
            // Reuse one capped output buffer. Never publish an oversized JPEG.
            for (int level = jpegQualityIndex; level < JPEG_QUALITIES.length; ++level) {
                int quality = JPEG_QUALITIES[level];
                lastQuality = quality;
                jpeg.reset();
                boolean encoded = bitmap.compress(Bitmap.CompressFormat.JPEG, quality, jpeg);
                lastJpegBytes = jpeg.size;
                if (encoded && !jpeg.overflow && jpeg.size >= 12) {
                    // Reuse the last fitting quality rather than spending several
                    // JPEG encodes every frame. Carefully recover quality when the
                    // scene has had a full second of ample payload headroom.
                    jpegQualityIndex = level;
                    qualityRecoveryFrames = Math.min(30, qualityRecoveryFrames + 1);
                    if (level > 0 && qualityRecoveryFrames >= 30 && jpeg.size < MAX_JPEG_BYTES * 3 / 5) {
                        jpegQualityIndex = level - 1;
                        qualityRecoveryFrames = 0;
                    }
                    stage = "jni";
                    ++jniCalls;
                    boolean accepted = MRFPSViewCapture.reportFrame(generation, jpeg.toByteArray(), WIDTH, HEIGHT);
                    ++jniReturns;
                    if (accepted) {
                        ++jniAccepted;
                        lastAcceptedAtMs = SystemClock.elapsedRealtime();
                        recovery.acceptedOutput();
                        surfaceRecoveryDelayMs = SURFACE_STALL_MS;
                        nextSurfaceRecoveryAtMs = lastAcceptedAtMs + SURFACE_STALL_MS;
                        if (jniAccepted == 1) Log.i(TAG, diagnostics("first_native_accept", lastAcceptedAtMs));
                    } else if (!stopRequested && MRFPSViewCapture.isRequested(generation)) {
                        ++nativeRejected;
                        recovery.badOutput(SystemClock.elapsedRealtime());
                    } else ++skippedGeneration;
                    return;
                }
            }
            jpegQualityIndex = JPEG_QUALITIES.length - 1;
            qualityRecoveryFrames = 0;
            ++jpegDrops;
            recovery.badOutput(SystemClock.elapsedRealtime());
        } catch (RuntimeException failure) {
            Log.w(TAG, diagnostics("frame_error", SystemClock.elapsedRealtime()));
            release(4);
        }
    }

    private void checkProgress() {
        long now = SystemClock.elapsedRealtime();
        long lastOutput = lastAcceptedAtMs == 0 ? startedAtMs : lastAcceptedAtMs;
        // OS-authorized projection can legitimately stop producing while unworn
        // or covered by system UI. Keep the same grant/display and poll; native
        // freshness expires independently. Silence alone never revokes consent.
        waitingForOutput = now - lastOutput >= SURFACE_STALL_MS;
        if (stopRequested || !MRFPSViewCapture.isRequested(generation)) { release(0); return; }
        long lastImage = lastImageAtMs == 0 ? startedAtMs : lastImageAtMs;
        if (!contentVisible || display == null) { recovery.suspend(); return; }
        boolean eligible = now >= nextSurfaceRecoveryAtMs;
        MRFPSCaptureRecoveryPolicy.Decision decision = recovery.decide(now, true, eligible);
        if (decision == MRFPSCaptureRecoveryPolicy.Decision.STOP) {
            Log.w(TAG, diagnostics("bad_output_exhausted", now));
            release(4);
            return;
        }
        if (decision != MRFPSCaptureRecoveryPolicy.Decision.REBIND &&
                (now - lastImage < SURFACE_STALL_MS || !eligible)) return;
        // Another system cast can temporarily take compositor output without
        // revoking this projection. Retry after it ends, using bounded backoff
        // and at most two readers during replacement; silence never re-prompts.
        nextSurfaceRecoveryAtMs = now + surfaceRecoveryDelayMs;
        surfaceRecoveryDelayMs = Math.min(30_000L, surfaceRecoveryDelayMs * 2);
        // Reattach a new bounded reader to the SAME already-consented display.
        // No second getMediaProjection/createVirtualDisplay, no resize or grant reuse.
        ImageReader replacement = null;
        try {
            stage = "surface_rebind";
            if (surfaceRebinds < Integer.MAX_VALUE) ++surfaceRebinds;
            replacement = ImageReader.newInstance(WIDTH, HEIGHT, PixelFormat.RGBA_8888, 2);
            if (stopRequested || !MRFPSViewCapture.isRequested(generation)) {
                replacement.close(); replacement = null; release(0); return;
            }
            display.setSurface(replacement.getSurface());
            if (stopRequested || !MRFPSViewCapture.isRequested(generation)) {
                replacement.close(); replacement = null; release(0); return;
            }
            ImageReader previous = reader;
            reader = replacement;
            replacement = null;
            if (previous != null) previous.close();
            if (stopRequested || !MRFPSViewCapture.isRequested(generation)) { release(0); return; }
            lastImageTimestampNs = 0; // A replacement producer may use a new time base.
            Log.i(TAG, diagnostics("surface_rebound", now));
        } catch (RuntimeException failure) {
            if (replacement != null) {
                try { replacement.close(); } catch (RuntimeException ignored) { Log.w(TAG, "MRFPS_CAPTURE replacement_close_error"); }
            }
            Log.w(TAG, diagnostics("surface_rebind_error", now));
            // A platform can reject rebinding while retaining a valid projection.
            // Keep the existing/new selected reader and wait for actual frames;
            // retry with bounded backoff, preserving the user's current consent.
        } finally {
            if (!released) stage = "idle";
            workerAtMs = SystemClock.elapsedRealtime();
        }
    }

    private String diagnostics(String event, long now) {
        return "MRFPS_CAPTURE event=" + event + " generation=" + generation
                + " requested=" + MRFPSViewCapture.isRequested(generation) + " released=" + released
                + " stage=" + stage + " workerAgeMs=" + age(now, workerAtMs)
                + " imageAgeMs=" + age(now, lastImageAtMs) + " acceptedAgeMs=" + age(now, lastAcceptedAtMs)
                + " images=" + acquiredImages + " null=" + nullImages + " invalid=" + invalidImages
                + " duplicate=" + duplicateImages + " jpegDrop=" + jpegDrops
                + " jniEnter=" + jniCalls + " jniReturn=" + jniReturns + " accepted=" + jniAccepted
                + " nativeRejected=" + nativeRejected + " skipped=" + skippedGeneration
                + " badRebinds=" + recovery.badOutputRebinds()
                + " rebinds=" + surfaceRebinds + " visible=" + contentVisible + " waiting=" + waitingForOutput
                + " layout=" + lastWidth + "x" + lastHeight + "/" + lastPlanes
                + " strides=" + lastPixelStride + "/" + lastRowStride
                + " jpegBytes=" + lastJpegBytes + " quality=" + lastQuality;
    }

    private static long age(long now, long then) { return then == 0 ? -1 : now - then; }

    private void finishWorkerGateIfComplete() {
        if (ownsWorkerGate && workerExited && serviceDestroyed && workerGateFinished.compareAndSet(false, true))
            MRFPSCaptureWorkerGate.finish();
    }

    @Override protected void dump(FileDescriptor fd, PrintWriter writer, String[] args) {
        writer.println(diagnostics("dumpsys", SystemClock.elapsedRealtime()));
        writer.println("accepted means native mailbox acceptance, not relay delivery or display; no frame data logged.");
    }

    /** Worker-thread-only, idempotent for callback/onDestroy/error races. */
    private void release(int status) {
        if (released) return;
        released = true;
        stopRequested = true;
        stage = "release";
        Log.i(TAG, diagnostics("release_" + status, SystemClock.elapsedRealtime()));
        // Publish terminal state and request foreground-service shutdown before
        // any driver/Binder cleanup that could itself block indefinitely.
        if (status != 0) MRFPSViewCapture.failSession(generation);
        else MRFPSViewCapture.sessionEnded(generation);
        main.removeCallbacks(watchdog);
        if (worker != null) worker.removeCallbacks(pump);
        final long endedGeneration = generation;
        main.post(() -> {
            if (!released || generation != endedGeneration) return;
            stopForeground(STOP_FOREGROUND_REMOVE);
            // Read the latest startId on the UI thread: a stale intent may have
            // been ignored while this worker was beginning its release.
            stopSelfResult(serviceStartId);
        });
        if (display != null) {
            try { display.release(); } catch (RuntimeException ignored) { Log.w(TAG, "MRFPS_CAPTURE display_release_error"); }
            display = null;
        }
        if (reader != null) {
            try { reader.close(); } catch (RuntimeException ignored) { Log.w(TAG, "MRFPS_CAPTURE reader_close_error"); }
            reader = null;
        }
        if (projection != null) {
            MediaProjection active = projection;
            projection = null;
            if (projectionCallback != null) {
                try { active.unregisterCallback(projectionCallback); } catch (RuntimeException ignored) { Log.w(TAG, "MRFPS_CAPTURE callback_release_error"); }
            }
            try { active.stop(); } catch (RuntimeException ignored) { Log.w(TAG, "MRFPS_CAPTURE projection_release_error"); }
        }
        if (bitmap != null) {
            try { bitmap.recycle(); } catch (RuntimeException ignored) { Log.w(TAG, "MRFPS_CAPTURE bitmap_release_error"); }
            bitmap = null;
        }
        pixels = null;
        jpeg = null;
        if (thread != null) thread.quitSafely();
        // Cleanup/stop is already scheduled even if a native mutex stalls this.
        MRFPSViewCapture.reportState(generation, status);
    }

    @Override public void onDestroy() {
        stopRequested = true;
        if (!released) MRFPSViewCapture.failSession(generation);
        serviceDestroyed = true;
        finishWorkerGateIfComplete();
        main.removeCallbacks(watchdog);
        // Cleanup is posted to the owning worker. If a platform call never
        // returns, the worker lease stays occupied and new captures fail closed.
        if (worker != null) worker.post(() -> release(0));
        else {
            MRFPSViewCapture.sessionEnded(generation);
            MRFPSViewCapture.reportState(generation, 0);
            if (thread != null) thread.quitSafely();
        }
        super.onDestroy();
    }

    /** Sink stays at most 60 KiB even if a pathological image exceeds the cap. */
    private static final class BoundedJpegOutput extends OutputStream {
        private final byte[] bytes;
        int size;
        boolean overflow;
        BoundedJpegOutput(int capacity) { bytes = new byte[capacity]; }
        void reset() { size = 0; overflow = false; }
        @Override public void write(int value) {
            if (size == bytes.length) { overflow = true; return; }
            bytes[size++] = (byte) value;
        }
        @Override public void write(byte[] buffer, int offset, int length) {
            if (length > bytes.length - size) { overflow = true; return; }
            System.arraycopy(buffer, offset, bytes, size, length);
            size += length;
        }
        byte[] toByteArray() { return Arrays.copyOf(bytes, size); }
    }
}
