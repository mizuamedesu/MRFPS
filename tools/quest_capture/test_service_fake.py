#!/usr/bin/env python3
"""Exercise the real capture service using small JDK-only Android fakes.

The fakes are deliberately narrow: they do not claim Android or headset
behavior, and they replace the native bridge with an explicit accept/reject.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_recovery import tool


FAKES = {
    "android/R.java": """package android; public final class R { public static final class drawable { public static final int ic_menu_camera=1, ic_media_pause=2; } }""",
    "android/util/Log.java": """package android.util; public final class Log { public static int i(String a,String b){return 0;} public static int w(String a,String b){return 0;} }""",
    "android/os/Build.java": """package android.os; public final class Build { public static final class VERSION { public static int SDK_INT=34; } }""",
    "android/os/IBinder.java": """package android.os; public interface IBinder {}""",
    "android/os/Looper.java": """package android.os; public class Looper { public static Looper getMainLooper(){return new Looper();} }""",
    "android/os/Handler.java": """package android.os; public class Handler { public static volatile int posts; public static volatile Runnable lastPosted; public Handler(){} public Handler(Looper x){} public boolean post(Runnable r){++posts;lastPosted=r;return true;} public boolean postDelayed(Runnable r,long m){return true;} public void removeCallbacks(Runnable r){} }""",
    "android/os/HandlerThread.java": """package android.os; public class HandlerThread extends Thread { public HandlerThread(String n){super(n);} @Override public synchronized void start(){} @Override public void run(){} public Looper getLooper(){return new Looper();} public boolean quitSafely(){return true;} }""",
    "android/os/SystemClock.java": """package android.os; public final class SystemClock { public static volatile long now; public static long elapsedRealtime(){return now;} public static long elapsedRealtimeNanos(){return now*1000000;} }""",
    "android/net/Uri.java": """package android.net; public final class Uri { private final String value; private Uri(String v){value=v;} public static Uri parse(String v){return new Uri(v);} @Override public String toString(){return value;} @Override public boolean equals(Object o){return o instanceof Uri && value.equals(((Uri)o).value);} @Override public int hashCode(){return value.hashCode();} }""",
    "android/content/res/Configuration.java": """package android.content.res; public class Configuration { public int densityDpi=320; }""",
    "android/content/res/Resources.java": """package android.content.res; public class Resources { public Configuration getConfiguration(){return new Configuration();} }""",
    "android/content/Context.java": """package android.content; public class Context { public static final String MEDIA_PROJECTION_SERVICE="projection", NOTIFICATION_SERVICE="notification"; public Object getSystemService(String n){return n.equals(MEDIA_PROJECTION_SERVICE) ? new android.media.projection.MediaProjectionManager() : new android.app.NotificationManager();} public android.content.res.Resources getResources(){return new android.content.res.Resources();} }""",
    "android/content/Intent.java": """package android.content; public class Intent { private String action; private Class<?> target; private android.net.Uri data; private final java.util.Map<String,Object> values=new java.util.HashMap<>(); public Intent(){} public Intent(Context c,Class<?> k){target=k;} public Intent setAction(String a){action=a;return this;} public Intent setData(android.net.Uri u){data=u;return this;} public android.net.Uri getData(){return data;} public Class<?> getTarget(){return target;} public Intent copy(){Intent i=new Intent();i.action=action;i.target=target;i.data=data;i.values.putAll(values);return i;} public Intent putExtra(String k,Object v){values.put(k,v);return this;} public String getAction(){return action;} public long getLongExtra(String n,long d){Object v=values.get(n);return v instanceof Number?((Number)v).longValue():d;} public int getIntExtra(String n,int d){Object v=values.get(n);return v instanceof Number?((Number)v).intValue():d;} @SuppressWarnings("unchecked") public <T> T getParcelableExtra(String n){return (T)values.get(n);} public <T> T getParcelableExtra(String n,Class<T> c){return c.cast(values.get(n));} public void removeExtra(String n){values.remove(n);} }""",
    "android/content/pm/ServiceInfo.java": """package android.content.pm; public final class ServiceInfo { public static final int FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION=1; }""",
    "android/app/Activity.java": """package android.app; public class Activity extends android.content.Context { public static final int RESULT_OK=-1, RESULT_CANCELED=0; public boolean queueUi; public Runnable queued; public int stops; public boolean isFinishing(){return false;} public boolean isDestroyed(){return false;} public void runOnUiThread(Runnable r){if(queueUi) queued=r; else r.run();} public void startActivityForResult(android.content.Intent i,int code){} public void startForegroundService(android.content.Intent i){} public void startService(android.content.Intent i){} public boolean stopService(android.content.Intent i){++stops;return true;} public java.io.File getFilesDir(){return new java.io.File(".");} }""",
    "android/app/Service.java": """package android.app; public class Service extends android.content.Context { public static final int START_NOT_STICKY=2,STOP_FOREGROUND_REMOVE=1; public boolean stopped; public int lastStopId=-1; public android.os.IBinder onBind(android.content.Intent i){return null;} public int onStartCommand(android.content.Intent i,int f,int id){return 0;} public void onDestroy(){} protected void dump(java.io.FileDescriptor f,java.io.PrintWriter w,String[] a){} public void startForeground(int id,Notification n,int type){} public void startForeground(int id,Notification n){} public void stopForeground(int flags){} public void stopSelf(){stopped=true;} public void stopSelf(int id){stopped=true;lastStopId=id;} public boolean stopSelfResult(int id){stopped=true;lastStopId=id;return true;} }""",
    "android/app/Notification.java": """package android.app; public class Notification { public static final String CATEGORY_SERVICE="service"; public static final int VISIBILITY_PRIVATE=0; public static class Action { public static class Builder { public Builder(android.graphics.drawable.Icon i,String s,PendingIntent p){} public Action build(){return new Action();} } } public static class Builder { public Builder(android.content.Context c,String s){} public Builder setSmallIcon(int x){return this;} public Builder setContentTitle(String x){return this;} public Builder setContentText(String x){return this;} public Builder setOngoing(boolean x){return this;} public Builder setCategory(String x){return this;} public Builder setVisibility(int x){return this;} public Builder addAction(Action x){return this;} public Notification build(){return new Notification();} } }""",
    "android/app/NotificationChannel.java": """package android.app; public class NotificationChannel { public NotificationChannel(String a,String b,int c){} }""",
    "android/app/NotificationManager.java": """package android.app; public class NotificationManager { public static final int IMPORTANCE_LOW=1; public void createNotificationChannel(NotificationChannel c){} }""",
    "android/app/PendingIntent.java": """package android.app; public class PendingIntent { public static final int FLAG_UPDATE_CURRENT=1,FLAG_IMMUTABLE=2; private static final java.util.Map<String,PendingIntent> TOKENS=new java.util.HashMap<>(); public static PendingIntent lastCreated; public android.content.Intent intent; public static synchronized PendingIntent getService(android.content.Context c,int id,android.content.Intent i,int f){String key=id+"|"+i.getTarget()+"|"+i.getAction()+"|"+i.getData();PendingIntent p=TOKENS.get(key);if(p==null){p=new PendingIntent();TOKENS.put(key,p);}if(p.intent==null||(f&FLAG_UPDATE_CURRENT)!=0)p.intent=i.copy();lastCreated=p;return p;} }""",
    "android/graphics/drawable/Icon.java": """package android.graphics.drawable; public class Icon { public static Icon createWithResource(android.content.Context c,int id){return new Icon();} }""",
    "android/graphics/PixelFormat.java": """package android.graphics; public final class PixelFormat { public static final int RGBA_8888=1; }""",
    "android/graphics/Bitmap.java": """package android.graphics; public class Bitmap { public static boolean oversize; public enum Config { ARGB_8888 } public enum CompressFormat { JPEG } public static Bitmap createBitmap(int w,int h,Config c){return new Bitmap();} public void setPixels(int[] p,int o,int s,int x,int y,int w,int h){} public boolean compress(CompressFormat f,int q,java.io.OutputStream out){try {out.write(new byte[oversize ? 50000 : 12]);return true;} catch(java.io.IOException e){return false;}} public void recycle(){} }""",
    "android/hardware/display/DisplayManager.java": """package android.hardware.display; public final class DisplayManager { public static final int VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR=1; }""",
    "android/hardware/display/VirtualDisplay.java": """package android.hardware.display; public class VirtualDisplay { public static Runnable afterSetSurface,afterRelease; public void setSurface(Object surface){if(afterSetSurface!=null) afterSetSurface.run();} public void release(){if(afterRelease!=null) afterRelease.run();} public static class Callback { public void onPaused(){} public void onResumed(){} public void onStopped(){} } }""",
    "android/media/Image.java": """package android.media; public class Image implements AutoCloseable { private static long clock; private static final java.nio.ByteBuffer BUFFER=java.nio.ByteBuffer.allocate(640*360*4); private final boolean valid; private Image(boolean v){valid=v;} public static Image valid(){return new Image(true);} public static Image invalid(){return new Image(false);} public long getTimestamp(){return ++clock;} public int getWidth(){return valid?640:1;} public int getHeight(){return 360;} public Plane[] getPlanes(){return new Plane[]{new Plane()};} public void close(){} public static class Plane { public java.nio.ByteBuffer getBuffer(){return BUFFER;} public int getPixelStride(){return 4;} public int getRowStride(){return 640*4;} } }""",
    "android/media/ImageReader.java": """package android.media; public class ImageReader { public static Image image; public static ImageReader newInstance(int w,int h,int f,int n){return new ImageReader();} public Object getSurface(){return new Object();} public Image acquireLatestImage(){return image;} public void close(){} }""",
    "android/media/projection/MediaProjection.java": """package android.media.projection; public class MediaProjection { public static volatile int stops; public static class Callback { public void onStop(){} public void onCapturedContentVisibilityChanged(boolean v){} } public void registerCallback(Callback c,android.os.Handler h){} public void unregisterCallback(Callback c){} public void stop(){++stops;} public android.hardware.display.VirtualDisplay createVirtualDisplay(String n,int w,int h,int d,int f,Object s,android.hardware.display.VirtualDisplay.Callback c,android.os.Handler handler){return new android.hardware.display.VirtualDisplay();} }""",
    "android/media/projection/MediaProjectionConfig.java": """package android.media.projection; public class MediaProjectionConfig { public static MediaProjectionConfig createConfigForDefaultDisplay(){return new MediaProjectionConfig();} }""",
    "android/media/projection/MediaProjectionManager.java": """package android.media.projection; public class MediaProjectionManager { public MediaProjection getMediaProjection(int code,android.content.Intent data){return new MediaProjection();} public android.content.Intent createScreenCaptureIntent(){return new android.content.Intent();} public android.content.Intent createScreenCaptureIntent(MediaProjectionConfig config){return new android.content.Intent();} }""",
    "com/mrfps/capture/MRFPSViewCapture.java": """package com.mrfps.capture; public final class MRFPSViewCapture { public static volatile boolean requested,accepted; public static volatile int lastState,frames,ended; public static volatile long failed,desired=1; public static volatile Runnable stateHook; public static boolean isRequested(long g){return g==desired && requested;} public static void sessionEnded(long g){if(isRequested(g)){++ended;requested=false;}} public static void failSession(long g){if(isRequested(g)){failed=g;sessionEnded(g);}} public static void reportState(long g,int s){if(stateHook!=null)stateHook.run();lastState=s;} public static boolean reportFrame(long g,byte[] b,int w,int h){++frames;return isRequested(g)&&accepted;} }""",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java")
    parser.add_argument("--javac")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = root / "Plugins/MRFPSXR/Source/MRFPSXR/Android/Java/com/mrfps/capture"
    harness = Path(__file__).with_name("MRFPSCaptureServiceFakeTest.java")
    with tempfile.TemporaryDirectory(prefix="mrfps-capture-fakes-") as temp:
        directory = Path(temp)
        platform_paths = []
        fake_bridge = None
        for name, content in FAKES.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            if name.startswith("com/"):
                fake_bridge = str(path)
            else:
                platform_paths.append(str(path))
        actual = directory / "actual"
        fake = directory / "fake"
        actual.mkdir()
        fake.mkdir()
        javac = tool("javac", args.javac)
        common = [str(source / "MRFPSCaptureRecoveryPolicy.java"), str(source / "MRFPSCaptureWorkerGate.java"),
                  str(source / "MRFPSViewCaptureService.java")]
        # Compile the actual Java-to-JNI signature as well, without invoking JNI.
        subprocess.run([javac, "-J-Xmx64m", "-encoding", "UTF-8", "-h", str(actual), "-d", str(actual),
                        *platform_paths, *common, str(source / "MRFPSViewCapture.java"),
                        str(Path(__file__).with_name("MRFPSCaptureBridgeFakeTest.java"))], check=True, timeout=30)
        generated = (actual / "com_mrfps_capture_MRFPSViewCapture.h").read_text(encoding="utf-8")
        native = (root / "Plugins/MRFPSXR/Source/MRFPSXR/Private/MRFPSViewCaptureSubsystem.cpp").read_text(encoding="utf-8")
        signature = r"JNIEXPORT\s+jboolean\s+JNICALL\s+Java_com_mrfps_capture_MRFPSViewCapture_nativeOnFrame\s*\(([^)]*)\)"
        expected_types = ["JNIEnv", "jclass", "jlong", "jbyteArray", "jint", "jint", "jlong"]
        for label, text in (("javac header", generated), ("native C++", native)):
            match = re.search(signature, text, re.DOTALL)
            types = re.findall(r"\b(?:JNIEnv|jclass|jlong|jbyteArray|jint)\b", match.group(1)) if match else []
            if types != expected_types:
                raise SystemExit(f"{label}: JNI frame callback does not match {expected_types}")
        print("PASS generated JNI frame callback signature matches C++", flush=True)
        subprocess.run([tool("java", args.java), "-Xms8m", "-Xmx32m", "-cp", str(actual),
                        "com.mrfps.capture.MRFPSCaptureBridgeFakeTest"], check=True, timeout=30)
        subprocess.run([javac, "-J-Xmx64m", "-encoding", "UTF-8", "-d", str(fake),
                        *platform_paths, *common, fake_bridge, str(harness)], check=True, timeout=30)
        subprocess.run([tool("java", args.java), "-Xms8m", "-Xmx32m", "-cp", str(fake),
                        "com.mrfps.capture.MRFPSCaptureServiceFakeTest"], check=True, timeout=30)


if __name__ == "__main__":
    main()
