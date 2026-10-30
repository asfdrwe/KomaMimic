#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage local Windows DLLs or an ad-hoc signed macOS .app for manual testing.

Not a release packager: dependency source collection and public distribution are
separate work. Run this script natively after run_validation.py on that OS.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import zipfile

from run_validation import ROOT, cache_values, execute, runtime_environment, save_json, windows_paths
from export_source import public_files


def tool(directory, *names):
    for name in names:
        path = directory / name
        if path.is_file():
            return path
    raise ValueError('Required Qt tool missing in ' + str(directory) + ': ' + ', '.join(names))


def required(paths):
    missing = [str(p) for p in paths if not p.exists()]
    if missing:
        raise ValueError('Incomplete stage: ' + ', '.join(missing))


def japanese_qt_translation(qtpaths, destination, roots=(), env=None):
    queried = subprocess.check_output([str(qtpaths), '--query', 'QT_INSTALL_TRANSLATIONS'],
                                      env=env, text=True).strip()
    candidates = [Path(queried), *roots]
    source = next((root / 'qtbase_ja.qm' for root in candidates if (root / 'qtbase_ja.qm').is_file()), None)
    if source is None:
        raise ValueError('qtbase_ja.qm missing; install matching qttranslations')
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination / source.name)


def windows(build, output, call):
    prefix, qt = windows_paths(build)
    env = runtime_environment(build)
    app = output / 'app'
    app.mkdir()
    for name in ('komamimic.exe', 'komamimic-script-worker.exe'):
        shutil.copy2(build / 'Release' / name, app / name)
        pdb = build / 'Release' / (Path(name).stem + '.pdb')
        if pdb.is_file():
            shutil.copy2(pdb, app / pdb.name)
    dlls = list((prefix / 'bin').glob('*.dll'))
    if not dlls:
        raise ValueError('No Release runtime DLLs in ' + str(prefix / 'bin'))
    for path in dlls:
        shutil.copy2(path, app / path.name)
    deploy = tool(qt, 'windeployqt.exe', 'windeployqt6.exe')
    for executable in ('komamimic.exe', 'komamimic-script-worker.exe'):
        call(executable, [deploy, '--release', '--no-compiler-runtime', '--dir', app,
                         '--add-plugin-types', 'imageformats,sqldrivers', app / executable], env)
    japanese_qt_translation(tool(qt, 'qtpaths.exe', 'qtpaths6.exe'), app / 'translations',
        [qt.parent / 'translations', prefix / 'translations', prefix / 'share/Qt6/translations', prefix / 'share/qt/translations'], env)
    (app / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n', encoding='utf-8')
    shutil.copytree(ROOT / 'scripts/readers', app / 'share/KomaMimic/readers')
    for name in ('LICENSE', 'README.md'):
        shutil.copy2(ROOT / name, app / name)
    required([app / 'Qt6Core.dll', app / 'Qt6WebEngineCore.dll', app / 'QtWebEngineProcess.exe',
              app / 'platforms/qwindows.dll', app / 'sqldrivers/qsqlite.dll',
              app / 'resources/qtwebengine_resources.pak', app / 'translations/qtwebengine_locales/en-US.pak'])
    # Keep a snapshot of actual installed dependency versions, not just the baseline.
    status = prefix.parent / 'vcpkg/status'
    if status.is_file():
        shutil.copy2(status, output / 'vcpkg-status.txt')
    identity = qt.parent / 'KOMAMIMIC_QT_SDK.json'
    if identity.is_file():
        shutil.copy2(identity, output / 'QT_SDK.json')
    return app / 'komamimic.exe'


def macho(path):
    try:
        with path.open('rb') as stream:
            return stream.read(4) in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf',
                                      b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf')
    except OSError:
        return False


def macho_commands(text):
    """Separate LC_ID_DYLIB (this library's name) from actual load commands."""
    identifiers, dependencies, rpaths = set(), set(), []
    command = ''
    loads = {'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
             'LC_LOAD_UPWARD_DYLIB', 'LC_LAZY_LOAD_DYLIB'}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('cmd '):
            command = line.split()[1]
        elif line.startswith('name ') and command in loads | {'LC_ID_DYLIB'}:
            name = line[5:].rsplit(' (offset ', 1)[0]
            (identifiers if command == 'LC_ID_DYLIB' else dependencies).add(name)
        elif line.startswith('path ') and command == 'LC_RPATH':
            value = line[5:].rsplit(' (offset ', 1)[0]
            if value not in rpaths:  # otool repeats commands for universal slices.
                rpaths.append(value)
    if len(identifiers) > 1:
        raise ValueError('Different install names across Mach-O architectures')
    return dict(identifier=next(iter(identifiers), ''), dependencies=sorted(dependencies), rpaths=rpaths)


def system_library(name):
    return name in ('/usr/lib', '/System/Library') or name.startswith(('/usr/lib/', '/System/Library/'))


def inspect_macos(app, report):
    """Read real binaries once, refusing links outside the staged app."""
    app = app.resolve()
    records = {}
    with report.open('w', encoding='utf-8') as stream:
        for path in sorted((app / 'Contents').rglob('*')):
            if not path.is_file() or not macho(path):
                continue
            path = path.resolve()
            if not path.is_relative_to(app):
                raise ValueError('Mach-O symlink escapes app: ' + str(path))
            if path in records:
                continue
            text = subprocess.check_output(['otool', '-l', str(path)], text=True)
            stream.write(text + '\n')
            records[path] = macho_commands(text)
    return records


def macos_relocation_plan(app, records):
    """Only reconnect dependencies to matching binaries already in this app."""
    frameworks = app.resolve() / 'Contents/Frameworks'
    identifiers = {}
    for path, record in records.items():
        if record['identifier']:
            identifiers.setdefault(record['identifier'], set()).add(path)
    changes = []
    for path, record in records.items():
        args = []
        identifier = record['identifier']
        if identifier.startswith('/') and not system_library(identifier):
            if not path.is_relative_to(frameworks):
                raise ValueError('Unexpected library outside Frameworks: ' + str(path))
            args += ['-id', '@rpath/' + path.relative_to(frameworks).as_posix()]
        for name in record['dependencies']:
            if not name.startswith('/') or system_library(name):
                continue
            candidates = set(identifiers.get(name, ()))
            # macdeployqt may already have changed the target's install name.
            # Framework paths retain the full versioned suffix; dylibs must be
            # direct children of Frameworks. Never search arbitrary basenames.
            match = re.search(r'(?:^|/)([^/]+\.framework/.*)$', name)
            target = frameworks / (match.group(1) if match else Path(name).name)
            if target.is_file() and target.resolve() in records:
                candidates.add(target.resolve())
            candidates = {p for p in candidates if p.is_relative_to(frameworks)}
            if len(candidates) != 1:
                raise ValueError('No unique bundled target for ' + name + ' in ' + str(path))
            target = candidates.pop()
            replacement = '@loader_path/' + Path(os.path.relpath(target, path.parent)).as_posix()
            args += ['-change', name, replacement]
        # Do not let a missing bundled dependency fall back to the build SDK.
        for rpath in dict.fromkeys(record.get('rpaths', [])):
            if rpath.startswith('/') and not system_library(rpath):
                args += ['-delete_rpath', rpath]
        if any(name.startswith('@rpath/') for name in record['dependencies']):
            rpath = '@loader_path/' + Path(os.path.relpath(frameworks, path.parent)).as_posix()
            if rpath not in record.get('rpaths', []):
                args += ['-add_rpath', rpath]
        if args:
            changes.append(dict(binary=path.relative_to(app.resolve()).as_posix(), arguments=args))
    return changes


def signing_entitlements(path):
    result = subprocess.run(['codesign', '--display', '--entitlements', ':-', str(path)],
                            check=True, capture_output=True)
    data = result.stdout.strip()
    entitlements = plistlib.loads(data) if data else {}
    if not isinstance(entitlements, dict):
        raise ValueError('Invalid entitlements for ' + str(path))
    return entitlements


def relocate_macos(app, output, call, records):
    changes = macos_relocation_plan(app, records)  # Preflight all targets before editing.
    save_json(output / 'macos-relocations.json', changes)
    signing = set()
    for change in changes:
        path = app / change['binary']
        signing.add(path)
        for parent in path.parents:
            if parent == app:
                break  # The outer app is sealed after updating qt.conf.
            if parent.suffix in ('.app', '.framework', '.xpc'):
                signing.add(parent)
    signing = sorted(signing, key=lambda p: (-len(p.parts), str(p)))
    # Capture before install_name_tool can invalidate or replace signatures.
    saved = {path: signing_entitlements(path) for path in signing}
    for i, change in enumerate(changes):
        call(f'relocate-{i:03}', ['install_name_tool', *change['arguments'], app / change['binary']])
    entitlements_dir = output / 'entitlements-before'
    for i, path in enumerate(signing):
        command = ['codesign', '--force', '--sign', '-', '--preserve-metadata=identifier,flags,runtime']
        if saved[path]:
            entitlements_dir.mkdir(exist_ok=True)
            entitlement_file = entitlements_dir / f'{i:03}.plist'
            entitlement_file.write_bytes(plistlib.dumps(saved[path]))
            command += ['--entitlements', entitlement_file]
        call(f'codesign-relocated-{i:03}', [*command, path])
    for path in signing:
        if signing_entitlements(path) != saved[path]:
            raise ValueError('Entitlements changed while re-signing ' + str(path))


def external_macos_dependencies(app, records):
    unresolved = []
    for path, record in records.items():
        for name in record['dependencies']:
            if name.startswith('/') and not system_library(name):
                unresolved.append(dict(binary=path.relative_to(app).as_posix(), dependency=name))
            elif name.startswith('@loader_path/'):
                target = (path.parent / name[len('@loader_path/'):]).resolve()
                if not target.is_relative_to(app) or target not in records:
                    unresolved.append(dict(binary=path.relative_to(app).as_posix(), dependency=name))
    return unresolved


def bundle_macos_sdl3(app, output):
    # Homebrew's sdl2-compat dlopens this library before main(). It is not
    # represented by LC_LOAD_DYLIB, so macdeployqt cannot discover it unaided.
    prefix = Path(subprocess.check_output(['brew', '--prefix', 'sdl3'], text=True).strip())
    source = prefix / 'lib/libSDL3.dylib'
    if not source.is_file() or not macho(source):
        raise ValueError('Homebrew SDL3 runtime missing: ' + str(source))
    frameworks = app / 'Contents/Frameworks'
    frameworks.mkdir(parents=True, exist_ok=True)
    target = frameworks / 'libSDL3.0.dylib'
    alias = frameworks / 'libSDL3.dylib'
    if target.exists() or alias.exists() or alias.is_symlink():
        raise ValueError('SDL3 destination already exists in stage')
    shutil.copy2(source.resolve(), target)
    # sdl2-compat tries @loader_path/libSDL3.dylib from the deployed SDL2
    # library in Frameworks. Keep the unversioned name as an internal link.
    alias.symlink_to(target.name)
    save_json(output / 'SDL3_RUNTIME.json', dict(
        source=str(source.resolve()), sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        binary=target.relative_to(app).as_posix(), alias=alias.relative_to(app).as_posix()))
    return target


def macos_probe_environment():
    # Exercise the bundle without Qt/SDL overrides from the developer shell.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('DYLD_', 'QT', 'QML', 'SDL_', 'SDL2COMPAT_', 'KOMA_PROBE_'))}
    env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
    return env


def macos(build, output, call):
    if platform.machine() != 'arm64':
        raise ValueError('Use native Apple Silicon, not Rosetta/x64')
    prefix = Path(subprocess.check_output(['brew', '--prefix', 'qtbase'], text=True).strip())
    qt = prefix / 'bin'
    app = output / 'KomaMimic.app'
    contents = app / 'Contents'
    binaries, resources, plugins = contents / 'MacOS', contents / 'Resources', contents / 'PlugIns'
    binaries.mkdir(parents=True)
    resources.mkdir()
    for name in ('komamimic', 'komamimic-script-worker'):
        shutil.copy2(build / name, binaries / name)
    # Deploy the test probe in the same pass, then keep it outside the sealed app.
    # Copying an undeployed build probe later would retain Homebrew load paths.
    probe = binaries / 'validation_probe'
    if (build / probe.name).is_file():
        shutil.copy2(build / probe.name, probe)
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    with (contents / 'Info.plist').open('wb') as stream:
        plistlib.dump(dict(CFBundleExecutable='komamimic', CFBundleName='KomaMimic',
                          CFBundleIdentifier='org.komamimic.viewer', CFBundlePackageType='APPL',
                          CFBundleShortVersionString=version, CFBundleVersion=version,
                          CFBundleIconFile='komamimic.icns', NSHighResolutionCapable=True), stream)
    shutil.copy2(ROOT / 'assets/icons/komamimic.icns', resources)
    shutil.copytree(ROOT / 'scripts/readers', resources / 'readers')
    for name in ('LICENSE', 'README.md'):
        shutil.copy2(ROOT / name, resources / name)
    qtpaths = tool(qt, 'qtpaths', 'qtpaths6')
    translationprefix = Path(subprocess.check_output(['brew', '--prefix', 'qttranslations'], text=True).strip())
    japanese_qt_translation(qtpaths, resources / 'translations',
        [translationprefix / 'share/qt/translations', translationprefix / 'translations'])
    plugin_roots = [Path(subprocess.check_output([str(qtpaths), '--query', 'QT_INSTALL_PLUGINS'], text=True).strip())]
    imageprefix = Path(subprocess.check_output(['brew', '--prefix', 'qtimageformats'], text=True).strip())
    plugin_roots += [imageprefix / 'share/qt/plugins', imageprefix / 'plugins', imageprefix / 'lib/qt6/plugins']
    for group in ('imageformats', 'sqldrivers'):
        for root in plugin_roots:
            directory = root / group
            if directory.is_dir():
                shutil.copytree(directory, plugins / group, dirs_exist_ok=True)
    sdl3 = bundle_macos_sdl3(app, output)
    command = [tool(qt, 'macdeployqt', 'macdeployqt6'), app, '-verbose=2', '-always-overwrite',
               '-codesign=-', '-executable=' + str(binaries / 'komamimic-script-worker'),
               '-executable=' + str(sdl3)]
    if probe.is_file():
        command.append('-executable=' + str(probe))
    call('macdeployqt', command)
    required([sdl3, sdl3.parent / 'libSDL3.dylib'])
    call('codesign-sdl3', ['codesign', '--force', '--sign', '-', sdl3])
    # Split Homebrew Qt modules can leave absolute references in the nested
    # WebEngine helper. Repair only links to deployed binaries and preserve its
    # entitlements while signing changed code from the inside out.
    records = inspect_macos(app, output / 'otool-before-relocation.txt')
    relocate_macos(app, output, call, records)
    if probe.is_file():
        shutil.move(probe, output / probe.name)
        call('codesign-probe', ['codesign', '--force', '--sign', '-', output / probe.name])
    # macdeployqt writes plugin paths but does not collect Qt's .qm catalogs.
    # Set the catalog path after deployment, then re-seal the outer bundle.
    qtconf = resources / 'qt.conf'
    config = configparser.ConfigParser()
    config.optionxform = str
    config.read(qtconf, encoding='utf-8')
    if not config.has_section('Paths'):
        config.add_section('Paths')
    config.set('Paths', 'Translations', 'Resources/translations')
    with qtconf.open('w', encoding='utf-8') as stream:
        config.write(stream)
    call('codesign-reseal', ['codesign', '--force', '--sign', '-', app])
    required([plugins / 'platforms/libqcocoa.dylib', plugins / 'sqldrivers/libqsqlite.dylib',
              contents / 'Frameworks/QtWebEngineCore.framework', resources / 'komamimic.icns'])
    helpers = list(contents.rglob('QtWebEngineProcess'))
    if not any(p.is_file() for p in helpers):
        raise ValueError('QtWebEngineProcess helper was not deployed')
    if not list(contents.rglob('qtwebengine_resources.pak')) or not list(contents.rglob('en-US.pak')):
        raise ValueError('WebEngine resources or locales were not deployed')
    records = inspect_macos(app, output / 'otool-dependencies.txt')
    unresolved = external_macos_dependencies(app, records)
    save_json(output / 'external-dependencies.json', unresolved)
    call('codesign-verify', ['codesign', '--verify', '--deep', '--strict', '--verbose=2', app])
    call('architecture', ['lipo', '-archs', binaries / 'komamimic'])
    if unresolved:
        raise ValueError('External non-system dylibs remain; see external-dependencies.json. Do not treat this .app as portable.')
    call('staged-version', [binaries / 'komamimic', '--version'], macos_probe_environment(), timeout=30)
    return binaries / 'komamimic'


def stage(build, output):
    build, output = Path(build).resolve(), Path(output).resolve()
    system = platform.system()
    if system not in ('Windows', 'Darwin'):
        raise ValueError('Run natively on Windows or macOS; Linux uses cmake --install')
    if output.exists():
        raise ValueError('Choose a new stage output')
    cache = cache_values(build)
    if not cache.get('CMAKE_HOME_DIRECTORY') or Path(cache['CMAKE_HOME_DIRECTORY']).resolve() != ROOT:
        raise ValueError('Build belongs to a different source directory; use this kit source and its build')
    output.mkdir(parents=True)
    info = dict(schema=1, kind='local-validation-only', system=system, status='RUNNING', steps=[])
    save_json(output / 'SOURCE_MANIFEST.json', dict(schema=1, files=[
        dict(path=p.relative_to(ROOT).as_posix(), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
        for p in public_files(ROOT)]))
    info['source_manifest_sha256'] = hashlib.sha256((output / 'SOURCE_MANIFEST.json').read_bytes()).hexdigest()
    def call(name, command, env=None, timeout=None):
        options = dict(env=env)
        if timeout is not None:
            options['timeout'] = timeout
        record = execute(command, output / (name + '.log'), **options)
        info['steps'].append(record)
        if record['returncode']:
            raise RuntimeError(name + ' failed; see its log')
    try:
        executable = (windows if system == 'Windows' else macos)(build, output, call)
        info['executable'] = str(executable)
        info['status'] = 'STAGED_NOT_MANUALLY_VERIFIED'
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        info['status'] = 'FAIL'
        info['error'] = str(error)
    finally:
        if system == 'Darwin':
            info['symlinks'] = {p.relative_to(output).as_posix(): os.readlink(p)
                                for p in sorted(output.rglob('*')) if p.is_symlink()}
        info['files'] = [{'path': p.relative_to(output).as_posix(),
                          'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                         for p in sorted(output.rglob('*')) if p.is_file() and not p.is_symlink()]
        save_json(output / 'STAGE_MANIFEST.json', info)
        print(json.dumps({k: v for k, v in info.items() if k != 'files'}, ensure_ascii=False, indent=2))
    if info['status'] != 'FAIL' and system == 'Windows':
        package = output / 'KomaMimic-windows-x64-validation.zip'
        with zipfile.ZipFile(package, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(output.rglob('*')):
                if path.is_file() and path != package:
                    archive.write(path, path.relative_to(output).as_posix())
        (output / 'SHA256SUMS').write_text(hashlib.sha256(package.read_bytes()).hexdigest() + '  ' + package.name + '\n', encoding='ascii')
    return 1 if info['status'] == 'FAIL' else 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.exit(stage(args.build_dir, args.output))
