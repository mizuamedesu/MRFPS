#!/usr/bin/env python3
"""Build a local Quest adapter overlay from the pinned private MoBits SDK.

Only the Java/Kotlin bridge is rebuilt. The vendor AAR's native library,
manifest, and consumer rules remain byte-for-byte unchanged.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from ensure_sdk import AAR_SHA256, CONNECT_REVISION, ROOT, sha256

FILES = (
    "BondRemoval.java", "GattConnectionSetup.java", "GattOperation.java",
    "MoBitsAndroid.kt", "MoBitsHapticProfile.java", "ScanSession.java",
)
QUEST_JAVA_SHA256 = "120b46332f8e7495c36f9c7c16c02c44fa4f8189920caef6cd305dd529c94fc6"


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError("Unexpected MoBits Android source at " + old.splitlines()[0][:80])
    return text.replace(old, new, 1)


def patch_bridge(text: str) -> str:
    start = '                "connect" -> {'
    end = '                "disconnect" ->'
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError("Unexpected MoBits connection dispatch")
    before, rest = text.split(start, 1)
    _, after = rest.split(end, 1)
    text = before + '''                "connect" -> {
                    if (!hasPermissions()) return null
                    peripherals.remove(address)?.close()
                    var connected = false
                    for (attempt in 0..1) {
                        val candidate = Peripheral(address, attempt == 1)
                        peripherals[address] = candidate
                        if (candidate.connect()) { connected = true; break }
                        val bonding = adapter?.getRemoteDevice(address)?.bondState == BluetoothDevice.BOND_BONDING
                        val retry = attempt == 0 && candidate.retryableDiscovery && !bonding
                        peripherals.remove(address, candidate)
                        candidate.close()
                        if (!retry) break
                        Log.w(TAG, "$address service discovery did not finish; refreshing this peer and reconnecting once")
                        try { Thread.sleep(750) } catch (interrupted: InterruptedException) {
                            Thread.currentThread().interrupt(); return null
                        }
                    }
                    if (connected) byteArrayOf(1) else null
                }
''' + end + after
    text = replace_once(text,
        "private inner class Peripheral(private val address: String) {",
        "private inner class Peripheral(private val address: String, private val recoverCache: Boolean = false) {")
    text = replace_once(text, "        @Volatile var mtu = 23\n", '''        @Volatile var mtu = 23
        @Volatile var retryableDiscovery = false
        @Volatile private var linkUp = false
        @Volatile private var servicesObserved = false
''')
    text = replace_once(text,
        '                if (state == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {\n',
        '                if (state == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {\n                    linkUp = true\n')
    text = replace_once(text,
        '                } else if (state == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {\n',
        '                } else if (state == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {\n                    linkUp = false\n')
    text = replace_once(text,
        '            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {\n',
        '            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {\n                servicesObserved = true\n')
    text = replace_once(text,
        '            // Profile v2 info is 24 bytes and tracker notifications can be 172 bytes.\n',
        '''            if (recoverCache) {
                // The prior GATT object has been closed; clear only this peer's
                // stale database before discovering on the new connection.
                if (refreshEmptyServices(g)) {
                    try { Thread.sleep(350) } catch (interrupted: InterruptedException) {
                        Thread.currentThread().interrupt(); return false
                    }
                }
            }
            // Profile v2 info is 24 bytes and tracker notifications can be 172 bytes.
''')
    text = replace_once(text,
        '                Log.w(TAG, "$address service/MTU setup failed; mtu=$mtu")\n',
        '''                retryableDiscovery = linkUp && !servicesObserved
                Log.w(TAG, "$address service/MTU setup failed; mtu=$mtu serviceCallback=$servicesObserved")
''')
    return text


def source_archive(cache: Path) -> dict[str, bytes]:
    prefix = "android/src/main/java/org/mobits/connect/"
    proc = subprocess.run(["git", "-C", str(cache), "archive", "--format=tar", CONNECT_REVISION,
                           *(prefix + name for name in FILES)], capture_output=True, check=True)
    result: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(proc.stdout), mode="r:") as archive:
        for entry in archive:
            name = PurePosixPath(entry.name)
            if entry.isdir():
                continue
            if not entry.isfile() or name.as_posix() not in {prefix + item for item in FILES}:
                raise RuntimeError("Unexpected source archive entry")
            stream = archive.extractfile(entry)
            if stream is None:
                raise RuntimeError("Missing Android bridge source")
            result[name.name] = stream.read()
    if set(result) != set(FILES):
        raise RuntimeError("Pinned Android bridge sources are incomplete")
    return result


def gradle_command(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).resolve(strict=True)
        if path.is_file():
            return path
        raise RuntimeError("Specified Gradle command is not a file")
    candidates = sorted((ROOT / ".local/mobits-build/gradle-android/wrapper/dists/gradle-8.14.3-all").glob(
        "*/gradle-8.14.3/bin/gradle.bat" if os.name == "nt" else "*/gradle-8.14.3/bin/gradle"))
    if candidates:
        return candidates[0]
    found = shutil.which("gradle.bat" if os.name == "nt" else "gradle")
    if found:
        return Path(found).resolve()
    raise RuntimeError("Gradle 8.14.3 is required for the Quest bridge; set MRFPS_MOBITS_GRADLE")


def run(args: argparse.Namespace) -> dict:
    plugin = ROOT / ".local/mobits-sdk/Plugins/MoBitsSDK"
    vendor_aar = plugin / "Source/Standalone/Libs/mobits-quest-standalone.aar"
    output_aar = plugin / "Source/Standalone/Libs/mobits-quest-standalone-mrfps.aar"
    marker = output_aar.with_suffix(".json")
    if sha256(vendor_aar) != AAR_SHA256:
        raise RuntimeError("Vendor Android library hash mismatch")
    quest_source = plugin / "Source/Standalone/Android/src/main/java/org/mobits/quest/QuestStandalone.java"
    provenance = json.loads((plugin / "Source/Standalone/Libs/provenance.json").read_text(encoding="utf-8"))
    # git archive may apply Windows checkout line endings; provenance uses LF.
    quest_digest = hashlib.sha256(quest_source.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    if quest_digest != QUEST_JAVA_SHA256 or provenance.get("sources", {}).get("Android/src/main/java/org/mobits/quest/QuestStandalone.java") != QUEST_JAVA_SHA256:
        raise RuntimeError("MoBits Quest bridge source hash mismatch")
    cache = ROOT / ".local/mobits-connect-source"
    if not (cache / ".git").exists():
        raise RuntimeError("Pinned MoBits Connect source cache is missing")
    identity = hashlib.sha256(Path(__file__).read_bytes() + AAR_SHA256.encode() + CONNECT_REVISION.encode() + quest_digest.encode()).hexdigest()
    if output_aar.exists():
        if not marker.is_file():
            raise RuntimeError("Existing recovery AAR is unmanaged; it was not overwritten")
        previous = json.loads(marker.read_text(encoding="utf-8"))
        if previous.get("sha256") != sha256(output_aar):
            raise RuntimeError("Existing recovery AAR was modified; it was not overwritten")
        if previous.get("identity") == identity:
            return {"status": "ready", "sha256": previous["sha256"]}
    sources = source_archive(cache)
    root = (ROOT / ".local/mobits-quest-bridge-build").resolve()
    if not root.is_relative_to(ROOT.resolve()):
        raise RuntimeError("Bridge staging path is outside the workspace")
    root.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="bridge-", dir=root))
    connect = stage / "src/main/java/org/mobits/connect"
    quest = stage / "src/main/java/org/mobits/quest"
    connect.mkdir(parents=True); quest.mkdir(parents=True)
    for name, body in sources.items():
        if name == "MoBitsAndroid.kt":
            body = patch_bridge(body.decode("utf-8").replace("\r\n", "\n")).encode("utf-8")
        (connect / name).write_bytes(body)
    shutil.copyfile(quest_source, quest / "QuestStandalone.java")
    (stage / "src/main/AndroidManifest.xml").write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android"/>\n', encoding="utf-8")
    (stage / "settings.gradle").write_text('pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }\ndependencyResolutionManagement { repositories { google(); mavenCentral() } }\nrootProject.name = "mrfps-mobits-bridge"\n', encoding="utf-8")
    (stage / "build.gradle").write_text('''plugins {
    id 'com.android.library' version '8.13.0'
    id 'org.jetbrains.kotlin.android' version '2.2.20'
}
android {
    namespace 'org.mobits.quest'
    compileSdk 35
    defaultConfig { minSdk 26 }
    compileOptions { sourceCompatibility JavaVersion.VERSION_1_8; targetCompatibility JavaVersion.VERSION_1_8 }
    kotlinOptions { jvmTarget = '1.8' }
}
''', encoding="utf-8")
    env = os.environ.copy()
    env["JAVA_HOME"] = str(Path(args.java_home).resolve(strict=True))
    env["ANDROID_HOME"] = env["ANDROID_SDK_ROOT"] = str(Path(args.android_home).resolve(strict=True))
    env["GRADLE_USER_HOME"] = str(ROOT / ".local/mobits-build/gradle-android")
    gradle = gradle_command(args.gradle or env.get("MRFPS_MOBITS_GRADLE"))
    command = [str(gradle), "--no-daemon", "--max-workers=1", "-Dorg.gradle.jvmargs=-Xmx1024m -Dfile.encoding=UTF-8", "assembleRelease"]
    result = subprocess.run(command, cwd=stage, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    (stage / "build.log").write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError("Quest BLE bridge build failed; see " + str(stage / "build.log"))
    rebuilt = stage / "build/outputs/aar/mrfps-mobits-bridge-release.aar"
    with zipfile.ZipFile(rebuilt) as archive:
        classes = archive.read("classes.jar")
    with zipfile.ZipFile(vendor_aar) as original:
        if original.namelist().count("classes.jar") != 1:
            raise RuntimeError("Vendor AAR has an unexpected class archive")
        with zipfile.ZipFile(io.BytesIO(original.read("classes.jar"))) as old_classes, zipfile.ZipFile(io.BytesIO(classes)) as new_classes:
            old_names = {name for name in old_classes.namelist() if name.endswith(".class")}
            new_names = {name for name in new_classes.namelist() if name.endswith(".class")}
            if old_names != new_names:
                raise RuntimeError("Rebuilt Android bridge class set differs from the pinned vendor AAR")
        with tempfile.NamedTemporaryFile(prefix=".mrfps-ble-", suffix=".aar", dir=output_aar.parent, delete=False) as temp:
            temp_path = Path(temp.name)
        try:
            with zipfile.ZipFile(temp_path, "w") as output:
                for entry in original.infolist():
                    output.writestr(entry, classes if entry.filename == "classes.jar" else original.read(entry.filename))
            digest = sha256(temp_path)
            os.replace(temp_path, output_aar)
            marker.write_text(json.dumps({"identity": identity, "sha256": digest, "vendor_sha256": AAR_SHA256}) + "\n", encoding="utf-8")
        finally:
            if temp_path.exists():
                temp_path.unlink()
    return {"status": "built", "sha256": digest, "log": str(stage / "build.log")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android-home", required=True)
    parser.add_argument("--java-home", required=True)
    parser.add_argument("--gradle")
    try:
        print(json.dumps(run(parser.parse_args())))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as exc:
        print("Quest BLE recovery build failed: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
