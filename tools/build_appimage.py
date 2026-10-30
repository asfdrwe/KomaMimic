#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build and check a Linux x86_64 AppImage candidate from tested native outputs."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
# These come from the target OS: glibc/loader and the graphics driver interface.
# Qt, ICU, C++ runtime, XCB, codecs, NSS and all other resolved DSOs are bundled.
HOST_LIBRARIES = re.compile(
    r'^(?:ld-linux[^/]*|lib(?:c|m|mvec|pthread|dl|rt|resolv|util|anl|BrokenLocale)\.so(?:\..*)?'
    r'|libnss_[^/]+\.so(?:\..*)?|lib(?:GL|GLX|OpenGL|EGL|GLdispatch)\.so(?:\..*)?)$')
APP_RUN = '''#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export APPDIR="$HERE"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/usr/plugins/platforms"
export QTWEBENGINEPROCESS_PATH="$HERE/usr/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$HERE/usr/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="$HERE/usr/translations/qtwebengine_locales"
export POPPLER_DATADIR="$HERE/usr/share/poppler"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/usr/bin/komamimic" "$@"
'''


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, **kwargs).strip()


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def copy(source, target):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(source).resolve(strict=True), target)


def elf(path):
    with Path(path).open('rb') as stream:
        return stream.read(4) == b'\x7fELF'


def x86_64_elf(path):
    with Path(path).open('rb') as stream:
        header = stream.read(20)
    return (len(header) == 20 and header[:6] == b'\x7fELF\x02\x01'
            and struct.unpack_from('<H', header, 18)[0] == 62)


def dependencies(path, env=None):
    result = output(['ldd', path], env=env)
    if 'not found' in result:
        raise ValueError(f'Unresolved dependencies of {path}:\n{result}')
    found = []
    for line in result.splitlines():
        match = re.match(r'\s*(\S+) => (/.+?) \(', line)
        if match:
            found.append((match[1], Path(match[2])))
    return found


def glibc_versions(text):
    markers = set(re.findall(r'\bGLIBC_ABI_[A-Z0-9_]+', text))
    if 'GLIBC_PRIVATE' in text or markers - {'GLIBC_ABI_DT_RELR', 'GLIBC_ABI_GNU2_TLS'}:
        raise ValueError('Payload uses a private/non-numeric glibc ABI')
    versions = {tuple(map(int, version.split('.')))
                for version in re.findall(r'\bGLIBC_([0-9]+(?:\.[0-9]+)+)', text)}
    if 'GLIBC_ABI_DT_RELR' in markers:
        versions.add((2, 36))
    if 'GLIBC_ABI_GNU2_TLS' in markers:
        # Conservative supported floor, not its historical introduction:
        # older distro releases have inconsistent backports of this tag.
        versions.add((2, 43))
    return versions


def qtpaths():
    candidates = [shutil.which('qtpaths6'), '/usr/lib64/qt6/bin/qtpaths', '/usr/lib/qt6/bin/qtpaths']
    return next(Path(p) for p in candidates if p and Path(p).is_file())


def japanese_catalog(translations):
    standard = translations / 'qtbase_ja.qm'
    if standard.is_file():
        return standard
    files = output(['rpm', '-ql', 'qt6-qttranslations']).splitlines()
    candidates = [Path(p) for p in files if Path(p).name == 'qtbase_ja.qm']
    usable = sorted({p.resolve(strict=True) for p in candidates if p.is_file()})
    print('RPM Japanese Qt catalog candidates: ' + repr(candidates), flush=True)
    if len(usable) != 1:
        raise ValueError('Japanese Qt catalog missing or ambiguous in qt6-qttranslations; '
                         'check the RPM _install_langs filter and translation package: ' + repr(usable))
    print('Japanese Qt catalog: ' + str(usable[0]), flush=True)
    return usable[0]


def qt_resources(app):
    paths = qtpaths()
    query = lambda key: Path(output([paths, '--query', key]))
    plugins = query('QT_INSTALL_PLUGINS')
    seeds = []
    selected = [plugins / 'platforms/libqxcb.so', plugins / 'platforms/libqoffscreen.so',
                plugins / 'sqldrivers/libqsqlite.so']
    for group in ('imageformats', 'xcbglintegrations', 'platforminputcontexts'):
        selected.extend(sorted((plugins / group).glob('*.so')))
    for source in selected:
        target = app / 'usr/plugins' / source.relative_to(plugins)
        copy(source, target)
        seeds.append((source, target))
    webengine = output(['rpm', '-ql', 'qt6-qtwebengine']).splitlines()
    helpers = [Path(p) for p in webengine if Path(p).name == 'QtWebEngineProcess' and Path(p).is_file()]
    if len(helpers) != 1:
        raise ValueError('Expected one QtWebEngineProcess: ' + repr(helpers))
    helper = app / 'usr/libexec/QtWebEngineProcess'
    copy(helpers[0], helper)
    seeds.append((helpers[0], helper))
    resources = [p for p in (query('QT_INSTALL_DATA') / 'resources',
                             query('QT_INSTALL_ARCHDATA') / 'resources') if p.is_dir()]
    resources = next((p for p in resources if (p / 'qtwebengine_resources.pak').is_file()), None)
    if resources is None:
        raise ValueError('Qt WebEngine resources directory is missing')
    shutil.copytree(resources, app / 'usr/share/qt6/resources')
    translations = query('QT_INSTALL_TRANSLATIONS')
    shutil.copytree(translations / 'qtwebengine_locales', app / 'usr/translations/qtwebengine_locales')
    copy(japanese_catalog(translations), app / 'usr/translations/qtbase_ja.qm')
    # NSS providers are loaded with dlopen and do not appear in ldd's closure.
    rpm_files = output(['rpm', '-qal']).splitlines()
    for name in ('libsoftokn3.so', 'libfreebl3.so', 'libfreeblpriv3.so', 'libnssckbi.so'):
        sources = sorted({Path(p).resolve() for p in rpm_files
                          if Path(p).name == name and Path(p).is_file() and x86_64_elf(p)})
        if len(sources) != 1:
            raise ValueError('Expected one NSS provider ' + name + ': ' + repr(sources))
        target = app / 'usr/lib' / name
        copy(sources[0], target)
        if sources[0].with_suffix('.chk').is_file():
            copy(sources[0].with_suffix('.chk'), target.with_suffix('.chk'))
        seeds.append((sources[0], target))
    return seeds


def dependency_chain(source, target, resolved):
    """Explain a transitive dependency with the source ELFs' DT_NEEDED edges."""
    queue = deque([(Path(source), [str(source)])])
    visited = set()
    while queue:
        path, chain = queue.popleft()
        if path in visited:
            continue
        visited.add(path)
        result = subprocess.run(['patchelf', '--print-needed', str(path)],
                                text=True, capture_output=True)
        if result.returncode:
            continue
        for name in result.stdout.splitlines():
            if name not in resolved:
                continue
            entry = name + ' => ' + str(resolved[name])
            if name == target:
                return [*chain, entry]
            queue.append((resolved[name], [*chain, entry]))
    return [str(source), target + ' (reported by ldd; direct chain unavailable)']


def collect_libraries(app, seeds, preferred=()):
    libraries = {}
    alternatives = []
    extra_seeds = []
    tiff_roots = []
    # Keep the exact Poppler/GLib/Cairo versions selected by CMake, even when
    # a vendor ELF's RPATH would otherwise prefer the older system equivalent.
    for path in preferred:
        name = output(['patchelf', '--print-soname', path])
        if not name or '/' in name or HOST_LIBRARIES.fullmatch(name):
            raise ValueError('Invalid preferred library SONAME: ' + str(path))
        libraries[name] = path.resolve(strict=True)
        target = app / 'usr/lib' / name
        copy(path, target)
        extra_seeds.append((path, target))
    # ldd includes each source's transitive dependency closure. Preserve the
    # first resolution of a SONAME, rather than bundling conflicting duplicates.
    for source, _ in [*extra_seeds, *seeds]:
        resolved = dict(dependencies(source))
        for tiff in ('libtiff.so.5', 'libtiff.so.6'):
            if tiff in resolved:
                tiff_roots.append((source, tiff, resolved))
        for name, path in resolved.items():
            if HOST_LIBRARIES.fullmatch(name):
                continue
            if '/' in name:
                raise ValueError('Absolute DT_NEEDED is not relocatable: ' + name)
            real = path.resolve(strict=True)
            if name in libraries:
                if real != libraries[name]:
                    alternatives.append(dict(soname=name, selected=str(libraries[name]), other=str(real)))
                continue
            libraries[name] = real
            copy(real, app / 'usr/lib' / name)
    if 'libtiff.so.5' in libraries and 'libtiff.so.6' in libraries:
        report = [dict(root=str(source), tiff=tiff, library=str(resolved[tiff]),
                       chain=dependency_chain(source, tiff, resolved))
                  for source, tiff, resolved in tiff_roots]
        text = json.dumps(report, indent=2)
        (app.parent / 'TIFF_DEPENDENCIES.json').write_text(text + '\n')
        print('TIFF dependency chains:\n' + text, flush=True)
        raise ValueError('Mixed TIFF 5/6 would reintroduce the known Qt/Poppler crash; '
                         'see TIFF_DEPENDENCIES.json and the dependency chains above')
    return libraries, alternatives


def audit_elf_layout(path):
    """Detect relocated sections/initializers that no longer match PT_LOAD."""
    with Path(path).open('rb') as stream:
        header = struct.unpack('<16sHHIQQQIHHHHHH', stream.read(64))
        if header[0][:6] != b'\x7fELF\x02\x01':
            raise ValueError('Expected little-endian ELF64: ' + str(path))
        segments = []
        for index in range(header[10]):
            stream.seek(header[5] + index * header[9])
            segments.append(struct.unpack('<IIQQQQQQ', stream.read(56)))
        loads = [p for p in segments if p[0] == 1]
        sections = []
        for index in range(header[12]):
            stream.seek(header[6] + index * header[11])
            sections.append(struct.unpack('<IIQQQQIIQQ', stream.read(64)))
        names = sections[header[13]]
        stream.seek(names[4])
        names = stream.read(names[5])
        for section in sections:
            name = names[section[0]:].split(b'\0', 1)[0].decode()
            if not section[2] & 2 or section[1] == 8 or not section[5]:
                continue  # Non-allocated, NOBITS, or empty section.
            if not any(p[3] <= section[3] and section[3] + section[5] <= p[3] + p[5]
                       and section[4] - p[2] == section[3] - p[3]
                       and (not section[2] & 4 or p[1] & 1) for p in loads):
                raise ValueError(f'Invalid ELF section mapping {path}: {name}')
        for segment in segments:
            if segment[0] != 2:
                continue
            stream.seek(segment[2])
            entries = stream.read(segment[5])
            for offset in range(0, len(entries), 16):
                tag, address = struct.unpack_from('<QQ', entries, offset)
                if tag not in (12, 13) or not address:
                    continue
                name = '.init' if tag == 12 else '.fini'
                # -Wl,-init/-fini may name a function in .text instead of
                # the conventional section; both are valid executable code.
                if not any(s[2] & 4 and s[3] <= address < s[3] + s[5] for s in sections) or not any(
                        p[1] & 1 and p[3] <= address < p[3] + p[5] for p in loads):
                    raise ValueError(f'Invalid ELF initializer {path}: {name} {address:#x}')


def relocate_and_audit(app, max_glibc):
    tool_version = output(['patchelf', '--version'])
    if tuple(map(int, re.search(r'(\d+)\.(\d+)\.(\d+)', tool_version).groups())) < (0, 19, 2):
        raise ValueError('patchelf >= 0.19.2 required to preserve ELF layout; found ' + tool_version)
    app = Path(app).resolve()
    binaries = [p for p in app.rglob('*') if p.is_file() and elf(p)]
    versions = set()
    env = dict(os.environ, LD_LIBRARY_PATH=str(app / 'usr/lib'))
    for binary in binaries:
        if 'Advanced Micro Devices X86-64' not in output(['readelf', '-h', binary]):
            raise ValueError('Non-x86_64 ELF: ' + str(binary))
        relative = os.path.relpath(app / 'usr/lib', binary.parent)
        rpath = '$ORIGIN' if relative == '.' else '$ORIGIN/' + relative
        # NSS providers can carry integrity signatures. Keep their signed bytes
        # intact; AppRun supplies usr/lib to their dynamic dependency search.
        if not binary.with_suffix('.chk').is_file():
            run(['patchelf', '--force-rpath', '--set-rpath', rpath, binary])
        audit_elf_layout(binary)
        versions.update(glibc_versions(output(['readelf', '--version-info', binary])))
    if versions and max(versions) > max_glibc:
        raise ValueError(f'Payload needs GLIBC_{max(versions)}, limit is {max_glibc}')
    for binary in binaries:
        for name, resolved in dependencies(binary, env):
            if not HOST_LIBRARIES.fullmatch(name) and not resolved.resolve().is_relative_to(app):
                raise ValueError(f'Payload leaks a build-host dependency: {binary}: {name} => {resolved}')
    return '.'.join(map(str, max(versions))) if versions else None


def package_notices(app, libraries, seeds, sources):
    notices = app / 'usr/share/doc/KomaMimic/dependencies'
    packages = {}
    data_files = {p.resolve() for p in Path('/usr/share/poppler').rglob('*') if p.is_file()}
    # One data file identifies the poppler-data RPM and its license directory.
    data_owner = set(sorted(data_files)[:1])
    for path in sorted(set(libraries.values()) | {p.resolve() for p, _ in seeds} | data_owner):
        result = subprocess.run(['rpm', '-qf', str(path), '--qf', '%{NAME}\n'], text=True, capture_output=True)
        if result.returncode:
            continue  # Locally built libarchive is recorded separately below.
        for name in set(result.stdout.splitlines()):
            if name in packages:
                continue
            packages[name] = output(['rpm', '-q', name, '--qf', '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\t%{LICENSE}\t%{SOURCERPM}'])
            files = output(['rpm', '-ql', name]).splitlines()
            for filename in files:
                source = Path(filename)
                legal = source.name.upper().startswith(('COPYING', 'LICENSE', 'COPYRIGHT', 'NOTICE', 'AUTHORS'))
                if source.is_file() and ('/licenses/' in filename or ('/doc/' in filename and legal)):
                    copy(source, notices / name / source.relative_to('/'))
    copy(sources / 'libarchive-COPYING.txt', notices / 'libarchive/COPYING')
    shutil.copytree(sources / 'poppler-notices', notices / 'poppler', dirs_exist_ok=True)
    return packages


def stage(build, destination, sources, max_glibc):
    destination.mkdir(parents=True, exist_ok=False)
    app = destination / 'AppDir'
    run(['cmake', '--install', build, '--prefix', app / 'usr'])
    seeds = [(build / name, app / 'usr/bin' / name) for name in ('komamimic', 'komamimic-script-worker')]
    # cmake --install may change RPATHs, so inspect the original tested ELF files.
    seeds += qt_resources(app)
    shutil.copytree('/usr/share/poppler', app / 'usr/share/poppler')
    cache = (build / 'CMakeCache.txt').read_text()
    preferred = [Path(p) for p in re.findall(r'^pkgcfg_lib_POPPLER_[^:]+:FILEPATH=(/[^\n]+)$', cache, re.MULTILINE)
                 if Path(p).is_file()]
    libraries, alternatives = collect_libraries(app, seeds, preferred)
    required_glibc = relocate_and_audit(app, max_glibc)
    packages = package_notices(app, libraries, seeds, sources)
    for directory in ('usr/bin', 'usr/libexec'):
        (app / directory / 'qt.conf').write_text(
            '[Paths]\nPrefix=..\nPlugins=plugins\nLibraries=lib\nTranslations=translations\n'
            'Data=share/qt6\nLibraryExecutables=libexec\n')
    copy(app / 'usr/share/applications/komamimic.desktop', app / 'komamimic.desktop')
    copy(app / 'usr/share/icons/hicolor/256x256/apps/komamimic.png', app / 'komamimic.png')
    copy(app / 'komamimic.png', app / '.DirIcon')
    (app / 'AppRun').write_text(APP_RUN)
    (app / 'AppRun').chmod(0o755)
    for flag in ('--version', '--formats'):
        run([app / 'AppRun', flag])
    manifest = dict(schema=1, status='CANDIDATE_REQUIRES_MANUAL_VALIDATION',
                    version=re.search(r'project\(KomaMimic VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text())[1],
                    # Container checkout and packaging users may differ. Trust
                    # only this explicit source directory for this read command.
                    commit=output(['git', '-c', 'safe.directory=' + str(ROOT),
                                   '-C', ROOT, 'rev-parse', 'HEAD']), required_glibc=required_glibc,
                    poppler_build=json.loads((sources / 'poppler-source.json').read_text()),
                    patchelf_version=output(['patchelf', '--version']),
                    source_archives={p.name: digest(p) for p in sorted(sources.glob('*.tar.gz'))},
                    host_libraries=HOST_LIBRARIES.pattern, packages=packages,
                    libraries={n: str(p) for n, p in libraries.items()}, resolution_alternatives=alternatives,
                    files={p.relative_to(app).as_posix(): digest(p) for p in sorted(app.rglob('*')) if p.is_file()})
    text = json.dumps(manifest, ensure_ascii=False, indent=2) + '\n'
    (destination / 'BUILD_MANIFEST.json').write_text(text)
    (app / 'usr/share/doc/KomaMimic/BUILD_MANIFEST.json').write_text(text)
    with tarfile.open(destination / 'AppDir.tar.gz', 'w:gz') as archive:
        archive.add(app, arcname='AppDir')
    copy(build / 'validation_probe', destination / 'validation_probe')


def download(entry, target):
    with urllib.request.urlopen(entry['url'], timeout=120) as source, target.open('wb') as destination:
        shutil.copyfileobj(source, destination)
    actual = digest(target)
    if actual != entry['sha256']:
        raise ValueError('Packaging tool checksum mismatch: ' + entry['url']
                         + '; expected=' + entry['sha256'] + '; actual=' + actual)
    target.chmod(0o755)


def desktop_integration(app, image, destination):
    """Export user-installable files alongside the versioned AppImage."""
    if not re.fullmatch(r'KomaMimic-[0-9.]+-linux-x86_64\.AppImage', image.name):
        raise ValueError('Unexpected AppImage filename for desktop launcher')
    text = (app / 'komamimic.desktop').read_text(encoding='utf-8')
    if text.splitlines().count('Exec=komamimic %F') != 1:
        raise ValueError('Unexpected staged desktop launcher')
    # Desktop entries do not expand HOME. Use sh with two layers of escaping
    # (desktop string, then Exec quoting); file arguments stay outside the script.
    command = (rf'Exec=/bin/sh -c "exec \\"\\$HOME/.local/bin/{image.name}\\" \\"\\$@\\""'
               ' komamimic %F')
    desktop = destination / 'komamimic.desktop'
    desktop.write_text(text.replace('Exec=komamimic %F', command), encoding='utf-8')
    icon = destination / 'komamimic.png'
    copy(app / 'komamimic.png', icon)
    return {p.name: digest(p) for p in (desktop, icon)}


def pack(stage_dir, destination):
    destination.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((stage_dir / 'BUILD_MANIFEST.json').read_text())
    app = (stage_dir / 'AppDir').resolve()
    for relative, expected in manifest['files'].items():
        path = (app / relative).resolve()
        if not path.is_relative_to(app) or digest(path) != expected:
            raise ValueError('Payload changed after staging: ' + relative)
    config = json.loads((ROOT / 'tools/appimage-tools.json').read_text())
    tools = destination / 'tools'
    tools.mkdir()
    for name, entry in config.items():
        download(entry, tools / name)
    image = destination / f'KomaMimic-{manifest["version"]}-linux-x86_64.AppImage'
    env = dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1')
    run([tools / 'appimagetool', '--no-appstream', '--runtime-file', tools / 'runtime',
         stage_dir / 'AppDir', image], env=env)
    image.chmod(0o755)
    manifest['packaging_tools'] = config
    manifest['appimage_sha256'] = digest(image)
    manifest['desktop_integration'] = desktop_integration(app, image, destination)
    (destination / 'BUILD_MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (destination / 'SHA256SUMS').write_text(''.join(
        f'{digest(path)}  {path.name}\n' for path in (
            image, destination / 'komamimic.desktop', destination / 'komamimic.png',
            destination / 'BUILD_MANIFEST.json')), encoding='ascii')
    return image


def verify(image, probe, destination):
    destination.mkdir(parents=True, exist_ok=False)
    # No Qt SDK, RX prefix, or compilation environment may supply dependencies.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('QT_', 'QML', 'LD_', 'KOMA_PROBE_'))}
    env.update(APPIMAGE_EXTRACT_AND_RUN='1', QT_QPA_PLATFORM='offscreen',
               TMPDIR=str(destination), XDG_CACHE_HOME=str(destination / 'cache'))
    for flag in ('--version', '--formats'):
        run([image, flag], env=env)
    run([image, '--appimage-extract'], cwd=destination, env=env, stdout=subprocess.DEVNULL)
    app = destination / 'squashfs-root'
    env.update(LD_LIBRARY_PATH=str(app / 'usr/lib'), QT_PLUGIN_PATH=str(app / 'usr/plugins'),
               QT_QPA_PLATFORM_PLUGIN_PATH=str(app / 'usr/plugins/platforms'),
               QTWEBENGINEPROCESS_PATH=str(app / 'usr/libexec/QtWebEngineProcess'),
               QTWEBENGINE_RESOURCES_PATH=str(app / 'usr/share/qt6/resources'),
               QTWEBENGINE_LOCALES_PATH=str(app / 'usr/translations/qtwebengine_locales'))
    env['POPPLER_DATADIR'] = str(app / 'usr/share/poppler')
    # Exercise the same native reader probe, using only the extracted payload's
    # libraries/plugins and worker. The probe itself is not shipped in the image.
    copy(probe, app / 'usr/bin/validation_probe')
    (app / 'usr/bin/validation_probe').chmod(0o755)
    fixtures = destination / 'fixtures'
    run(['python3', ROOT / 'tests/fixtures/generate_validation_samples.py', '--output', fixtures,
         '--verify-worker', app / 'usr/bin/komamimic-script-worker'], env=env)
    run(['ldd', app / 'usr/bin/validation_probe'], env=env)
    try:
        result = output([app / 'usr/bin/validation_probe', fixtures], env=env)
    except subprocess.CalledProcessError:
        if shutil.which('gdb'):
            # The host debugger must not load the payload's older C++/NSS DSOs.
            debugger_env = {k: v for k, v in env.items() if not k.startswith('LD_')}
            subprocess.run(['gdb', '--batch', '-ex', 'set pagination off',
                            '-ex', 'set debuginfod enabled off',
                            '-ex', 'set startup-with-shell off',
                            '-ex', 'set environment LD_LIBRARY_PATH ' + env['LD_LIBRARY_PATH'], '-ex', 'run',
                            '-ex', 'thread apply all bt', '-ex', 'info sharedlibrary',
                            '--args', str(app / 'usr/bin/validation_probe'), str(fixtures)], env=debugger_env, check=False)
        raise
    report = json.loads(result)
    if not report['all_passed']:
        raise ValueError('Extracted AppImage native reader checks failed: ' + result)
    (destination / 'native-reader.json').write_text(result + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('stage')
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--sources', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--max-glibc', default='2.35')
    p = commands.add_parser('pack')
    p.add_argument('--stage', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = commands.add_parser('verify')
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'stage':
        stage(args.build_dir.resolve(), args.output.resolve(), args.sources.resolve(),
              tuple(map(int, args.max_glibc.split('.'))))
    elif args.command == 'pack':
        pack(args.stage.resolve(), args.output.resolve())
    else:
        verify(args.image.resolve(), args.probe.resolve(), args.output.resolve())
