#!/usr/bin/env python3
"""Install the pinned, access-controlled MoBits SDK into the local workspace.

The upstream SDK is private; its source and Android library stay under .local.
The checkout is reproducible and never overwrites an unowned installation.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = "2aba57a51923fb2252347407f023db8457e05804"
ORIGIN = "https://github.com/MoBits-Project/UE5_SDK.git"
AAR_SHA256 = "b877e2ecbea3538e6065ec2d5056186191ab994b273bd0ae3ab98c42c6439713"
PREFIX = PurePosixPath("Plugins/MoBitsSDK")


def git(*args: str, capture: bool = False) -> bytes:
    result = subprocess.run(["git", *args], stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise RuntimeError("MoBits SDK Git operation failed; check access to the private upstream repository")
    return result.stdout if capture else b""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def apply_unreal_compatibility(target: Path) -> None:
    # The upstream profile helper is named `check`, which collides with UE's
    # check macro when the module is compiled with a shared/unity PCH.
    path = target / "Source/MoBitsSDK/Private/HapticProfile/MoBitsHapticProfileCore.cpp"
    text = path.read_text(encoding="utf-8")
    updated, count = re.subn(r"\bcheck(?=\s*\()", "mrfps_profile_check", text)
    if count == 3:
        with path.open("w", encoding="utf-8", newline="\n") as output:
            output.write(updated)
    elif count != 0 or len(re.findall(r"\bmrfps_profile_check(?=\s*\()", text)) != 3:
        raise RuntimeError("Unexpected MoBits profile helper; compatibility patch was not applied")
    # UE 5.8 stages BuildDir/libs under app/src/main/libs, not app/libs.
    upl = target / "Source/Standalone/MoBitsQuestStandalone_UPL.xml"
    text = upl.read_text(encoding="utf-8")
    original = "implementation files('libs/mobits-quest-standalone.aar')"
    corrected = "implementation files('src/main/libs/mobits-quest-standalone.aar')"
    if text.count(original) == 1 and corrected not in text:
        with upl.open("w", encoding="utf-8", newline="\n") as output:
            output.write(text.replace(original, corrected))
    elif original in text or text.count(corrected) != 1:
        raise RuntimeError("Unexpected MoBits Android UPL; compatibility patch was not applied")
    # Remove the old local init hook, if present. UE's legacy buildscript uses
    # the project Gradle file to place the compatible R8 before AGP's copy.
    text = upl.read_text(encoding="utf-8")
    addition = """  <gradleCopies>
    <copyFile src="$S(PluginDir)/mrfps-r8.gradle" dst="$S(BuildDir)/gradle/mrfps-r8.gradle" />
  </gradleCopies>
  <gradleParameters><insert> --init-script mrfps-r8.gradle</insert></gradleParameters>
"""
    if text.count(addition) == 1:
        with upl.open("w", encoding="utf-8", newline="\n") as output:
            output.write(text.replace(addition, ""))
    elif "mrfps-r8.gradle" in text:
        raise RuntimeError("Unexpected MoBits R8 configuration; it was not replaced")


def installed(target: Path) -> bool:
    marker = target / ".mrfps-sdk.json"
    if not target.exists():
        return False
    if not target.is_dir() or not marker.is_file():
        raise RuntimeError("Existing MoBits SDK directory is not managed by MRFPS; it was not modified")
    data = json.loads(marker.read_text(encoding="utf-8"))
    if data.get("revision") != REVISION or data.get("aar_sha256") != AAR_SHA256:
        raise RuntimeError("Existing managed MoBits SDK has a different revision; it was not modified")
    aar = target / "Source/Standalone/Libs/mobits-quest-standalone.aar"
    if not aar.is_file() or sha256(aar) != AAR_SHA256:
        raise RuntimeError("Local MoBits SDK library is missing or has changed; it was not modified")
    if not (target / "Source/MoBitsSDK/Public/MoBitsQuestStandaloneComponent.h").is_file():
        raise RuntimeError("Local MoBits SDK is incomplete; it was not modified")
    apply_unreal_compatibility(target)
    return True


def ensure(source: Path | None) -> dict:
    base = (ROOT / ".local/mobits-sdk").resolve()
    if not base.is_relative_to(ROOT.resolve()):
        raise RuntimeError("MoBits destination must remain inside this workspace")
    target = base / "Plugins/MoBitsSDK"
    if installed(target):
        return {"status": "ready", "revision": REVISION, "path": str(target)}
    if source is None:
        source = (ROOT / ".local/mobits-sdk-source").resolve()
        if not source.is_relative_to(ROOT.resolve()):
            raise RuntimeError("MoBits source cache must remain inside this workspace")
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            git("clone", "--no-checkout", "--no-tags", "--filter=blob:none", ORIGIN, str(source))
        if not (source / ".git").exists():
            raise RuntimeError("Existing MoBits source cache is not a Git checkout")
        origin = git("-C", str(source), "remote", "get-url", "origin", capture=True).decode().strip()
        if origin.rstrip("/") not in (ORIGIN, ORIGIN.removesuffix(".git")):
            raise RuntimeError("Existing MoBits source cache has a different origin")
        git("-C", str(source), "fetch", "--no-tags", "origin", REVISION)
    else:
        source = source.resolve(strict=True)
    actual = git("-C", str(source), "rev-parse", REVISION + "^{commit}", capture=True).decode().strip()
    if actual != REVISION:
        raise RuntimeError("MoBits source revision mismatch")
    archive = git("-C", str(source), "archive", "--format=tar", REVISION, str(PREFIX), capture=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.parent.resolve().is_relative_to(ROOT.resolve()):
        raise RuntimeError("MoBits staging path must remain inside this workspace")
    with tempfile.TemporaryDirectory(prefix=".mobits-sdk-", dir=target.parent) as temp:
        stage = Path(temp).resolve() / "MoBitsSDK"
        if not stage.is_relative_to(ROOT.resolve()):
            raise RuntimeError("MoBits staging path must remain inside this workspace")
        stage.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
            for item in tar:
                path = PurePosixPath(item.name)
                if not path.is_relative_to(PREFIX):
                    if item.isdir() and PREFIX.is_relative_to(path):
                        continue
                    raise RuntimeError("Unexpected path in MoBits source archive")
                relative = path.relative_to(PREFIX)
                if ".." in relative.parts or not relative.parts:
                    continue
                if "Tests" in relative.parts or "androidTest" in relative.parts:
                    continue
                destination = stage.joinpath(*relative.parts)
                if item.isdir():
                    destination.mkdir(parents=True, exist_ok=True)
                elif item.isfile():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    content = tar.extractfile(item)
                    if content is None:
                        raise RuntimeError("Missing MoBits archive content")
                    with content, destination.open("wb") as output:
                        while block := content.read(1024 * 1024):
                            output.write(block)
                else:
                    raise RuntimeError("Unexpected non-file entry in MoBits source archive")
        aar = stage / "Source/Standalone/Libs/mobits-quest-standalone.aar"
        if not aar.is_file() or sha256(aar) != AAR_SHA256:
            raise RuntimeError("MoBits standalone Android library hash mismatch")
        descriptor = stage / "MoBitsSDK.uplugin"
        plugin = json.loads(descriptor.read_text(encoding="utf-8-sig"))
        for platforms in (plugin["SupportedTargetPlatforms"], plugin["Modules"][0]["PlatformAllowList"]):
            if "Linux" not in platforms:
                platforms.append("Linux")
        descriptor.write_text(json.dumps(plugin, indent=2) + "\n", encoding="utf-8")
        apply_unreal_compatibility(stage)
        (stage / ".mrfps-sdk.json").write_text(json.dumps({"revision": REVISION, "aar_sha256": AAR_SHA256}) + "\n", encoding="utf-8")
        if target.exists():
            raise RuntimeError("MoBits destination appeared during installation; it was not modified")
        os.replace(stage, target)
    return {"status": "installed", "revision": REVISION, "path": str(target)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="existing checkout containing the pinned revision")
    try:
        print(json.dumps(ensure(parser.parse_args().source)))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tarfile.TarError) as exc:
        print("MoBits SDK setup failed: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
