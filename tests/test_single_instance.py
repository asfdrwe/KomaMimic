#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Black-box single-instance smoke test (does not assert OS foreground policy).

Called by CTest with --executable <built KomaMimic>.  Runs offscreen; no desktop
session or Wayland activation token is required. Each run uses private configs.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from isolated_test import test_environment


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


def run(executable, paths_probe):
    with tempfile.TemporaryDirectory(prefix='koma-single-instance-') as directory:
        root = Path(directory)
        def prepare(profile):
            env = test_environment(profile)
            # These instances belong to the same isolated CTest entry. Keep its
            # short socket directory; config paths give them distinct IPC names.
            for name in ('TMPDIR', 'TMP', 'TEMP'):
                if name in os.environ:
                    env[name] = os.environ[name]
            env['QT_QPA_PLATFORM'] = 'offscreen'
            env.setdefault('QTWEBENGINE_CHROMIUM_FLAGS', '--disable-gpu')
            if os.name != 'nt' and hasattr(os, 'geteuid') and os.geteuid() == 0:
                env['QTWEBENGINE_DISABLE_SANDBOX'] = '1'
            # Check the actual Qt result before starting a GUI that could save
            # thumbnails. Do not continue if OS redirection did not take effect.
            result = subprocess.run([str(paths_probe), '--standard-paths'], env=env,
                                    capture_output=True, text=True, encoding='utf-8', check=True, timeout=15)
            cache = Path(json.loads(result.stdout)['cache']).resolve()
            require(cache.is_relative_to(profile.resolve()),
                    f'OS cache directory is outside the test environment: {cache}')
            config = profile / 'config' / 'settings with spaces.ini'
            config.write_text('[%General]\nsingleInstance=true\n', encoding='utf-8')
            return env, config
        env, config = prepare(root / 'test')
        unrelated_env, unrelated_config = prepare(root / 'unrelated')
        logfile = root / 'primary.log'
        otherlog = root / 'other.log'
        unrelatedlog = root / 'unrelated.log'
        primary = None
        other = None
        unrelated = None
        def wait_for_lock(process, settings, label):
            lock = Path(str(settings) + '.instance.lock')
            deadline = time.monotonic() + 40
            while not lock.exists() and time.monotonic() < deadline:
                ensure_running(process, label)
                time.sleep(0.1)
            require(lock.exists(), f'{label} did not acquire the instance lock')
        try:
            with logfile.open('wb') as primary_output, otherlog.open('wb') as other_output, unrelatedlog.open('wb') as unrelated_output:
                unrelated = start(executable, ['--config', unrelated_config], unrelated_env, unrelated_output)
                wait_for_lock(unrelated, unrelated_config, 'unrelated instance')
                primary = start(executable, ['--config', config], env, primary_output)
                wait_for_lock(primary, config, 'test primary')
                ensure_running(unrelated, 'unrelated instance after test startup')

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
                ensure_running(unrelated, 'unrelated instance after forwarded request')
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

                result = subprocess.run([str(executable), '--config', str(config), '--execute', 'quit'],
                                        env=env, capture_output=True, timeout=15, check=False)
                require(result.returncode == 0, 'test-profile quit was not forwarded')
                require(primary.wait(timeout=15) == 0, 'test primary did not exit normally')
                ensure_running(unrelated, 'unrelated instance after test-profile quit')
                print('PASS: separate environments stay independent, including forwarded quit')

            print('PASS: the unrelated instance remains alive throughout')
        except Exception:
            for name, log in [('primary', logfile), ('new-window', otherlog), ('unrelated', unrelatedlog)]:
                if log.is_file():
                    print(f'--- {name} log ---', file=sys.stderr)
                    print(log.read_text(encoding='utf-8', errors='replace')[-4000:],
                          file=sys.stderr)
            raise
        finally:
            stop(other)
            stop(primary)
            stop(unrelated)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--paths-probe', required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f'executable not found: {executable}')
    try:
        run(executable, args.paths_probe.resolve())
    except (SmokeFailure, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
