#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Black-box single-instance smoke test (does not assert OS foreground policy).

Called by CTest with --executable <built KomaMimic>.  Runs offscreen; no desktop
session or Wayland activation token is required. Each run uses private configs.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class SmokeFailure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SmokeFailure(message)


def start(executable, args, environment, log):
    return subprocess.Popen(
        [str(executable), *map(str, args)],
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        env=environment,
    )


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=8)


def ensure_running(process, label):
    code = process.poll()
    require(code is None, f"{label} exited unexpectedly ({code})")


def run(executable):
    env = os.environ.copy()
    env['QT_QPA_PLATFORM'] = 'offscreen'
    env.setdefault('QTWEBENGINE_CHROMIUM_FLAGS', '--disable-gpu')
    if os.name != 'nt' and hasattr(os, 'geteuid') and os.geteuid() == 0:
        env['QTWEBENGINE_DISABLE_SANDBOX'] = '1'

    with tempfile.TemporaryDirectory(prefix='koma-single-instance-') as directory:
        root = Path(directory)
        config = root / 'settings with spaces.ini'
        # Require an explicit, isolated single-instance setting; no user data.
        config.write_text('[%General]\nsingleInstance=true\n', encoding='utf-8')
        logfile = root / 'primary.log'
        otherlog = root / 'other.log'
        primary = None
        other = None
        try:
            with logfile.open('wb') as primary_output, otherlog.open('wb') as other_output:
                primary = start(executable, ['--config', config], env, primary_output)
                lock_path = Path(str(config) + '.instance.lock')
                deadline = time.monotonic() + 40
                while not lock_path.exists() and time.monotonic() < deadline:
                    ensure_running(primary, 'primary')
                    time.sleep(0.1)
                require(lock_path.exists(), 'primary did not acquire the instance lock')

                # The primary acquires its lock before publishing QLocalServer.
                # Retry a cold-start race; only the final successful attempt is
                # interpreted as the IPC regression result.
                last_error = ''
                reply = None
                for attempt in range(8):
                    ensure_running(primary, 'primary')
                    try:
                        reply = subprocess.run(
                            [str(executable), '--config', str(config)],
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=15, check=False,
                        )
                    except subprocess.TimeoutExpired as error:
                        raise SmokeFailure(f'secondary timed out: {error}') from error
                    if reply.returncode == 0:
                        break
                    last_error = (f'secondary returned {reply.returncode}: '
                                  + reply.stdout.decode('utf-8', errors='replace')[-1800:])
                    time.sleep(0.5)
                else:
                    raise SmokeFailure(last_error)
                require(reply is not None and reply.returncode == 0, last_error)
                ensure_running(primary, 'primary after forwarded request')
                print('PASS: repeated launch forwards to the running instance')

                # --new-window must bypass IPC and remain alive as a second GUI.
                other = start(executable, ['--config', config, '--new-window'],
                              env, other_output)
                time.sleep(1.5)
                ensure_running(primary, 'primary with --new-window')
                ensure_running(other, '--new-window process')
                print('PASS: --new-window starts another instance')
                stop(other)
                other = None

            print('PASS: the original instance remains alive throughout')
        except Exception:
            for name, log in [('primary', logfile), ('new-window', otherlog)]:
                if log.is_file():
                    print(f'--- {name} log ---', file=sys.stderr)
                    print(log.read_text(encoding='utf-8', errors='replace')[-4000:],
                          file=sys.stderr)
            raise
        finally:
            stop(other)
            stop(primary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f'executable not found: {executable}')
    try:
        run(executable)
    except (SmokeFailure, OSError, subprocess.SubprocessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
