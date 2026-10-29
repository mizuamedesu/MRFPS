#!/usr/bin/env python3
"""Own one lightweight Ubuntu process; inspect/signal only its recorded identity."""
from __future__ import annotations

import argparse
import errno
import json
import os
from pathlib import Path
import signal
import sys
import time
import uuid


def valid_id(value):
    try:
        parsed=uuid.UUID(value)
    except (ValueError, AttributeError):
        raise ValueError("invalid keeper identifier") from None
    if parsed.version!=4 or str(parsed)!=value:
        raise ValueError("invalid keeper identifier")
    return value


def boot_id():
    value=Path('/proc/sys/kernel/random/boot_id').read_text(encoding='ascii').strip()
    if str(uuid.UUID(value))!=value:
        raise RuntimeError('invalid boot identifier')
    return value


def start_ticks(pid):
    # comm is parenthesized and may contain spaces or ')' characters; fields
    # following its final ')' begin at field 3. Linux starttime is field 22.
    raw=Path(f'/proc/{pid}/stat').read_text(encoding='ascii')
    end=raw.rfind(')')
    if end<0:
        raise RuntimeError('invalid process stat')
    fields=raw[end+1:].split()
    if len(fields)<20 or not fields[19].isdigit():
        raise RuntimeError('invalid process start time')
    return int(fields[19])


def process_arguments(pid):
    raw=Path(f'/proc/{pid}/cmdline').read_bytes()
    if not raw or len(raw)>16384 or not raw.endswith(b'\0'):
        raise RuntimeError('invalid process arguments')
    return [os.fsdecode(part) for part in raw[:-1].split(b'\0')]


def same_command(pid, keeper_id):
    args=process_arguments(pid)
    if len(args)!=4 or args[2:]!=['serve',keeper_id]:
        return False
    try:
        return (os.path.samefile(args[0],sys.executable) and
                os.path.samefile(args[1],__file__) and
                os.path.samefile(f'/proc/{pid}/exe',sys.executable))
    except (FileNotFoundError, OSError):
        return False


def inspect(pid, ticks, expected_boot, keeper_id, send_stop=False):
    """Pin one Linux PID via pidfd; never signal a changed or recycled PID."""
    valid_id(keeper_id)
    if type(pid) is not int or not 1<pid<2**31 or type(ticks) is not int or not 0<=ticks<2**64:
        raise ValueError('invalid process identity')
    if str(uuid.UUID(expected_boot))!=expected_boot:
        raise ValueError('invalid boot identifier')
    if boot_id()!=expected_boot:
        return {'status':'gone','reason':'boot_changed'}
    if not callable(getattr(os,'pidfd_open',None)) or not callable(getattr(signal,'pidfd_send_signal',None)):
        return {'status':'unknown','reason':'pidfd_unavailable'}
    try:
        fd=os.pidfd_open(pid,0)
    except OSError as error:
        if error.errno==errno.ESRCH:
            return {'status':'gone','reason':'pid_absent'}
        return {'status':'unknown','reason':'pidfd_failed'}
    try:
        try:
            before=start_ticks(pid)
            if before!=ticks:
                return {'status':'gone','reason':'pid_reused'}
            if not same_command(pid,keeper_id):
                return {'status':'unknown','reason':'arguments_changed'}
            if start_ticks(pid)!=before:
                return {'status':'unknown','reason':'identity_changed'}
            signal.pidfd_send_signal(fd,0)
            if send_stop:
                signal.pidfd_send_signal(fd,signal.SIGTERM)
                return {'status':'stop_sent'}
            return {'status':'running'}
        except (ProcessLookupError,FileNotFoundError):
            return {'status':'gone','reason':'process_exited'}
        except (OSError,ValueError,RuntimeError):
            return {'status':'unknown','reason':'inspection_failed'}
    finally:
        os.close(fd)


def serve(keeper_id):
    valid_id(keeper_id)
    stopped=False
    def request_stop(_signum,_frame):
        nonlocal stopped
        stopped=True
    signal.signal(signal.SIGTERM,request_stop)
    pid=os.getpid()
    record={'version':1,'id':keeper_id,'pid':pid,'start_ticks':start_ticks(pid),'boot_id':boot_id()}
    print(json.dumps(record,separators=(',',':')),flush=True)
    while not stopped:
        time.sleep(1)
    return 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    run=sub.add_parser('serve'); run.add_argument('id')
    for name in ('inspect','stop'):
        p=sub.add_parser(name); p.add_argument('id'); p.add_argument('pid',type=int)
        p.add_argument('start_ticks',type=int); p.add_argument('boot_id')
    args=parser.parse_args()
    if args.command=='serve':
        return serve(args.id)
    result=inspect(args.pid,args.start_ticks,args.boot_id,args.id,args.command=='stop')
    print(json.dumps(result,separators=(',',':')))
    return 0 if result['status']!='unknown' else 2


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (OSError,ValueError,RuntimeError):
        print(json.dumps({'status':'unknown','reason':'helper_failed'}),file=sys.stderr)
        raise SystemExit(2)
