#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create and verify an arm64 .app archive and unnotarized DMG on macOS.

Outputs are Actions candidates. Final dependency notices/corresponding-source
review and downloaded-artifact GUI verification remain separate release gates.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys

from export_source import public_files
from prepare_release import prepare
from run_validation import ROOT, cache_values, execute, save_json
from stage_validation import (inspect_macos, macos_bundle_or_system_path,
                              macos_expand_path as expand_path, macos_probe_environment,
                              signing_entitlements, system_library)

RUNTIME_FORMULAE = ('qtbase', 'qtdeclarative', 'qttools', 'qttranslations', 'qtwebengine',
                   'qtimageformats', 'libarchive', 'icu4c', 'sdl2-compat', 'sdl3', 'poppler')
REQUIRED = ('Contents/MacOS/komamimic', 'Contents/MacOS/komamimic-script-worker',
            'Contents/Info.plist', 'Contents/Resources/komamimic.icns',
            'Contents/Resources/qt.conf', 'Contents/Resources/translations/qtbase_ja.qm',
            'Contents/Resources/readers/pic/decode.js', 'Contents/Resources/LICENSE',
            'Contents/Resources/README.md', 'Contents/PlugIns/platforms/libqcocoa.dylib',
            'Contents/PlugIns/imageformats/libqtiff.dylib', 'Contents/PlugIns/imageformats/libqwebp.dylib',
            'Contents/PlugIns/sqldrivers/libqsqlite.dylib', 'Contents/Frameworks/libSDL3.dylib')


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def inventory(app):
    """Preserve framework links and executable modes; reject escaping links."""
    app = app.resolve()
    records, names = [], set()
    for path in sorted(app.rglob('*')):
        relative = path.relative_to(app).as_posix()
        if relative.casefold() in names:
            raise ValueError('Case-colliding bundle path: ' + relative)
        names.add(relative.casefold())
        if path.is_symlink():
            target = os.readlink(path)
            if Path(target).is_absolute() or not path.exists() or not path.resolve().is_relative_to(app):
                raise ValueError('Unsafe bundle symlink: ' + relative)
            records.append(dict(path=relative, link=target))
        elif path.is_file():
            records.append(dict(path=relative, sha256=digest(path), mode=stat.S_IMODE(path.stat().st_mode)))
        elif not path.is_dir():
            raise ValueError('Unexpected bundle file type: ' + relative)
    return records


def audit_dependencies(app, records):
    """Resolve every normal/weak/reexport load, including @rpath, inside the app."""
    app = app.resolve()
    for binary, record in records.items():
        rpaths = []
        for value in record.get('rpaths', []):
            path = expand_path(value, app, binary)
            if not macos_bundle_or_system_path(path, app):
                raise ValueError('External or unsupported rpath: ' + value + ' in ' + str(binary))
            rpaths.append(path)
        for name in record['dependencies']:
            if system_library(name):
                continue
            candidates = ([p / name[len('@rpath/'):] for p in rpaths] if name.startswith('@rpath/')
                          else [expand_path(name, app, binary)])
            if not any(p is not None and (system_library(str(p)) or
                       (p.resolve().is_relative_to(app) and p.resolve() in records)) for p in candidates):
                raise ValueError('Unresolved bundled load: ' + name + ' in ' + str(binary))


def audit(app, output, call):
    inventory(app)
    missing = [name for name in REQUIRED if not (app / name).is_file()]
    for pattern in ('**/QtWebEngineProcess', '**/qtwebengine_resources.pak', '**/icudtl.dat',
                    '**/qtwebengine_locales/en-US.pak', '**/qtwebengine_locales/ja.pak'):
        if not any(p.is_file() for p in app.glob(pattern)):
            missing.append(pattern)
    if missing:
        raise ValueError('Incomplete macOS bundle: ' + ', '.join(missing))
    records = inspect_macos(app, output / 'otool.txt')
    if not records:
        raise ValueError('No Mach-O binaries in app')
    audit_dependencies(app, records)
    report = {}
    for binary, record in sorted(records.items()):
        text = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).strip()
        arches = text.split()
        if 'arm64' not in arches:
            raise ValueError('Missing arm64 slice: ' + str(binary))
        if binary.parent == app / 'Contents/MacOS' and arches != ['arm64']:
            raise ValueError('Expected native arm64 application executable: ' + str(binary))
        report[binary.relative_to(app).as_posix()] = dict(record, architectures=arches)
    call('codesign-verify', ['codesign', '--verify', '--deep', '--strict', '--verbose=2', app])
    save_json(output / 'mach-o.json', report)
    return report


def collect_notices(destination, output):
    metadata = json.loads(subprocess.check_output(['brew', 'info', '--json=v2', '--installed'], text=True))
    save_json(output / 'HOMEBREW.json', metadata)
    formulae = metadata['formulae']
    by_name = {}
    for formula in formulae:
        for name in [formula['name'], formula.get('full_name', ''), *formula.get('aliases', [])]:
            by_name[name] = formula
    selected, pending = {}, list(RUNTIME_FORMULAE)
    while pending:
        name = pending.pop()
        if name not in by_name:
            raise ValueError('Missing installed runtime formula metadata: ' + name)
        formula = by_name[name]
        if formula['name'] in selected:
            continue
        selected[formula['name']] = formula
        pending.extend(formula.get('dependencies', []))
    names = sorted(selected)
    prefixes = subprocess.check_output(['brew', '--prefix', *names], text=True).splitlines()
    if len(prefixes) != len(names):
        raise ValueError('Unexpected Homebrew prefix list')
    files = []
    for name, prefix in zip(names, prefixes):
        source = Path(prefix).resolve(strict=True)
        if not re.fullmatch(r'[A-Za-z0-9@+_.-]+', name):
            raise ValueError('Unsafe formula name: ' + name)
        for path in sorted(source.rglob('*')):
            relative = path.relative_to(source)
            parts = [p.lower() for p in relative.parts]
            filename = path.name.lower()
            legal = (filename.startswith(('license', 'licence', 'copying', 'copyright', 'notice'))
                     or 'license' in filename or filename == 'qt_attribution.json'
                     or any(p in ('licenses', 'licences', 'sbom') for p in parts[:-1])
                     or filename == 'install_receipt.json' or '.brew' in parts[:-1])
            if legal and path.is_file() and path.resolve().is_relative_to(source):
                target = destination / name / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                files.append(target.relative_to(destination).as_posix())
    if not files:
        raise ValueError('Homebrew dependency notices missing')
    return dict(formulae={n: selected[n] for n in names}, files=files,
                final_license_and_corresponding_source_review='PENDING')


def verified_stage(build, stage):
    cache = cache_values(build)
    if (Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != ROOT
            or cache.get('CMAKE_OSX_ARCHITECTURES') != 'arm64'
            or cache.get('CMAKE_BUILD_TYPE') != 'Release'):
        raise ValueError('Use this source native arm64 Release build')
    info = json.loads((stage / 'STAGE_MANIFEST.json').read_text())
    if info['system'] != 'Darwin' or info['status'] != 'STAGED_NOT_MANUALLY_VERIFIED':
        raise ValueError('macOS stage did not succeed')
    for record in info['files']:
        relative = Path(record['path'])
        path = stage / relative
        if relative.is_absolute() or '..' in relative.parts or not path.resolve().is_relative_to(stage):
            raise ValueError('Unsafe stage path')
        if not path.is_file() or digest(path) != record['sha256']:
            raise ValueError('Stage changed: ' + record['path'])
    app = stage / 'KomaMimic.app'
    actual = {r['path']: r['sha256'] for r in inventory(app) if 'sha256' in r}
    expected = {r['path'][len('KomaMimic.app/'):]: r['sha256'] for r in info['files']
                if r['path'].startswith('KomaMimic.app/')}
    if actual != expected:
        raise ValueError('Stage app inventory changed')
    links = {p.relative_to(stage).as_posix(): os.readlink(p)
             for p in stage.rglob('*') if p.is_symlink()}
    if links != info.get('symlinks'):
        raise ValueError('Stage symlinks changed or were not recorded')
    records = [dict(path=p.relative_to(ROOT).as_posix(), sha256=digest(p)) for p in public_files(ROOT)]
    if (digest(stage / 'SOURCE_MANIFEST.json') != info['source_manifest_sha256']
            or json.loads((stage / 'SOURCE_MANIFEST.json').read_text())['files'] != records):
        raise ValueError('Stage belongs to different source contents')
    if not (stage / 'validation_probe').is_file():
        raise ValueError('Stage the deployed validation probe with this build')
    return app, records


def native_verify(app, probe, output, call, expected_version):
    env = macos_probe_environment()
    # The deployed bundle contains cocoa, not the SDK's offscreen plugin.
    env.update(QT_QPA_PLATFORM='cocoa', QT_LOGGING_TO_CONSOLE='1', DYLD_PRINT_LIBRARIES='1')
    binary = app / 'Contents/MacOS/komamimic'
    config = output / 'settings.ini'
    call('version', [binary, '--config', config, '--version'], env=env, cwd=output, timeout=60)
    if not re.search(r'(?m)^KomaMimic ' + re.escape(expected_version) + r'\s*$',
                     (output / 'version.log').read_text()):
        raise ValueError('Deployed executable version mismatch')
    call('formats', [binary, '--config', config, '--formats'], env=env, cwd=output, timeout=60)
    fixtures = output / 'fixtures'
    worker_env = dict(env)
    # The fixture harness is the runner's Python, not a shipped executable.
    worker_env.pop('DYLD_PRINT_LIBRARIES', None)
    call('worker', [sys.executable, ROOT / 'tests/fixtures/generate_validation_samples.py',
                   '--output', fixtures, '--verify-worker', app / 'Contents/MacOS/komamimic-script-worker'],
         env=worker_env, cwd=output, timeout=120)
    test_probe = app / 'Contents/MacOS/validation_probe'
    shutil.copy2(probe, test_probe)
    try:
        with (output / 'native-readers.json').open('w') as stdout, (output / 'native-readers-stderr.log').open('w') as stderr:
            result = subprocess.run([str(test_probe), str(fixtures)], env=env, cwd=output,
                                    stdout=stdout, stderr=stderr, check=False, timeout=120)
        if result.returncode or not json.loads((output / 'native-readers.json').read_text())['all_passed']:
            raise ValueError('Installed app native readers failed')
    finally:
        test_probe.unlink()
    # PATH isolation alone does not prevent dyld from loading an absolute SDK path.
    for path in output.glob('*.log'):
        text = path.read_text(errors='replace')
        if re.search(r'dyld\[\d+\]:[^\n]*(?:/opt/homebrew/|/usr/local/)', text):
            raise ValueError('Runtime loaded a Homebrew library: ' + path.name)


def pack(build, stage, output):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise ValueError('DMG packaging requires native macOS arm64')
    build, stage, output = build.resolve(), stage.resolve(), output.resolve()
    staged_app, source_files = verified_stage(build, stage)
    output.mkdir(parents=True, exist_ok=False)
    diagnostics = output / 'verification'
    diagnostics.mkdir()
    def call(name, command, **kwargs):
        record = execute(command, diagnostics / (name + '.log'), timeout=kwargs.pop('timeout', 180), **kwargs)
        if record['returncode']:
            raise RuntimeError(name + ' failed; see ' + str(diagnostics))
    volume = output / 'work/dmg-root'
    volume.mkdir(parents=True)
    app = volume / 'KomaMimic.app'
    call('copy-app', ['ditto', staged_app, app])
    if inventory(app) != inventory(staged_app):
        raise ValueError('App copy changed payload or executable modes')
    with (app / 'Contents/Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text())[1]
    if info.get('CFBundleShortVersionString') != version or info.get('CFBundleExecutable') != 'komamimic':
        raise ValueError('Staged app identity/version mismatch')
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=all'], text=True).strip())
    notices = collect_notices(app / 'Contents/Resources/licenses/homebrew', output)
    save_json(app / 'Contents/Resources/BUILD_INFO.json', dict(version=version, commit=commit, source_tree_dirty=dirty,
              signing='ad-hoc', notarization='NOT_PERFORMED', manual_verification='PENDING'))
    entitlements = signing_entitlements(app)
    command = ['codesign', '--force', '--sign', '-', '--preserve-metadata=identifier,flags,runtime']
    if entitlements:
        entitlement_file = diagnostics / 'app-entitlements.plist'
        entitlement_file.write_bytes(plistlib.dumps(entitlements))
        command += ['--entitlements', entitlement_file]
    call('codesign-app', [*command, app])
    if signing_entitlements(app) != entitlements:
        raise ValueError('App entitlements changed')
    macho_report = audit(app, diagnostics, call)
    manifest = dict(schema=1, kind='macos-distribution-candidate', version=version, commit=commit,
                    source_tree_dirty=dirty, signing='ad-hoc', notarization='NOT_PERFORMED',
                    manual_verification='PENDING', notices=notices, source_files=source_files,
                    mach_o=macho_report, files=inventory(app))
    save_json(output / 'BUILD_MANIFEST.json', manifest)
    shutil.copy2(output / 'BUILD_MANIFEST.json', volume)
    (volume / '配布情報.txt').write_text(
        f'KomaMimic {version} / macOS arm64\n\n'
        'KomaMimic.appをApplicationsへコピーして使います。\n'
        'この.appはad-hoc署名済みですが、Developer ID署名とAppleの公証は行っていません。\n'
        'ブラウザー取得後のFinder起動・OSの警告は取得物で別途確認します。\n'
        '依存通知の原文は.app内Contents/Resources/licenses/にあります。\n'
        '正式公開前の全依存通知・必要な対応ソース照合は別途行います。\n', encoding='utf-8')
    (volume / 'Applications').symlink_to('/Applications', target_is_directory=True)
    archive = output / f'KomaMimic-{version}-macos-arm64.app.zip'
    call('archive-app', ['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, archive])
    extracted = diagnostics / 'archive'
    extracted.mkdir()
    call('extract-app', ['ditto', '-x', '-k', archive, extracted])
    if inventory(extracted / app.name) != manifest['files']:
        raise ValueError('App archive changed payload, links or executable modes')
    call('archive-signature', ['codesign', '--verify', '--deep', '--strict', extracted / app.name])
    image = output / f'KomaMimic-{version}-macos-arm64.dmg'
    call('create-dmg', ['hdiutil', 'create', '-volname', 'KomaMimic', '-srcfolder', volume,
                        '-fs', 'HFS+', '-format', 'UDZO', image], timeout=300)
    call('verify-dmg', ['hdiutil', 'verify', image])
    mount = diagnostics / 'mount'
    mount.mkdir()
    mounted = False
    try:
        call('attach-dmg', ['hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen',
                           '-mountpoint', mount, image])
        mounted = True
        if os.readlink(mount / 'Applications') != '/Applications':
            raise ValueError('DMG Applications link changed')
        if inventory(mount / app.name) != manifest['files']:
            raise ValueError('Mounted DMG payload differs from manifest')
        if (mount / 'BUILD_MANIFEST.json').read_bytes() != (output / 'BUILD_MANIFEST.json').read_bytes():
            raise ValueError('Mounted DMG manifest changed')
        call('mounted-signature', ['codesign', '--verify', '--deep', '--strict', mount / app.name])
        installed = diagnostics / 'installed/KomaMimic.app'
        installed.parent.mkdir()
        call('install-copy', ['ditto', mount / app.name, installed])
        if inventory(installed) != manifest['files']:
            raise ValueError('Installed DMG app changed')
        native_verify(installed, stage / 'validation_probe', diagnostics, call, version)
        if inventory(installed) != manifest['files']:
            raise ValueError('Runtime verification modified installed app')
        call('installed-signature', ['codesign', '--verify', '--deep', '--strict', installed])
    finally:
        if mounted or os.path.ismount(mount):
            call('detach-dmg', ['hdiutil', 'detach', mount])
    prepare(ROOT, output / 'sources', 0)
    (output / 'SHA256SUMS').write_text(''.join(digest(p) + '  ' + p.name + '\n'
        for p in (archive, image, output / 'BUILD_MANIFEST.json', output / 'HOMEBREW.json')), encoding='ascii')
    print('Verified macOS arm64 app and unnotarized DMG: ' + str(image))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        pack(args.build_dir, args.stage, args.output)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
