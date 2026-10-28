#!/usr/bin/env python3
"""Check a real official MoBits v2 app, without ARM or playback commands.

Requires websocket-client==1.9.0. Reads credentials only from the same-user
discovery file. Outputs an allowlisted report, never headers/messages/secrets.
STOP_ALL is the only haptics command; use while the app has no other SDK owner.
Protocol: MoBits-app-tauri 2f025306 / src-tauri/src/sdk_bridge/{server,protocol}.rs.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import select
import time

from quest_bridge import validate_discovery


def check(path: Path, duration: float) -> dict:
    import websocket

    # The library's debug trace includes Authorization. Keep it explicitly off.
    websocket.enableTrace(False)
    raw = path.read_bytes()
    validate_discovery(raw)
    descriptor = json.loads(raw.decode('utf-8'))
    report = {'timestamp_utc': datetime.now(timezone.utc).isoformat(),
              'passed': False, 'discovery_valid': True,
              'control_hello': False, 'motion_hello': False,
              'device_response': False, 'device_count': 0, 'connected_devices': 0,
              'motion_frames': 0, 'pongs': 0, 'stop_receipt_confirmed': False,
              'arm_requests_sent': 0, 'play_requests_sent': 0,
              'physical_feedback_tested': False}
    channels = []
    stage = 'control_handshake'
    try:
        control = websocket.create_connection(
            descriptor['control_url'], suppress_origin=True, timeout=0.3,
            header={'Authorization': 'Bearer ' + descriptor['token']},
            http_no_proxy=['127.0.0.1'])
        channels.append(control)

        def receive(channel):
            data = channel.recv()
            if not isinstance(data, str) or len(data) > 262144:
                raise ValueError('Invalid bounded JSON message')
            value = json.loads(data)
            if not isinstance(value, dict):
                raise ValueError('Invalid object')
            return value

        def hello(channel, expected):
            value = receive(channel)
            if not (value.get('type') == 'HELLO' and value.get('version') == 2
                    and value.get('channel') == expected
                    and value.get('session_id') == descriptor['session_id']
                    and value.get('heartbeat_ms') == 100 and value.get('timeout_ms') == 500):
                raise ValueError('HELLO contract mismatch')
            report[expected + '_hello'] = True

        stage = 'control_hello'
        hello(control, 'control')
        stage = 'motion_handshake'
        motion = websocket.create_connection(
            descriptor['motion_url'], suppress_origin=True, timeout=0.3,
            header={'Authorization': 'Bearer ' + descriptor['token'],
                    'X-MoBits-Session': descriptor['session_id']},
            http_no_proxy=['127.0.0.1'])
        channels.append(motion)
        stage = 'motion_hello'
        hello(motion, 'motion')
        # No output permission is requested. Only query, heartbeat and STOP.
        control.send(json.dumps({'type': 'GET_DEVICES', 'id': 1}))
        control.send(json.dumps({'type': 'STOP_ALL', 'id': 2}))
        last_sequence = 0
        next_ping = 0.0
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_ping:
                control.send(json.dumps({'type': 'PING', 'last_frame_sequence': last_sequence}))
                next_ping = now + 0.08
            readable, _, _ = select.select([control.sock, motion.sock], [], [], 0.015)
            for channel in channels:
                if channel.sock not in readable:
                    continue
                stage = 'motion_receive' if channel is motion else 'control_receive'
                value = receive(channel)
                kind = value.get('type')
                if channel is motion:
                    sequence = value.get('sequence')
                    if (kind != 'MOTION' or value.get('version') != 2
                            or value.get('session_id') != descriptor['session_id']
                            or type(sequence) is not int or sequence <= last_sequence):
                        raise ValueError('Motion contract mismatch')
                    last_sequence = sequence
                    report['motion_frames'] += 1
                elif kind == 'PONG':
                    report['pongs'] += 1
                elif kind == 'DEVICES' and value.get('id') == 1:
                    devices = value.get('devices')
                    if not isinstance(devices, list):
                        raise ValueError('Invalid device inventory')
                    report['device_response'] = True
                    report['device_count'] = len(devices)
                    report['connected_devices'] = sum(d.get('connected') is True for d in devices)
                elif kind == 'RESULT' and value.get('id') == 2:
                    report['stop_receipt_confirmed'] = value.get('status') == 'stopped_confirmed'
                else:
                    raise ValueError('Unexpected control message')
        report['passed'] = all((report['control_hello'], report['motion_hello'],
                                report['device_response'], report['motion_frames'] >= 10,
                                report['pongs'] >= 5, report['stop_receipt_confirmed']))
        return report
    except Exception as error:
        report['failure_type'] = type(error).__name__
        report['failure_stage'] = stage
        return report
    finally:
        for channel in reversed(channels):
            try:
                channel.close(timeout=0.2)
            except Exception:
                pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--discovery', type=Path, default=Path(os.environ.get('LOCALAPPDATA', '')) /
                        'MoBits/SDK/connection-v2.json')
    parser.add_argument('--seconds', type=float, default=2.0)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 10:
        parser.error('--seconds must be in [1, 10]')
    try:
        report = check(args.discovery, args.seconds)
    except Exception as error:
        # Exception text may contain secret-bearing headers, never serialize it.
        report = {'timestamp_utc': datetime.now(timezone.utc).isoformat(), 'passed': False,
                  'failure_type': type(error).__name__, 'arm_requests_sent': 0,
                  'play_requests_sent': 0, 'physical_feedback_tested': False}
    output = json.dumps(report, indent=2)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(output + '\n', encoding='utf-8')
    print(output)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
