#!/usr/bin/env python3
"""Inspect or idempotently install an MRFPS Quest APK, without launching it."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import device_login as login


def apk_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as src:
        for chunk in iter(lambda: src.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def installed_hash(adb: str, serial: str) -> str | None:
    result = login.adb_result(adb, serial, ['shell', 'pm', 'path', login.PACKAGE])
    output = result.stdout.decode('utf-8', 'replace').strip()
    error = result.stderr.decode('utf-8', 'replace').strip()
    if result.returncode == 1 and not output and not error:
        return None
    if result.returncode or error:
        raise RuntimeError('Cannot inspect installed Quest package')
    entries = output.splitlines()
    if len(entries) != 1:
        raise RuntimeError('Installed package paths are ambiguous; no installation attempted')
    match = re.fullmatch(r'package:(/data/app/[A-Za-z0-9_./~+=-]+/base\.apk)', entries[0].strip())
    if not match:
        raise RuntimeError('Installed package path is not recognized; no installation attempted')
    path = match.group(1)
    value = login.run_adb(adb, serial, ['shell', 'sha256sum', path])
    match = re.fullmatch(r'([0-9a-fA-F]{64})\s+' + re.escape(path), value)
    if not match:
        raise RuntimeError('Cannot verify installed package hash')
    return match.group(1).lower()


def run(args: argparse.Namespace) -> dict:
    state_path = login.quest_state_path(args.serial)  # also validates serial
    apk = args.apk.resolve(strict=True)
    if not apk.is_file():
        raise ValueError('APK must be a file')
    with login.state_lock(state_path):
        badging = subprocess.run([str(args.aapt), 'dump', 'badging', str(apk)],
                                 capture_output=True, timeout=30, check=True).stdout.decode('utf-8', 'strict')
        packages = re.findall(r"(?m)^package: name='([^']+)'", badging)
        if packages != [login.PACKAGE]:
            raise ValueError('APK package is not org.mrfps.arena')
        digest = apk_hash(apk)
        if login.run_adb(args.adb, args.serial, ['get-state']) != 'device':
            raise RuntimeError('Quest is not authorized and online')
        current = installed_hash(args.adb, args.serial)
        if current == digest:
            return {'status': 'already_installed', 'sha256': digest}
        if not args.install:
            return {'status': 'different' if current else 'not_installed', 'sha256': digest}
        # `install -r` can stop the existing app and indirectly dismiss a
        # permission prompt. Inspect immediately before touching this package.
        login.assert_quest_share_clear(args.adb, args.serial)
        if apk_hash(apk) != digest:
            raise RuntimeError('APK changed during inspection; no installation attempted')
        result = login.adb_result(args.adb, args.serial, ['install', '-r', str(apk)])
        lines = result.stdout.decode('utf-8', 'replace').splitlines()
        if result.returncode or not lines or lines[-1].strip() != 'Success':
            raise RuntimeError('APK installation did not report success')
        if installed_hash(args.adb, args.serial) != digest:
            raise RuntimeError('Installed package hash does not match the APK')
        return {'status': 'installed', 'sha256': digest}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--aapt', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--install', action='store_true', help='install if needed; otherwise only inspect')
    try:
        print(json.dumps(run(parser.parse_args())))
        return 0
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError, UnicodeError) as exc:
        print(json.dumps({'status': 'error', 'message': str(exc)}), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())