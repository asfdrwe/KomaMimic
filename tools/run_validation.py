#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build, run CTest, retain local evidence, and create editable acceptance forms."""
import argparse
import collections
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

from export_source import public_files
from validation_cases import cases_for, STATUSES

ROOT = Path(__file__).resolve().parents[1]


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def cache_values(build):
    path = Path(build) / 'CMakeCache.txt'
    if not path.is_file():
        return {}
    return {m[1]: m[2] for line in path.read_text(encoding='utf-8', errors='replace').splitlines()
            if (m := re.match(r'([^/#][^:]*):[^=]*=(.*)', line))}


def windows_paths(build):
    cache = cache_values(build)
    triplet = cache.get('VCPKG_TARGET_TRIPLET', 'x64-windows')
    prefix = Path(cache.get('VCPKG_INSTALLED_DIR', str(Path(build) / 'vcpkg_installed'))) / triplet
    sdk = cache.get('KOMAMIMIC_QT_ROOT', '') if cache.get('KOMAMIMIC_WINDOWS_FAST') == 'ON' else ''
    qt = Path(sdk) / 'bin' if sdk else prefix / 'tools/Qt6/bin'
    return prefix, qt


def runtime_environment(build):
    env = os.environ.copy()
    if platform.system() == 'Windows':
        prefix, tools = windows_paths(build)
        # SDK Qt first: do not load another Qt installation from a developer PATH.
        directories = (tools, prefix / 'bin') if cache_values(build).get('KOMAMIMIC_WINDOWS_FAST') == 'ON' else (prefix / 'bin', tools)
        env['PATH'] = os.pathsep.join(map(str, directories)) + os.pathsep + env.get('PATH', '')
        qtpaths = next((p for p in (tools / 'qtpaths.exe', tools / 'qtpaths6.exe') if p.is_file()), None)
        if qtpaths:
            env['QT_PLUGIN_PATH'] = subprocess.check_output([str(qtpaths), '--query', 'QT_INSTALL_PLUGINS'], env=env, text=True).strip()
    return env


def execute(command, log, env=None, cwd=None, timeout=None):
    command = [str(x) for x in command]
    print('+', subprocess.list2cmdline(command), flush=True)
    start = time.monotonic()
    with Path(log).open('w', encoding='utf-8') as stream:
        stream.write(subprocess.list2cmdline(command) + '\n')
        stream.flush()
        try:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=timeout, check=False)
            code = result.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            stream.write('\n' + str(error) + '\n')
            code = 124 if isinstance(error, subprocess.TimeoutExpired) else 127
    duration = round(time.monotonic() - start, 2)
    print(f'  exit={code}, {duration}s; {log}', flush=True)
    return dict(command=command, returncode=code, seconds=duration, log=Path(log).name)


def create_forms(output, system, scope='full'):
    cases = cases_for(system, scope)
    fields = ['id', 'status', 'input', 'operation', 'expected', 'actual', 'evidence']
    with (output / 'manual.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows({**{k: c[k] for k in ('id', 'input', 'operation', 'expected')},
                         'status': 'NOT_RUN', 'actual': '', 'evidence': ''} for c in cases)
    (output / 'manual-guide.md').write_text(
        '# 手動確認票\n\nmanual.csvを編集してstatus、actual、evidenceへ記録します。\n'
        'status: ' + ', '.join(STATUSES) + '。NA/BLOCKEDには理由が必要です。\n'
        'PASSには観察結果を書きます。未実行をPASSにせず、端末／版を変更したら別の結果フォルダを作ります。\n\n' +
        '\n\n'.join(f"## {c['id']}\n\n入力: {c['input']}\n\n操作: {c['operation']}\n\n期待: {c['expected']}" for c in cases) + '\n', encoding='utf-8')
    save_json(output / 'environment-manual.json', dict(
        os_edition_version='', cpu='', ram='', gpu_driver='', qt_platform='', desktop_session='',
        display_scaling='', monitors='', input_devices='', printer='', power_timeout='',
        dependency_install_date='', notes='実際のOS版・GPU・Wayland/X11・DPI・機器を補記'))


def collect_test_evidence(build, output):
    logs = Path(build) / 'Testing/Temporary'
    for name in ('LastTest.log', 'LastTestsFailed.log', 'CTestCostData.txt'):
        if (logs / name).is_file():
            shutil.copy2(logs / name, output / name)
    text = (output / 'LastTest.log').read_text(encoding='utf-8', errors='replace') if (output / 'LastTest.log').exists() else ''
    # QtTest can send its default Windows output to OutputDebugString instead
    # of CTest's captured stdout. Prefer the independent per-suite file logs.
    qt_files = sorted((Path(build) / 'tests').glob('*.qtest.txt'))
    qt_text = text
    if qt_files:
        qt_output = output / 'qt-tests'
        qt_output.mkdir(exist_ok=True)
        for path in qt_files:
            shutil.copy2(path, qt_output / path.name)
        qt_text = '\n'.join(path.read_text(encoding='utf-8', errors='replace') for path in qt_files)
        skipped = [line for line in qt_text.splitlines() if re.search(r'\bSKIP\s*[:\(]', line)]
        skipped += [line for line in text.splitlines() if re.search(r'skipped\s+[\"\']|^skipped ', line)]
    else:
        skipped = [line for line in text.splitlines() if re.search(r'\bSKIP\s*[:\(]|skipped\s+[\"\']|^skipped ', line)]
    (output / 'skips.txt').write_text('\n'.join(skipped) + '\n', encoding='utf-8')
    totals = [dict(passed=int(a), failed=int(b), skipped=int(c))
              for a, b, c in re.findall(r'Totals:\s*(\d+) passed,\s*(\d+) failed,\s*(\d+) skipped', qt_text)]
    suites = []
    junit = output / 'ctest.xml'
    if junit.exists():
        for case in ET.parse(junit).iter('testcase'):
            suites.append(dict(name=case.get('name'), status='FAIL' if case.find('failure') is not None or case.find('error') is not None else
                               'SKIP' if case.find('skipped') is not None else 'PASS'))
    return dict(qt_totals={key: sum(t[key] for t in totals) for key in ('passed', 'failed', 'skipped')},
                qt_total_lines=len(totals), skip_lines=len(skipped), suites=suites,
                qt_totals_available=bool(totals), qt_log_files=[p.name for p in qt_files],
                python_tests_run=sum(map(int, re.findall(r'Ran (\d+) tests? in ', text))),
                note='Qtの集計はinit/cleanupとCロケール再実行を含む。Python件数はLastTest.logを参照。')


def run(args):
    source = ROOT
    build, output, fixtures = args.build_dir.resolve(), args.output.resolve(), args.fixtures.resolve()
    if output.exists():
        raise ValueError('Result directory exists; use a new --output for each run')
    if build == source or source in build.parents and not build.name.startswith('build'):
        raise ValueError('Use a separate build directory (or source/build-*)')
    if not (fixtures / 'FIXTURES.json').is_file():
        raise ValueError('Missing fixtures/FIXTURES.json; generate or extract the validation kit')
    fixture_manifest = json.loads((fixtures / 'FIXTURES.json').read_text(encoding='utf-8'))
    for record in fixture_manifest['files']:
        path = fixtures / record['path']
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
            raise ValueError('Fixture missing or changed: ' + record['path'])
    source_records = [{'path': p.relative_to(source).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                      for p in public_files(source)]
    output.mkdir(parents=True)
    scope = getattr(args, 'test_scope', None) or ('platform' if platform.system() == 'Windows' else 'full')
    windows_mode = getattr(args, 'windows_mode', 'prebuilt')
    create_forms(output, platform.system(), scope)
    save_json(output / 'SOURCE_MANIFEST.json', dict(schema=1, files=source_records))
    shutil.copy2(fixtures / 'FIXTURES.json', output / 'FIXTURES.json')
    data = dict(schema=1, started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                system=platform.system(), test_scope=scope, windows_mode=windows_mode if platform.system() == 'Windows' else None, platform=platform.platform(), machine=platform.machine(),
                python=sys.version, source=str(source), build=str(build), fixtures=str(fixtures),
                source_manifest_sha256=hashlib.sha256((output / 'SOURCE_MANIFEST.json').read_bytes()).hexdigest(),
                fixtures_manifest_sha256=hashlib.sha256((fixtures / 'FIXTURES.json').read_bytes()).hexdigest(),
                environment={k: os.environ[k] for k in ('XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP', 'QT_QPA_PLATFORM',
                    'QTWEBENGINE_DISABLE_SANDBOX', 'QTWEBENGINE_CHROMIUM_FLAGS', 'VCPKG_ROOT',
                    'PYTHONUTF8', 'VCPKG_KEEP_ENV_VARS', 'VCPKG_MAX_CONCURRENCY', 'KOMAMIMIC_QT_ROOT') if k in os.environ},
                steps=[], status='RUNNING')
    def step(name, command, env=None, timeout=None):
        record = execute(command, output / (name + '.log'), env, source, timeout)
        data['steps'].append(dict(name=name, **record))
        save_json(output / 'automatic.json', data)
        if record['returncode']:
            raise RuntimeError(f'{name} failed; see {output / (name + ".log")}')
    try:
        step('source-check', [sys.executable, source / 'tools/check_source.py'], timeout=120)
        configure = ['cmake', '-S', source, '-B', build, '-DBUILD_TESTING=ON', '-DKOMAMIMIC_TEST_SCOPE=' + scope]
        if platform.system() == 'Windows':
            vcpkg = os.environ.get('VCPKG_ROOT')
            if not vcpkg or not (Path(vcpkg) / 'scripts/buildsystems/vcpkg.cmake').is_file():
                raise ValueError('Set VCPKG_ROOT to a bootstrapped vcpkg checkout (see Windows guide)')
            configure += ['-G', 'Visual Studio 17 2022', '-A', 'x64',
                          '-DCMAKE_TOOLCHAIN_FILE=' + str(Path(vcpkg) / 'scripts/buildsystems/vcpkg.cmake'),
                          ]
            old = cache_values(build)
            if old and (old.get('KOMAMIMIC_WINDOWS_FAST') == 'ON') != (windows_mode == 'prebuilt'):
                raise ValueError('Use separate build directories for prebuilt Qt and Qt source builds')
            if windows_mode == 'prebuilt':
                from setup_windows import verify_sdk
                sdk = Path(getattr(args, 'qt_root', None) or os.environ.get('KOMAMIMIC_QT_ROOT', ''))
                identity = verify_sdk(sdk.resolve())
                save_json(output / 'QT_SDK.json', identity)
                configure += ['-DKOMAMIMIC_WINDOWS_FAST=ON', '-DKOMAMIMIC_QT_ROOT=' + str(sdk.resolve())]
            else:
                configure += ['-DKOMAMIMIC_WINDOWS_FAST=OFF', '-DVCPKG_TARGET_TRIPLET=x64-windows',
                              '-DVCPKG_HOST_TRIPLET=x64-windows']
        else:
            configure += ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release']
            if platform.system() == 'Darwin':
                if platform.machine() != 'arm64':
                    raise ValueError('Run native arm64 Python/Terminal/Homebrew; Rosetta/x64 is not this test target')
                configure += ['-DCMAKE_OSX_ARCHITECTURES=arm64']
        configure += args.cmake_arg
        step('configure', configure)
        step('build', ['cmake', '--build', build, '--config', 'Release', '--parallel', args.jobs])
        env = runtime_environment(build)
        cli_env = dict(env, QT_QPA_PLATFORM='offscreen')
        executable = build / ('Release/komamimic.exe' if platform.system() == 'Windows' else 'komamimic')
        for name in ('version', 'formats', 'commands'):
            step(name, [executable, '--' + name], cli_env, 60)
        # Do not mistake logs from an earlier scope/run for this attempt.
        for path in (build / 'tests').glob('*.qtest.txt'):
            path.unlink()
        step('ctest', ['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
                      '--parallel', '2', '--no-tests=error', '--output-junit', output / 'ctest.xml'], env)
        probe = build / ('Release/validation_probe.exe' if platform.system() == 'Windows' else 'validation_probe')
        step('fixture-probe', [probe, fixtures], cli_env, 120)
        data['status'] = 'PASS'
    except (ValueError, RuntimeError, OSError) as error:
        data['status'] = 'FAIL'
        data['error'] = str(error)
        print(error, file=sys.stderr)
    finally:
        data['tests'] = (collect_test_evidence(build, output) if any(s['name'] == 'ctest' for s in data['steps'])
                         else dict(qt_totals={}, suites=[], note='CTest was not run in this attempt'))
        cache = build / 'CMakeCache.txt'
        if cache.is_file():
            shutil.copy2(cache, output / cache.name)
        # Dependency versions and compiler details are local diagnostics, never uploaded.
        commands = {'Linux': ['rpm', '-q', 'gcc-c++', 'qt6-qtbase', 'qt6-qtwebengine', 'poppler', 'libarchive', 'sdl2-compat'],
                    'Darwin': ['brew', 'list', '--versions'],
                    'Windows': ['git', '-C', os.environ.get('VCPKG_ROOT', '.'), 'rev-parse', 'HEAD']}
        execute(commands[platform.system()], output / 'dependency-versions.log', timeout=60)
        status = Path(cache_values(build).get('VCPKG_INSTALLED_DIR', str(build / 'vcpkg_installed'))) / 'vcpkg/status'
        if platform.system() == 'Windows' and status.is_file():
            shutil.copy2(status, output / 'vcpkg-status.txt')
        data['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save_json(output / 'automatic.json', data)
        summarize(output)
    return 0 if data['status'] == 'PASS' else 1


def launch(args):
    output, executable = args.results.resolve(), args.executable.resolve()
    if not (output / 'automatic.json').exists() or not executable.is_file():
        raise ValueError('Missing result run or executable')
    data = json.loads((output / 'automatic.json').read_text(encoding='utf-8'))
    # Use development DLL paths only for build-tree binaries, never for a staged app.
    build = Path(data['build'])
    env = runtime_environment(build) if executable.is_relative_to(build) else os.environ.copy()
    for key in ('QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QTWEBENGINEPROCESS_PATH',
                'QTWEBENGINE_RESOURCES_PATH', 'QTWEBENGINE_LOCALES_PATH', 'QTWEBENGINE_DISABLE_SANDBOX', 'QTWEBENGINE_CHROMIUM_FLAGS'):
        if key == 'QT_PLUGIN_PATH' and executable.is_relative_to(build) and platform.system() == 'Windows':
            continue
        env.pop(key, None)
    profile = output / 'profiles' / args.profile
    profile.mkdir(parents=True, exist_ok=True)
    if platform.system() == 'Linux':
        env['XDG_CACHE_HOME'] = str(profile / 'cache')
    config = profile / 'settings.ini'
    # Qt's cache is OS-global on macOS/Windows; start disabled until the tester
    # deliberately enables it in a dedicated OS account (C26).
    if not config.exists():
        initial = '[cache]\nthumbnailsEnabled=false\n' if platform.system() in ('Windows', 'Darwin') else ''
        if args.profile == 'legacy':
            initial += '[text]\nrenderer=0\n'
        if initial:
            config.write_text(initial, encoding='utf-8')
    command = [str(executable), '--config', str(config)]
    if args.input:
        command.append(str(args.input.resolve()))
    number = len(list(output.glob('gui-*.log'))) + 1
    save_json(output / f'gui-{number:03}-identity.json', dict(executable=str(executable),
              sha256=hashlib.sha256(executable.read_bytes()).hexdigest(), config=str(config),
              started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()))
    return execute(command, output / f'gui-{number:03}.log', env=env)['returncode']


def summarize(output):
    output = Path(output).resolve()
    data = json.loads((output / 'automatic.json').read_text(encoding='utf-8'))
    with (output / 'manual.csv').open(encoding='utf-8-sig', newline='') as stream:
        rows = list(csv.DictReader(stream))
    expected = {c['id'] for c in cases_for(data['system'], data.get('test_scope', 'full'))}
    seen, counts = set(), collections.Counter()
    for row in rows:
        if row['id'] not in expected or row['id'] in seen or row['status'] not in STATUSES:
            raise ValueError('Invalid/duplicate case or status in manual.csv: ' + str(row.get('id')))
        if row['status'] != 'NOT_RUN' and not row['actual'].strip():
            raise ValueError('Write observed result / reason in actual: ' + row['id'])
        seen.add(row['id'])
        counts[row['status']] += 1
    if seen != expected:
        raise ValueError('Cases removed from manual.csv: ' + ', '.join(sorted(expected - seen)))
    tests = data.get('tests', {})
    summary = dict(automatic_status=data['status'], test_scope=data.get('test_scope', 'full'), qt_totals=tests.get('qt_totals', {}),
                   qt_totals_available=tests.get('qt_totals_available', bool(tests.get('qt_total_lines', 0))),
                   python_tests_run=tests.get('python_tests_run', 0),
                   manual_counts={s: counts[s] for s in STATUSES},
                   all_manual_passed=counts['PASS'] == len(expected),
                   note='自動試験成功や一部手動成功から全OS・配布可能を推定しない。NAは理由をレビューする。')
    save_json(output / 'summary.json', summary)
    print(json.dumps(summary, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('run')
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--fixtures', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--windows-mode', choices=('prebuilt', 'source'), default='prebuilt')
    p.add_argument('--qt-root', type=Path, help='Matching prebuilt SDK; defaults to KOMAMIMIC_QT_ROOT')
    p.add_argument('--test-scope', choices=('full', 'platform'), help='Default: Windows platform, other OS full')
    p.add_argument('--cmake-arg', action='append', default=[], help='Use --cmake-arg=-DNAME=VALUE')
    p = commands.add_parser('launch')
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--results', type=Path, required=True)
    p.add_argument('--profile', choices=('main', 'import', 'ipc', 'legacy'), default='main')
    p.add_argument('--input', type=Path)
    p = commands.add_parser('report')
    p.add_argument('--results', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'run':
            return run(args)
        if args.command == 'launch':
            return launch(args)
        summarize(args.results)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, ET.ParseError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
