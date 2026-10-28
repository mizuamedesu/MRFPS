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
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;

/** GameActivity bridge. No consent Intent or captured frame is stored on disk. */
public final class MRFPSViewCapture {
    private static final String TAG = "MRFPSViewCapture";
    private static final int REQUEST_CODE = 58481;
    private static final int MAX_JPEG_BYTES = 60 * 1024;
    private static final AtomicLong desiredGeneration = new AtomicLong(0);
    // Accessed exclusively on the Activity UI thread. Keep a cancelled request
    // pending until its result arrives, so it cannot be mistaken for a new grant.
    private static volatile long pendingGeneration = 0;

    private MRFPSViewCapture() {}

    /** Read by the native game thread; the OS result remains the only writer of the grant. */
    public static boolean hasPendingConsent() { return pendingGeneration != 0; }

    private static native void nativeOnState(long generation, int state);
    private static native void nativeOnFrame(long generation, byte[] jpeg,
                                             int width, int height, long sourceUtcNanoseconds);

    static boolean isRequested(long generation) {
        return generation != 0 && desiredGeneration.get() == generation;
    }

    static void reportState(long generation, int state) {
        Log.i(TAG, "MRFPS_CAPTURE_STATE generation=" + generation + " state=" + state
                + " requested=" + isRequested(generation));
        nativeOnState(generation, state);
    }

    // True means JNI returned, not that the native mailbox/relay accepted it.
    static boolean reportFrame(long generation, byte[] jpeg, int width, int height) {
        if (isRequested(generation)) {
            nativeOnFrame(generation, jpeg, width, height, 0);
            return true;
        }
        return false;
    }

    static void sessionEnded(long generation) {
        desiredGeneration.compareAndSet(generation, 0);
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
            final MediaProjectionManager manager = (MediaProjectionManager)
                    activity.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            if (manager == null) {
                reportState(generation, 4);
                return;
            }
            desiredGeneration.set(generation);
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
            Log.i(TAG, "MRFPS_CAPTURE_STOP desired=" + desiredGeneration.get());
            desiredGeneration.set(0);
            activity.stopService(new Intent(activity, MRFPSViewCaptureService.class));
            // Do not reset pendingGeneration; its OS activity result must drain.
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
