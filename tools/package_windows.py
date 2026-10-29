#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package a verified Windows stage; audit PE dependencies on any host.

Native Windows is required for pack/verify. This creates Actions candidates,
not a GitHub Release. Dependency corresponding-source review remains separate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import struct
import subprocess
import sys
import zipfile

from export_source import public_files
from prepare_release import prepare
from run_validation import ROOT, cache_values, execute, save_json, windows_paths

# Windows 10/11 system components, including the OS ICU C API (icuuc.dll).
# Never accept an arbitrary DLL merely because the build runner has it installed.
SYSTEM_DLLS = set('''advapi32.dll authz.dll avrt.dll bcrypt.dll bcryptprimitives.dll
    bthprops.cpl cabinet.dll cfgmgr32.dll comctl32.dll comdlg32.dll crypt32.dll
    cryptbase.dll cryptsp.dll d3d9.dll d3d11.dll d3d12.dll dbghelp.dll dcomp.dll
    dhcpcsvc.dll dnsapi.dll dsound.dll dwmapi.dll dwrite.dll dxgi.dll dxva2.dll
    fontsub.dll gdi32.dll hid.dll icu.dll icuin.dll icuuc.dll imagehlp.dll imm32.dll
    iphlpapi.dll kernel32.dll ksuser.dll mf.dll mfplat.dll mfreadwrite.dll
    mfuuid.dll mmdevapi.dll mpr.dll msimg32.dll msvcrt.dll ncrypt.dll netapi32.dll
    normaliz.dll ntdll.dll odbc32.dll ole32.dll oleacc.dll oleaut32.dll pdh.dll
    powrprof.dll propsys.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll
    shell32.dll shlwapi.dll ucrtbase.dll uiautomationcore.dll urlmon.dll user32.dll
    userenv.dll usp10.dll uxtheme.dll version.dll windowscodecs.dll winhttp.dll
    wininet.dll winmm.dll winspool.drv wintrust.dll winusb.dll ws2_32.dll
    wtsapi32.dll xmllite.dll'''.split())
MSVC_DLLS = set('''msvcp140.dll msvcp140_1.dll msvcp140_2.dll msvcp140_atomic_wait.dll
    msvcp140_codecvt_ids.dll vcruntime140.dll vcruntime140_1.dll concrt140.dll'''.split())
REDIST_URL = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
REQUIRED = (
    'komamimic.exe', 'komamimic-script-worker.exe', 'Qt6Core.dll', 'Qt6Gui.dll',
    'Qt6Widgets.dll', 'Qt6Sql.dll', 'Qt6PrintSupport.dll', 'Qt6WebEngineCore.dll',
    'Qt6WebEngineWidgets.dll', 'QtWebEngineProcess.exe', 'platforms/qwindows.dll',
    'sqldrivers/qsqlite.dll', 'imageformats/qtiff.dll', 'imageformats/qwebp.dll',
    'resources/qtwebengine_resources.pak', 'resources/qtwebengine_resources_100p.pak',
    'resources/qtwebengine_resources_200p.pak', 'resources/icudtl.dat',
    'resources/v8_context_snapshot.bin', 'translations/qtwebengine_locales/en-US.pak',
    'translations/qtwebengine_locales/ja.pak', 'translations/qtbase_ja.qm',
    'qt.conf', 'LICENSE', 'README.md', 'share/KomaMimic/readers/pic/decode.js')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def inventory(directory):
    records = []
    for path in sorted(directory.rglob('*')):
        if path.is_symlink():
            raise ValueError('Symlink in package: ' + str(path))
        if path.is_file():
            records.append(dict(path=path.relative_to(directory).as_posix(), sha256=digest(path)))
    # WindowsPath ordering folds case; ZIP manifests use a host-independent order.
    return sorted(records, key=lambda r: r['path'])


def relative_path(value):
    path = PurePosixPath(value)
    if (not value or path.is_absolute() or '..' in path.parts or str(path) != value
            or '\\' in value or ':' in value):
        raise ValueError('Unsafe package path: ' + value)
    return path


def pe_imports(data):
    """Read PE32+ normal and delay imports without executing or loading the PE."""
    def read(fmt, offset):
        try:
            return struct.unpack_from('<' + fmt, data, offset)
        except struct.error as error:
            raise ValueError('Truncated PE') from error
    if data[:2] != b'MZ':
        raise ValueError('Not a PE')
    header = read('I', 60)[0]
    if data[header:header + 6] != b'PE\0\0\x64\x86':
        raise ValueError('Expected x86_64 PE')
    count, optional_size = read('H', header + 6)[0], read('H', header + 20)[0]
    optional = header + 24
    if read('H', optional)[0] != 0x20b or optional_size < 240:
        raise ValueError('Expected PE32+ optional header')
    sections = [read('IIII', optional + optional_size + i * 40 + 8) for i in range(count)]

    def offset(rva):
        for _, address, size, raw in sections:
            if address <= rva < address + size:
                result = raw + rva - address
                if result < len(data):
                    return result
        raise ValueError('Invalid PE RVA: ' + str(rva))

    def name(rva):
        start = offset(rva)
        end = data.find(b'\0', start, start + 512)
        if end < 0:
            raise ValueError('Unterminated PE import')
        try:
            value = data[start:end].decode('ascii').lower()
        except UnicodeDecodeError as error:
            raise ValueError('Invalid PE import') from error
        if not re.fullmatch(r'[a-z0-9_.-]+', value):
            raise ValueError('Invalid PE import: ' + value)
        return value

    result = {}
    for kind, index, fmt, name_index in (
            ('imports', 1, 'IIIII', 3), ('delay_imports', 13, 'IIIIIIII', 1)):
        rva, length = read('II', optional + 112 + index * 8)
        names = []
        if rva:
            start, size = offset(rva), struct.calcsize('<' + fmt)
            if length < size:
                raise ValueError('Invalid PE import directory size')
            for pos in range(start, start + length, size):
                record = read(fmt, pos)
                if not any(record):
                    break
                if kind == 'delay_imports' and not record[0] & 1:
                    raise ValueError('Unsupported VA delay import')
                names.append(name(record[name_index]))
            else:
                raise ValueError('Unterminated PE import directory')
        result[kind] = sorted(set(names))
    return result


def audit(app):
    missing = [name for name in REQUIRED if not (app / name).is_file()]
    if missing:
        raise ValueError('Incomplete Windows payload: ' + ', '.join(missing))
    records, unresolved = {}, []
    runtime = set()
    # Plugins load dependencies from their own directory or the executable root.
    for path in sorted(app.rglob('*')):
        if path.suffix.lower() not in ('.dll', '.exe'):
            continue
        record = pe_imports(path.read_bytes())
        bundled = {p.name.lower() for parent in (app, path.parent) for p in parent.glob('*') if p.is_file()}
        external = set(record['imports'] + record['delay_imports']) - bundled
        record['msvc_prerequisites'] = sorted(external & MSVC_DLLS)
        runtime.update(record['msvc_prerequisites'])
        unknown = sorted(n for n in external - SYSTEM_DLLS - MSVC_DLLS
                         if not n.startswith(('api-ms-win-', 'ext-ms-win-')))
        record['unresolved'] = unknown
        relative = path.relative_to(app).as_posix()
        records[relative] = record
        unresolved.extend(relative + ': ' + n for n in unknown)
    if unresolved:
        raise ValueError('Unresolved non-system imports: ' + '; '.join(unresolved))
    return dict(architecture='x86_64', binaries=records, msvc_prerequisites=sorted(runtime))


def trim_plugins(app):
    # The application uses SQLite and does not use positioning or QML debugging.
    # SDK plugins for external DB clients/NMEA introduce unavailable dependencies.
    for group in ('position', 'qmltooling'):
        shutil.rmtree(app / group, ignore_errors=True)
    for path in (app / 'sqldrivers').glob('*'):
        if path.name.lower() != 'qsqlite.dll':
            if not path.is_file():
                raise ValueError('Unexpected SQL plugin entry: ' + str(path))
            path.unlink()


def collect_notices(prefix, sdk, destination):
    """Keep installed originals, plus SPDX metadata when present, without rewriting."""
    copied = []
    for source, label in ((prefix / 'share', 'vcpkg'), (sdk, 'qt')):
        for path in sorted(source.rglob('*')):
            name = path.name.lower()
            if path.is_file() and (name.startswith(('copyright', 'license', 'licence', 'notice'))
                                  or name == 'qt_attribution.json' or name.endswith(('.spdx', '.spdx.json'))
                                  or any(p.lower() in ('licenses', 'license', 'sbom')
                                         for p in path.relative_to(source).parts[:-1])):
                target = destination / label / path.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                copied.append(target.relative_to(destination).as_posix())
    if not any(n.startswith('vcpkg/') and '/copyright' in n for n in copied):
        raise ValueError('Installed vcpkg copyright files missing')
    return dict(files=copied, qt_notices_found=any(n.startswith('qt/') for n in copied),
                final_license_and_corresponding_source_review='PENDING')


def write_zip(directory, package):
    records = inventory(directory)
    with zipfile.ZipFile(package, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for record in records:
            # Fixed timestamps/modes; do not expose the staging filesystem's metadata.
            entry = zipfile.ZipInfo(directory.name + '/' + record['path'], (1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            with (directory / record['path']).open('rb') as source, archive.open(entry, 'w', force_zip64=True) as target:
                shutil.copyfileobj(source, target)


def extract_verified(package, destination):
    destination.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(package) as archive:
        names = archive.namelist()
        if len({n.casefold() for n in names}) != len(names):
            raise ValueError('Duplicate ZIP path')
        for entry in archive.infolist():
            relative_path(entry.filename)
            if entry.is_dir() or entry.external_attr >> 16 & 0o170000 == 0o120000:
                raise ValueError('Unexpected ZIP entry type')
        roots = {PurePosixPath(n).parts[0] for n in names}
        if len(roots) != 1:
            raise ValueError('Expected one ZIP root')
        archive.extractall(destination)
    app = destination / roots.pop()
    manifest = json.loads((app / 'BUILD_MANIFEST.json').read_text(encoding='utf-8'))
    actual = [r for r in inventory(app) if r['path'] != 'BUILD_MANIFEST.json']
    if actual != manifest['files']:
        raise ValueError('ZIP payload differs from BUILD_MANIFEST.json')
    if audit(app) != manifest['pe_dependencies']:
        raise ValueError('ZIP PE dependencies differ from manifest')
    return app


def native_verify(package, build, output):
    app = extract_verified(package, output / 'extracted')
    system_root = os.environ.get('SystemRoot')
    if not system_root:
        raise ValueError('Windows SystemRoot is missing')
    env = {k: v for k, v in os.environ.items()
           if not k.upper().startswith(('QT', 'QML', 'SDL_', 'SDL2COMPAT_', 'KOMA_PROBE_'))}
    env.update(PATH=os.pathsep.join((str(Path(system_root) / 'System32'), system_root)),
               QT_QPA_PLATFORM='offscreen')
    def run(name, command):
        record = execute(command, output / (name + '.log'), env=env, cwd=output, timeout=120)
        if record['returncode']:
            raise RuntimeError('Extracted ZIP check failed: ' + name)
    run('version', [app / 'komamimic.exe', '--version'])
    version = json.loads((app / 'BUILD_MANIFEST.json').read_text(encoding='utf-8'))['version']
    if not re.search(r'(?m)^KomaMimic ' + re.escape(version) + r'\s*$', (output / 'version.log').read_text(encoding='utf-8')):
        raise ValueError('Extracted executable version mismatch')
    run('formats', [app / 'komamimic.exe', '--formats'])
    fixtures = output / 'fixtures'
    run('worker', [sys.executable, ROOT / 'tests/fixtures/generate_validation_samples.py',
                   '--output', fixtures, '--verify-worker', app / 'komamimic-script-worker.exe'])
    if build is None:
        return  # Standalone smoke check; full pack always supplies its reader probe.
    # The probe is a test executable, added only after checking the shipped inventory.
    shutil.copy2(build / 'Release/validation_probe.exe', app / 'validation_probe.exe')
    result = subprocess.run([str(app / 'validation_probe.exe'), str(fixtures)],
                            env=env, cwd=output, timeout=120, capture_output=True,
                            text=True, encoding='utf-8', check=False)
    (output / 'native-readers.json').write_text(result.stdout, encoding='utf-8')
    (output / 'native-readers-stderr.log').write_text(result.stderr, encoding='utf-8')
    if result.returncode or not json.loads(result.stdout)['all_passed']:
        raise ValueError('Extracted ZIP native readers failed')


def pack(build, stage, output):
    if platform.system() != 'Windows':
        raise ValueError('Package and runtime checks require native Windows')
    build, stage, output = build.resolve(), stage.resolve(), output.resolve()
    cache = cache_values(build)
    if Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != ROOT or cache.get('KOMAMIMIC_WINDOWS_FAST') != 'ON':
        raise ValueError('Use this source build with the official prebuilt Qt SDK')
    info = json.loads((stage / 'STAGE_MANIFEST.json').read_text(encoding='utf-8'))
    if info['system'] != 'Windows' or info['status'] != 'STAGED_NOT_MANUALLY_VERIFIED':
        raise ValueError('Windows stage did not succeed')
    for record in info['files']:
        relative_path(record['path'])
        path = stage / record['path']
        if not path.is_file() or digest(path) != record['sha256']:
            raise ValueError('Stage changed: ' + record['path'])
    app_files = [dict(path='app/' + r['path'], sha256=r['sha256']) for r in inventory(stage / 'app')]
    if app_files != sorted((r for r in info['files'] if r['path'].startswith('app/')), key=lambda r: r['path']):
        raise ValueError('Stage app inventory changed')
    source_files = [dict(path=p.relative_to(ROOT).as_posix(), sha256=digest(p)) for p in public_files(ROOT)]
    if json.loads((stage / 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))['files'] != source_files:
        raise ValueError('Stage belongs to different source contents')
    if digest(stage / 'SOURCE_MANIFEST.json') != info['source_manifest_sha256']:
        raise ValueError('Stage source manifest changed')
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))[1]
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=all'], text=True).strip())
    output.mkdir(parents=True, exist_ok=False)
    work = output / 'work'
    app = work / f'KomaMimic-{version}-windows-x86_64'
    shutil.copytree(stage / 'app', app)
    symbols = work / (app.name + '-symbols')
    symbols.mkdir()
    for path in app.glob('*.pdb'):
        shutil.move(path, symbols / path.name)
    trim_plugins(app)
    prefix, qtbin = windows_paths(build)
    notices = collect_notices(prefix, qtbin.parent, app / 'licenses')
    for name in ('QT_SDK.json', 'vcpkg-status.txt', 'SOURCE_MANIFEST.json'):
        shutil.copy2(stage / name, app / name)
    (app / '配布情報.txt').write_text(
        f'KomaMimic {version} / Windows x86_64\n\n'
        'Microsoft Visual C++ v14 x64 再頒布可能パッケージが必要です。\n'
        '公式配布先: ' + REDIST_URL + '\n'
        'MSVCランタイムはこのZIPに含めていません。Qt SDKやvcpkgの導入は不要です。\n\n'
        '第三者通知の原文はlicenses/、本体のライセンス・通知はLICENSEとREADME.mdを参照してください。\n'
        'このActions成果物は取得物の確認用です。最終公開前の依存通知・対応ソース照合は別途行います。\n', encoding='utf-8-sig')
    manifest = dict(schema=1, kind='windows-distribution-candidate', version=version,
                    commit=commit, source_tree_dirty=dirty, source_files=source_files,
                    manual_verification='PENDING', notices=notices, msvc_redist_url=REDIST_URL,
                    pe_dependencies=audit(app), files=inventory(app))
    save_json(app / 'BUILD_MANIFEST.json', manifest)
    save_json(output / 'BUILD_MANIFEST.json', manifest)
    package = output / (app.name + '.zip')
    write_zip(app, package)
    # Reopen and execute the ZIP itself with SDK/vcpkg paths removed.
    native_verify(package, build, output / 'verification')
    if any(symbols.iterdir()):
        save_json(symbols / 'BUILD_INFO.json', dict(version=version, commit=commit, source_tree_dirty=dirty,
                                                  executable_sha256={n: digest(app / n) for n in ('komamimic.exe', 'komamimic-script-worker.exe')}))
        symbol_output = output / 'symbols'
        symbol_output.mkdir()
        symbol_package = symbol_output / (symbols.name + '.zip')
        write_zip(symbols, symbol_package)
        (symbol_output / 'SHA256SUMS').write_text(digest(symbol_package) + '  ' + symbol_package.name + '\n', encoding='ascii')
    prepare(ROOT, output / 'sources', 0)
    packages = sorted(output.glob('*.zip')) + [output / 'BUILD_MANIFEST.json']
    (output / 'SHA256SUMS').write_text(''.join(digest(p) + '  ' + p.name + '\n' for p in packages), encoding='ascii')
    print('Windows distribution candidate verified: ' + str(package))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('audit')
    p.add_argument('--app', type=Path, required=True)
    p = commands.add_parser('pack')
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--stage', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = commands.add_parser('verify')
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--build-dir', type=Path, help='also run this build\'s native reader probe')
    p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'audit':
            print(json.dumps(audit(args.app.resolve()), indent=2))
        elif args.command == 'verify':
            if platform.system() != 'Windows':
                raise ValueError('Runtime verification requires native Windows')
            native_verify(args.package.resolve(), args.build_dir.resolve() if args.build_dir else None,
                          args.output.resolve())
            print('Extracted ZIP runtime verification passed' + (' (including reader probe)' if args.build_dir else ' (CLI and worker)'))
        else:
            pack(args.build_dir, args.stage, args.output)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
