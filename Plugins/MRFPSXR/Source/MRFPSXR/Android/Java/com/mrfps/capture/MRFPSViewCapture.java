package com.mrfps.capture;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionConfig;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.util.Log;

import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** GameActivity bridge. No consent Intent or captured frame is stored on disk. */
public final class MRFPSViewCapture {
    private static final String TAG = "MRFPSViewCapture";
    private static final int REQUEST_CODE = 58481;
    private static final int MAX_JPEG_BYTES = 60 * 1024;
    private static final AtomicLong desiredGeneration = new AtomicLong(0);
    private static final AtomicLong failedGeneration = new AtomicLong(0);
    private static final class Owner {
        final long generation;
        final WeakReference<Activity> activity;
        Owner(long generation, Activity activity) {
            this.generation = generation;
            this.activity = new WeakReference<>(activity);
        }
    }
    private static final AtomicReference<Owner> owner = new AtomicReference<>();
    // Accessed exclusively on the Activity UI thread. Keep a cancelled request
    // pending until its result arrives, so it cannot be mistaken for a new grant.
    private static volatile long pendingGeneration = 0;

    private MRFPSViewCapture() {}

    /** Read by the native game thread; the OS result remains the only writer of the grant. */
    public static boolean hasPendingConsent() { return pendingGeneration != 0; }

    /** Written only for an actual failed generation, never for a request not yet run on the UI thread. */
    public static boolean hasCaptureFailure(long generation) {
        return generation > 0 && failedGeneration.get() == generation;
    }

    private static native void nativeOnState(long generation, int state);
    private static native boolean nativeOnFrame(long generation, byte[] jpeg,
                                                int width, int height, long sourceUtcNanoseconds);

    static boolean isRequested(long generation) {
        return generation != 0 && desiredGeneration.get() == generation;
    }

    static void reportState(long generation, int state) {
        Log.i(TAG, "MRFPS_CAPTURE_STATE generation=" + generation + " state=" + state
                + " requested=" + isRequested(generation));
        nativeOnState(generation, state);
    }

    // True means the current native mailbox accepted the frame. It does not
    // assert relay delivery or that an inspector displayed the frame.
    static boolean reportFrame(long generation, byte[] jpeg, int width, int height) {
        return isRequested(generation) && nativeOnFrame(generation, jpeg, width, height, 0);
    }

    static void sessionEnded(long generation) {
        if (desiredGeneration.compareAndSet(generation, 0)) clearOwner(generation);
    }

    private static void clearOwner(long generation) {
        Owner current = owner.get();
        if (current != null && current.generation == generation) owner.compareAndSet(current, null);
    }

    // Called from the main watchdog or worker before any potentially blocking
    // cleanup. A delayed old failure cannot overwrite a newer failed session.
    static void failSession(long generation) {
        if (!desiredGeneration.compareAndSet(generation, 0)) return;
        clearOwner(generation);
        long previous;
        do {
            previous = failedGeneration.get();
            if (generation <= previous) return;
        } while (!failedGeneration.compareAndSet(previous, generation));
    }

    /** Called by JNI on the game thread; all consent/UI work moves to UI thread. */
    public static void request(final Activity activity, final long generation) {
        activity.runOnUiThread(() -> {
            Log.i(TAG, "MRFPS_CAPTURE_REQUEST generation=" + generation
                    + " pending=" + pendingGeneration + " desired=" + desiredGeneration.get());
            if (generation <= 0 || activity.isFinishing() || activity.isDestroyed()) {
                reportState(generation, 4);
                return;
            }
            if (pendingGeneration != 0) {
                reportState(generation, 6);
                return;
            }
            if (desiredGeneration.get() != 0) {
                reportState(generation, 4);
                return;
            }
            if (MRFPSCaptureWorkerGate.isBusy()) {
                // Do not consume a new one-use consent while an older worker is
                // still unwinding or permanently blocked in platform/JNI code.
                reportState(generation, 4);
                return;
            }
            final MediaProjectionManager manager = (MediaProjectionManager)
                    activity.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            if (manager == null) {
                reportState(generation, 4);
                return;
            }
            desiredGeneration.set(generation);
            owner.set(new Owner(generation, activity));
            pendingGeneration = generation;
            try {
                // Request the default display for the actual headset composite.
                // The OS still displays its consent UI and retains final control.
                Intent consent = Build.VERSION.SDK_INT >= 34
                        ? manager.createScreenCaptureIntent(MediaProjectionConfig.createConfigForDefaultDisplay())
                        : manager.createScreenCaptureIntent();
                activity.startActivityForResult(consent, REQUEST_CODE);
            } catch (RuntimeException failure) {
                pendingGeneration = 0;
                sessionEnded(generation);
                reportState(generation, 4);
            }
        });
    }

    /** Injected by UPL into GameActivity.onActivityResult. No automatic grant. */
    public static void onActivityResult(Activity activity, int requestCode,
                                        int resultCode, Intent data) {
        if (requestCode != REQUEST_CODE) return;
        final long generation = pendingGeneration;
        pendingGeneration = 0;
        Log.i(TAG, "MRFPS_CAPTURE_CONSENT_RESULT generation=" + generation
                + " requested=" + isRequested(generation)
                + " granted=" + (resultCode == Activity.RESULT_OK && data != null));
        if (!isRequested(generation)) return; // Cancelled result: do not consume token.
        if (resultCode != Activity.RESULT_OK || data == null) {
            sessionEnded(generation);
            reportState(generation, 3);
            return;
        }
        Intent service = new Intent(activity, MRFPSViewCaptureService.class)
                .setAction(MRFPSViewCaptureService.ACTION_START)
                .putExtra(MRFPSViewCaptureService.EXTRA_GENERATION, generation)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_CODE, resultCode)
                .putExtra(MRFPSViewCaptureService.EXTRA_RESULT_DATA, data);
        try {
            // Service calls startForeground before getMediaProjection on API 34.
            if (Build.VERSION.SDK_INT >= 26) activity.startForegroundService(service);
            else activity.startService(service);
        } catch (RuntimeException failure) {
            sessionEnded(generation);
            reportState(generation, 4);
        }
        // The one-use consent Intent lives only in this service start request.
    }

    public static void stop(final Activity activity) {
        activity.runOnUiThread(() -> {
            Owner current = owner.get();
            if (current == null || current.activity.get() != activity) return;
            Log.i(TAG, "MRFPS_CAPTURE_STOP desired=" + current.generation);
            if (desiredGeneration.compareAndSet(current.generation, 0))
                activity.stopService(new Intent(activity, MRFPSViewCaptureService.class));
            clearOwner(current.generation);
            // Do not reset pendingGeneration; its OS activity result must drain.
        });
    }

    /** A delayed stop for an old generation must never terminate a newer one. */
    public static void stopForGeneration(final Activity activity, final long generation) {
        activity.runOnUiThread(() -> {
            if (generation > 0 && desiredGeneration.compareAndSet(generation, 0)) {
                activity.stopService(new Intent(activity, MRFPSViewCaptureService.class));
                clearOwner(generation);
            }
        });
    }

    /** Explicit -MRFPSVideoFromAdb only. No external/storage fallback exists. */
    public static void pollDebug(Activity activity, long generation) {
        File frame = new File(activity.getFilesDir(), "MRFPS/Video/frame.bin");
        final long fileSize = frame.length();
        if (fileSize < 24 + 12 || fileSize > 24 + MAX_JPEG_BYTES) return;
        try (DataInputStream input = new DataInputStream(new FileInputStream(frame))) {
            byte[] header = new byte[24];
            input.readFully(header);
            if (header[0] != 'M' || header[1] != 'R' || header[2] != 'V' || header[3] != '1') return;
            ByteBuffer fields = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
            fields.position(4);
            int width = fields.getInt();
            int height = fields.getInt();
            int length = fields.getInt();
            long sequenceUtcNanoseconds = fields.getLong();
            if (width <= 0 || width > 1280 || height <= 0 || height > 720 ||
                    length < 12 || length > MAX_JPEG_BYTES || sequenceUtcNanoseconds <= 0 ||
                    fileSize != 24L + length) return;
            byte[] jpeg = new byte[length];
            input.readFully(jpeg);
            if (input.read() != -1) return;
            // Native verifies UTC freshness, sequence, SOI/EOI and encoded dimensions.
            nativeOnFrame(generation, jpeg, width, height, sequenceUtcNanoseconds);
        } catch (IOException | SecurityException ignored) {
            // Missing/partial files are not frames. Status remains waiting/stale.
        }
    }
}
