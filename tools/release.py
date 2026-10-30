#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate a stable tag, assemble tested artifacts, and publish a complete release.

Only the explicit publish/resume subcommands contact GitHub. Validation and assembly are
read-only with respect to Git and never create tags or modify existing outputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
from urllib.parse import quote
import zipfile

from export_source import public_files

ROOT = Path(__file__).resolve().parents[1]
VERSION = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
COMMIT = r'[0-9a-f]{40}'


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def save_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def relative_name(name):
    if not isinstance(name, str):
        raise ValueError('Non-string path')
    name = name.removeprefix('./')  # GNU sha256sum may emit this prefix.
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or '..' in path.parts or path.as_posix() != name
            or any(c in name for c in '\\:\r\n\t\0')):
        raise ValueError('Unsafe path: ' + repr(name))
    return name


def file_at(directory, name):
    directory = Path(directory).resolve()
    path = directory / relative_name(name)
    if (not path.is_file() or not path.resolve().is_relative_to(directory)
            or any(p.is_symlink() for p in (path, *path.parents) if p != directory and p.is_relative_to(directory))):
        raise ValueError('Missing or symlinked input: ' + str(path))
    return path


def checksum_records(directory):
    result = {}
    for line in file_at(directory, 'SHA256SUMS').read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not match:
            raise ValueError('Invalid checksum line')
        checksum, name = match.groups()
        name = relative_name(name)
        if name in result or digest(file_at(directory, name)) != checksum:
            raise ValueError('Duplicate or mismatched checksum: ' + name)
        result[name] = checksum
    if not result:
        raise ValueError('Empty checksum list')
    return result


def version_for_tag(root, tag=None):
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'project\(KomaMimic VERSION (' + VERSION + r')\s', cmake)
    if not match:
        raise ValueError('Invalid CMake project version')
    version = match[1]
    tag = tag or 'v' + version
    if not re.fullmatch('v' + VERSION, tag) or tag != 'v' + version:
        raise ValueError('Use a stable vMAJOR.MINOR.PATCH tag matching the source version')
    for name in ('vcpkg.json', 'cmake/windows-fast/vcpkg.json'):
        if json.loads((root / name).read_text(encoding='utf-8'))['version-string'] != version:
            raise ValueError('Version mismatch: ' + name)
    if not re.search(r'(?<![\w.])' + re.escape(tag) + r'(?![\w.])', (root / 'README.md').read_text(encoding='utf-8')):
        raise ValueError('README must identify the release version')
    return tag, version


def validate_checkout(root, tag, commit, require_tag=False):
    tag, version = version_for_tag(root, tag)
    if not re.fullmatch(COMMIT, commit):
        raise ValueError('Invalid commit SHA')
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    if git('rev-parse', 'HEAD') != commit:
        raise ValueError('Checkout differs from the triggering commit')
    if git('status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Release checkout is dirty')
    if require_tag and git('rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}') != commit:
        raise ValueError('Tag does not identify the triggering commit')
    return dict(tag=tag, version=version, commit=commit)


def read_source(directory, version):
    sums = checksum_records(directory)
    name = f'KomaMimic-{version}-source.tar.gz'
    if not {name, 'SOURCE_MANIFEST.json'} <= sums.keys():
        raise ValueError('Missing source checksums')
    manifest = json.loads(file_at(directory, 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    if manifest.get('version') != version or manifest.get('distribution') != 'source-only':
        raise ValueError('Source manifest identity mismatch')
    records = {}
    for item in manifest['files']:
        path = relative_name(item['path'])
        if path in records or not re.fullmatch(r'[0-9a-f]{64}', item['sha256']):
            raise ValueError('Invalid source record: ' + path)
        records[path] = item['sha256']
    actual, prefix = {}, f'KomaMimic-{version}/'
    with tarfile.open(file_at(directory, name)) as archive:
        for item in archive:
            if not item.isfile() or not item.name.startswith(prefix):
                raise ValueError('Unexpected source archive member')
            path = relative_name(item.name[len(prefix):])
            if path in actual:
                raise ValueError('Duplicate source archive member')
            actual[path] = archive.extractfile(item).read()
    if not actual or {p: hashlib.sha256(b).hexdigest() for p, b in actual.items()} != records:
        raise ValueError('Source archive differs from its manifest')
    return manifest, actual, sums


def compare_source(actual, canonical, allow_crlf=False):
    if actual.keys() != canonical.keys():
        raise ValueError('Artifact source file selection differs')
    differences = []
    for name, data in actual.items():
        expected = canonical[name]
        if data == expected:
            continue
        if allow_crlf and b'\0' not in data + expected:
            try:
                data.decode('utf-8')
                expected.decode('utf-8')
            except UnicodeDecodeError:
                pass
            else:
                if data.replace(b'\r\n', b'\n') == expected.replace(b'\r\n', b'\n'):
                    differences.append(name)
                    continue
        raise ValueError('Artifact source content differs: ' + name)
    return differences


def check_artifact(directory, system, version, commit, canonical):
    sums = checksum_records(directory)
    manifest = json.loads(file_at(directory, 'BUILD_MANIFEST.json').read_text(encoding='utf-8'))
    if manifest.get('version') != version or manifest.get('commit') != commit:
        raise ValueError('Artifact version/commit mismatch: ' + system)
    if system != 'linux' and manifest.get('source_tree_dirty') is not False:
        raise ValueError('Dirty artifact: ' + system)
    prefix = f'KomaMimic-{version}'
    expected = dict(windows={prefix + '-windows-x86_64.zip', 'BUILD_MANIFEST.json'},
                    macos={prefix + '-macos-arm64.app.zip', prefix + '-macos-arm64.dmg',
                           'BUILD_MANIFEST.json', 'HOMEBREW.json'},
                    linux={prefix + '-linux-x86_64.AppImage', 'komamimic.desktop',
                           'komamimic.png', 'BUILD_MANIFEST.json'})[system]
    if set(sums) != expected:
        raise ValueError('Unexpected artifact checksum selection: ' + system)
    source, data, source_sums = read_source(directory / 'sources', version)
    eol = compare_source(data, canonical, allow_crlf=system == 'windows')
    if system != 'linux' and manifest.get('source_files') != source['files']:
        raise ValueError('Artifact source records differ: ' + system)
    if system == 'linux':
        if sums[prefix + '-linux-x86_64.AppImage'] != manifest.get('appimage_sha256'):
            raise ValueError('AppImage hash differs from build manifest')
        for name in ('komamimic.desktop', 'komamimic.png'):
            if sums[name] != manifest.get('desktop_integration', {}).get(name):
                raise ValueError('Desktop integration hash differs')
        dependencies = manifest.get('source_archives', {})
        if not dependencies or any(source_sums.get(n) != s for n, s in dependencies.items()):
            raise ValueError('Missing dependency source archive')
        if set(source_sums) != {prefix + '-source.tar.gz', 'SOURCE_MANIFEST.json', *dependencies}:
            raise ValueError('Unexpected Linux source selection')
    elif set(source_sums) != {prefix + '-source.tar.gz', 'SOURCE_MANIFEST.json'}:
        raise ValueError('Unexpected OS source selection')
    return dict(manifest=manifest, source_eol_differences=eol, checksums=sums)


def write_sums(directory):
    paths = sorted(p for p in directory.rglob('*') if p.is_file() and p != directory / 'SHA256SUMS')
    (directory / 'SHA256SUMS').write_text(''.join(
        digest(p) + '  ' + p.relative_to(directory).as_posix() + '\n' for p in paths), encoding='ascii')


def linux_zip(target, files):
    with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, path in sorted(files.items()):
            entry = zipfile.ZipInfo(relative_name(name), date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | (0o755 if name.endswith('.AppImage') else 0o644)) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            with path.open('rb') as source, archive.open(entry, 'w', force_zip64=True) as output:
                shutil.copyfileobj(source, output)
    with zipfile.ZipFile(target) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(files):
            raise ValueError('Linux release ZIP failed round-trip validation')
        for name, path in files.items():
            with archive.open(name) as stream:
                value = hashlib.sha256()
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    value.update(chunk)
            if value.hexdigest() != digest(path):
                raise ValueError('Linux release ZIP payload changed')


def release_notes(tag, version):
    heading = 'Initial release' if version == '1.0.0' else 'Release ' + tag
    return (f'# KomaMimic {tag} — {heading}\n\n'
            '- Windows x86_64: extract the ZIP; Microsoft Visual C++ v14 x64 Redistributable is required.\n'
            '- macOS arm64: copy the app from the DMG to Applications. macOS 15+; no Developer ID signature or Apple notarization.\n'
            '- Linux x86_64: the ZIP contains the AppImage, desktop entry, icon, and source files.\n\n'
            'See the [English installation guide](https://asfdrwe.github.io/KomaMimic/en/user/install/) '
            'or [Japanese guide](https://asfdrwe.github.io/KomaMimic/user/install/).\n\n'
            'SHA256SUMS covers every attached file except itself. The application source archive is '
            'generated once on Linux; per-OS build manifests and RELEASE_MANIFEST.json record provenance.\n')


def assemble(root, inputs, output, tag, commit):
    tag, version = version_for_tag(root, tag)
    if not re.fullmatch(COMMIT, commit):
        raise ValueError('Invalid commit SHA')
    source = inputs / 'source'
    source_manifest, canonical, _ = read_source(source, version)
    expected = {p.relative_to(root).as_posix(): p.read_bytes() for p in public_files(root)}
    compare_source(canonical, expected)
    reports = {system: check_artifact(inputs / system, system, version, commit, canonical)
               for system in ('windows', 'macos', 'linux')}
    output.mkdir(parents=True, exist_ok=False)
    prefix = f'KomaMimic-{version}'
    for name in (prefix + '-source.tar.gz', 'SOURCE_MANIFEST.json'):
        target = prefix + '-SOURCE_MANIFEST.json' if name == 'SOURCE_MANIFEST.json' else name
        shutil.copy2(file_at(source, name), output / target)
    for system, report in reports.items():
        suffix = 'macos-arm64' if system == 'macos' else system + '-x86_64'
        shutil.copy2(inputs / system / 'BUILD_MANIFEST.json', output / (prefix + '-' + suffix + '-BUILD_MANIFEST.json'))
        if system != 'linux':
            for name in report['checksums']:
                if name.endswith(('.zip', '.dmg')):
                    shutil.copy2(file_at(inputs / system, name), output / name)
    shutil.copy2(inputs / 'macos/HOMEBREW.json', output / (prefix + '-macos-arm64-HOMEBREW.json'))
    # Keep source archive names unique. Only the Ubuntu-generated application
    # source is published; Windows checkout CRLF variants are never copied.
    for name in reports['linux']['manifest']['source_archives']:
        if (not re.fullmatch(r'[A-Za-z0-9_.+-]+\.tar\.gz', name)
                or name in {p.name for p in output.iterdir()}):
            raise ValueError('Dependency source name collision: ' + name)
        shutil.copy2(file_at(inputs / 'linux/sources', name), output / name)
    with tempfile.TemporaryDirectory(prefix='linux-release-', dir=output.parent) as temporary:
        linux = Path(temporary)
        for name in (prefix + '-linux-x86_64.AppImage', 'komamimic.desktop', 'komamimic.png', 'BUILD_MANIFEST.json'):
            shutil.copy2(file_at(inputs / 'linux', name), linux / name)
        (linux / 'sources').mkdir()
        for name in (prefix + '-source.tar.gz', 'SOURCE_MANIFEST.json'):
            shutil.copy2(file_at(source, name), linux / 'sources' / name)
        for name in reports['linux']['manifest']['source_archives']:
            shutil.copy2(file_at(inputs / 'linux/sources', name), linux / 'sources' / name)
        write_sums(linux / 'sources')
        write_sums(linux)
        files = {p.relative_to(linux).as_posix(): p for p in linux.rglob('*') if p.is_file()}
        linux_zip(output / (prefix + '-linux-x86_64.zip'), files)
    (output / 'RELEASE_NOTES.md').write_text(release_notes(tag, version), encoding='utf-8')
    manifest = dict(schema=1, tag=tag, version=version, commit=commit,
                    source_manifest=prefix + '-SOURCE_MANIFEST.json',
                    source_sha256=digest(output / (prefix + '-source.tar.gz')),
                    windows_source_eol_differences=reports['windows']['source_eol_differences'],
                    verification='Required CI jobs and native package checks passed before assembly',
                    manual_verification='Not inferred from CI; original package records are retained',
                    files={p.name: digest(p) for p in sorted(output.iterdir())})
    save_json(output / 'RELEASE_MANIFEST.json', manifest)
    write_sums(output)
    verify_bundle(output, tag, commit)
    return manifest


def verify_bundle(directory, tag, commit):
    sums = checksum_records(directory)
    actual = {p.name for p in directory.iterdir() if p.is_file()}
    if any(p.is_dir() or p.is_symlink() for p in directory.iterdir()) or actual != {*sums, 'SHA256SUMS'}:
        raise ValueError('Unexpected release bundle contents')
    manifest = json.loads(file_at(directory, 'RELEASE_MANIFEST.json').read_text(encoding='utf-8'))
    if manifest.get('tag') != tag or manifest.get('commit') != commit or tag != 'v' + manifest.get('version', ''):
        raise ValueError('Release bundle identity mismatch')
    if manifest['files'] != {n: s for n, s in sums.items() if n != 'RELEASE_MANIFEST.json'}:
        raise ValueError('Release asset list differs from manifest')
    return {**sums, 'SHA256SUMS': digest(directory / 'SHA256SUMS')}


class GitHub:
    def __init__(self, repository):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
            raise ValueError('Invalid repository')
        self.repository = repository

    def api(self, endpoint, missing=False, *, method='GET', data=None, input_file=None):
        target = 'repos/' + self.repository + endpoint
        if endpoint.startswith('https://'):
            if not endpoint.startswith('https://uploads.github.com/repos/' + self.repository + '/'):
                raise ValueError('Unexpected upload endpoint')
            target = endpoint
        command = ['gh', 'api', target, '--method', method]
        options = dict(capture_output=True, text=True, timeout=900 if input_file else 120)
        if data is not None:
            command += ['--input', '-', '-H', 'Content-Type: application/json']
            options['input'] = json.dumps(data)
        if input_file is not None:
            command += ['--input', str(input_file), '-H', 'Content-Type: application/octet-stream',
                        '-H', 'Content-Length: ' + str(input_file.stat().st_size)]
        result = subprocess.run(command, **options)
        if result.returncode:
            if missing and method == 'GET' and '(HTTP 404)' in result.stderr:
                return None
            raise RuntimeError('GitHub API failed: ' + result.stderr.strip())
        return json.loads(result.stdout)

    def get_release(self, release_id):
        if type(release_id) is not int or release_id <= 0:
            raise ValueError('Invalid release ID')
        record = self.api('/releases/' + str(release_id))
        if record.get('id') != release_id:
            raise ValueError('Release ID changed')
        return record

    def tag_commit(self, tag):
        record = self.api('/git/ref/tags/' + tag)['object']
        for _ in range(8):
            if record['type'] == 'commit':
                return record['sha']
            if record['type'] != 'tag' or not re.fullmatch(COMMIT, record['sha']):
                break
            record = self.api('/git/tags/' + record['sha'])['object']
        raise ValueError('Invalid or excessively nested remote tag')

    def release(self, tag):
        record = self.api('/releases/tags/' + tag, missing=True)
        if record is not None:
            return record
        # The tag endpoint documents published releases. Authenticated listing
        # also exposes drafts, including one left by an interrupted upload.
        page = 1
        found = None
        while True:
            records = self.api('/releases?per_page=100&page=' + str(page))
            if not isinstance(records, list):
                raise ValueError('Invalid release listing')
            for item in records:
                if item['tag_name'] == tag:
                    if found is not None:
                        raise ValueError('Multiple releases have this tag')
                    found = item
            if len(records) < 100:
                return found
            page += 1

    def create(self, tag, commit, notes):
        # publish() has verified the existing remote tag. Keep the authoritative
        # creation response instead of immediately searching tag/list indexes.
        return self.api('/releases', method='POST', data=dict(
            tag_name=tag, target_commitish=commit, name='KomaMimic ' + tag,
            body=notes.read_text(encoding='utf-8'), draft=True, prerelease=False))

    def upload(self, release_id, path):
        if type(release_id) is not int or release_id <= 0:
            raise ValueError('Invalid release ID')
        self.api('https://uploads.github.com/repos/' + self.repository + '/releases/'
                 + str(release_id) + '/assets?name=' + quote(path.name, safe=''),
                 method='POST', input_file=path)  # A duplicate name fails; never replace.

    def publish(self, release_id):
        if type(release_id) is not int or release_id <= 0:
            raise ValueError('Invalid release ID')
        self.api('/releases/' + str(release_id), method='PATCH', data=dict(draft=False))

    def asset_digest(self, asset, directory):
        if asset.get('digest'):
            return asset['digest']
        # GitHub may omit an asset digest. Verify the downloaded bytes instead.
        with tempfile.TemporaryFile(dir=directory) as stream:
            subprocess.run(['gh', 'api', 'repos/' + self.repository + '/releases/assets/' + str(int(asset['id'])),
                            '-H', 'Accept: application/octet-stream'], stdout=stream, check=True, timeout=900)
            stream.seek(0)
            value = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                value.update(chunk)
            return 'sha256:' + value.hexdigest()


def check_remote_assets(github, record, directory, expected, subset=False):
    seen = set()
    for asset in record['assets']:
        name = asset['name']
        if (name not in expected or name in seen or asset.get('state') != 'uploaded'
                or asset.get('size') != (directory / name).stat().st_size
                or github.asset_digest(asset, directory.parent) != 'sha256:' + expected[name]):
            raise ValueError('Existing release asset differs: ' + name)
        seen.add(name)
    if not subset and seen != expected.keys():
        raise ValueError('Incomplete uploaded release')
    return seen


def check_draft_commit(record, commit):
    # GitHub may retain a branch here because target_commitish is unused for an
    # existing tag. The peeled remote tag and verified assets are authoritative.
    target = record.get('target_commitish', '')
    if re.fullmatch(COMMIT, target) and target != commit:
        raise ValueError('Existing draft belongs to another commit')


def publish(directory, tag, commit, github, existing_release_id=None):
    expected = verify_bundle(directory, tag, commit)
    if github.tag_commit(tag) != commit:
        raise ValueError('Remote tag moved or differs from the build')
    record = (github.get_release(existing_release_id) if existing_release_id is not None
              else github.release(tag))
    if record is None:
        if existing_release_id is not None:
            raise ValueError('Existing release was not found')
        record = github.create(tag, commit, directory / 'RELEASE_NOTES.md')
    if record is None or record['tag_name'] != tag or record.get('prerelease'):
        raise ValueError('Unexpected release record')
    release_id = record.get('id')
    if type(release_id) is not int or release_id <= 0:
        raise ValueError('Missing release ID')
    if existing_release_id is not None and release_id != existing_release_id:
        raise ValueError('Release ID changed')
    if not record['draft']:
        check_remote_assets(github, record, directory, expected)
        print('Matching release is already published; no changes made')
        return
    check_draft_commit(record, commit)
    uploaded = check_remote_assets(github, record, directory, expected, subset=True)
    for name in sorted(expected.keys() - uploaded):
        print('Uploading verified asset: ' + name, flush=True)
        github.upload(release_id, directory / name)
    record = github.get_release(release_id)
    if (not record or record.get('id') != release_id or not record['draft']
            or record['tag_name'] != tag or record.get('prerelease')):
        raise ValueError('Draft changed during upload')
    check_draft_commit(record, commit)
    check_remote_assets(github, record, directory, expected)
    if github.tag_commit(tag) != commit:
        raise ValueError('Remote tag moved during upload')
    github.publish(release_id)
    record = github.get_release(release_id)
    if not record or record.get('id') != release_id or record['draft'] or record['tag_name'] != tag:
        raise ValueError('Release was not published')
    check_remote_assets(github, record, directory, expected)
    print('Published complete release: ' + tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('validate', 'assemble', 'verify', 'publish'):
        command = commands.add_parser(name)
        command.add_argument('--tag')
        command.add_argument('--commit', required=True)
        if name == 'validate':
            command.add_argument('--require-tag', action='store_true')
            command.add_argument('--github-output', type=Path)
        elif name == 'assemble':
            command.add_argument('--inputs', type=Path, required=True)
            command.add_argument('--output', type=Path, required=True)
        else:
            command.add_argument('--bundle', type=Path, required=True)
        if name == 'publish':
            command.add_argument('--repo', required=True)
    resume = commands.add_parser('resume', help='Administrator recovery using an existing release ID and verified bundle')
    resume.add_argument('--bundle', type=Path, required=True)
    resume.add_argument('--repo', required=True)
    resume.add_argument('--release-id', type=int, required=True)
    args = parser.parse_args()
    if args.command == 'validate':
        info = validate_checkout(ROOT, args.tag, args.commit, args.require_tag)
        if args.github_output:
            with args.github_output.open('a', encoding='utf-8') as stream:
                stream.write(''.join(k + '=' + v + '\n' for k, v in info.items()))
        print(json.dumps(info))
    elif args.command == 'assemble':
        assemble(ROOT, args.inputs.resolve(), args.output.resolve(), args.tag, args.commit)
    elif args.command == 'verify':
        verify_bundle(args.bundle.resolve(), args.tag, args.commit)
    elif args.command == 'resume':
        # An older, already-built tag may need a repaired publisher from a newer
        # checkout. Identity comes from the complete bundle and the remote tag,
        # not this checkout's HEAD. Never create a release in recovery mode.
        directory = args.bundle.resolve()
        identity = json.loads(file_at(directory, 'RELEASE_MANIFEST.json').read_text(encoding='utf-8'))
        tag, commit = identity.get('tag', ''), identity.get('commit', '')
        if not re.fullmatch('v' + VERSION, tag) or not re.fullmatch(COMMIT, commit):
            raise ValueError('Invalid recovery bundle identity')
        if args.release_id <= 0:
            raise ValueError('Invalid release ID')
        publish(directory, tag, commit, GitHub(args.repo), existing_release_id=args.release_id)
    else:
        if os.environ.get('GITHUB_EVENT_NAME') != 'push' or os.environ.get('GITHUB_REF') != 'refs/tags/' + str(args.tag):
            raise ValueError('Publishing is only enabled for the matching tag push')
        validate_checkout(ROOT, args.tag, args.commit, require_tag=True)
        publish(args.bundle.resolve(), args.tag, args.commit, GitHub(args.repo))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, tarfile.TarError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
