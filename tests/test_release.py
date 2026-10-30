#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise release rejection paths and draft publication without GitHub writes."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import release

COMMIT = 'a' * 40
TAG = 'v1.0.0'


class FakeGitHub:
    """A remote with durable draft state, uploads, and injected failures."""
    def __init__(self):
        self.record = None
        self.events = []
        self.fail_upload = None
        self.move_tag = False
        self.tag_reads = 0

    def tag_commit(self, tag):
        self.tag_reads += 1
        return 'b' * 40 if self.move_tag and self.tag_reads > 1 else COMMIT

    def release(self, tag):
        return copy.deepcopy(self.record)

    def create(self, tag, commit, notes):
        self.events.append('create-draft')
        self.record = dict(id=42, tag_name=tag, target_commitish=commit, draft=True,
                           prerelease=False, assets=[])
        return copy.deepcopy(self.record)

    def get_release(self, release_id):
        if not self.record or self.record['id'] != release_id:
            raise ValueError('Unknown release ID')
        return copy.deepcopy(self.record)

    def upload(self, tag, path):
        self.events.append('upload:' + path.name)
        if self.fail_upload == path.name:
            raise RuntimeError('Injected upload failure')
        self.record['assets'].append(dict(name=path.name, state='uploaded',
                                         size=path.stat().st_size, digest='sha256:' + release.digest(path)))

    def publish(self, tag):
        self.events.append('publish')
        self.record['draft'] = False

    def asset_digest(self, asset, directory):
        return asset['digest']


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='koma-release-')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / 'source-root'
        for directory in ('docs', 'assets', 'tools', 'cmake/windows-fast'):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        (self.root / 'CMakeLists.txt').write_text('project(KomaMimic VERSION 1.0.0 LANGUAGES CXX)\n')
        (self.root / 'README.md').write_text('# KomaMimic v1.0.0\n')
        for name in ('vcpkg.json', 'cmake/windows-fast/vcpkg.json'):
            release.save_json(self.root / name, {'version-string': '1.0.0'})
        policy = dict(schema=1, root_files=['CMakeLists.txt', 'README.md', 'vcpkg.json',
                                          'cmake/windows-fast/vcpkg.json'], source_trees={},
                      documents=[], excluded_paths=[], reviewed_assets=[], runtime_sample_data=False)
        release.save_json(self.root / 'tools/public_source.json', policy)
        # Model the Ubuntu canonical checkout even when this suite runs on
        # Windows, where write_text() would otherwise produce native CRLF.
        for path in self.root.rglob('*'):
            if path.is_file():
                path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n'))
        self.contents = {p.relative_to(self.root).as_posix(): p.read_bytes() for p in release.public_files(self.root)}
        self.inputs = self.base / 'inputs'
        self.inputs.mkdir()
        self.source(self.inputs / 'source', self.contents)
        for system in ('windows', 'macos', 'linux'):
            self.artifact(system)
        self.output = self.base / 'bundle'

    def sums(self, directory, names):
        (directory / 'SHA256SUMS').write_text(''.join(
            release.digest(directory / n) + '  ' + n + '\n' for n in names), encoding='ascii')

    def source(self, directory, contents, extras=None):
        directory.mkdir(parents=True, exist_ok=True)
        records = [dict(path=n, sha256=hashlib.sha256(b).hexdigest()) for n, b in sorted(contents.items())]
        manifest = dict(version='1.0.0', distribution='source-only', files=records)
        release.save_json(directory / 'SOURCE_MANIFEST.json', manifest)
        name = 'KomaMimic-1.0.0-source.tar.gz'
        with tarfile.open(directory / name, 'w:gz') as archive:
            for n, data in sorted(contents.items()):
                entry = tarfile.TarInfo('KomaMimic-1.0.0/' + n)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        for n, data in (extras or {}).items():
            (directory / n).write_bytes(data)
        self.sums(directory, [name, 'SOURCE_MANIFEST.json', *(extras or {})])
        return manifest

    def artifact(self, system, contents=None):
        directory = self.inputs / system
        directory.mkdir(exist_ok=True)
        extras = {'libarchive-3.8.10.tar.gz': b'synthetic libarchive source',
                  'poppler-22.12.0-source.tar.gz': b'synthetic Poppler source'} if system == 'linux' else None
        source = self.source(directory / 'sources', contents or self.contents, extras)
        manifest = dict(version='1.0.0', commit=COMMIT, source_tree_dirty=False,
                        source_files=source['files'])
        names = dict(windows=['KomaMimic-1.0.0-windows-x86_64.zip'],
                     macos=['KomaMimic-1.0.0-macos-arm64.app.zip', 'KomaMimic-1.0.0-macos-arm64.dmg', 'HOMEBREW.json'],
                     linux=['KomaMimic-1.0.0-linux-x86_64.AppImage', 'komamimic.desktop', 'komamimic.png'])[system]
        for name in names:
            (directory / name).write_bytes(('Synthetic packaging fixture: ' + name).encode())
        if system == 'linux':
            manifest.update(appimage_sha256=release.digest(directory / names[0]),
                            desktop_integration={n: release.digest(directory / n) for n in names[1:]},
                            source_archives={n: release.digest(directory / 'sources' / n) for n in extras})
        release.save_json(directory / 'BUILD_MANIFEST.json', manifest)
        self.sums(directory, [*names, 'BUILD_MANIFEST.json'])

    def change_manifest(self, system, **values):
        directory = self.inputs / system
        path = directory / 'BUILD_MANIFEST.json'
        record = json.loads(path.read_text())
        record.update(values)
        names = list(release.checksum_records(directory))
        release.save_json(path, record)
        self.sums(directory, names)

    def assemble(self):
        return release.assemble(self.root, self.inputs, self.output, TAG, COMMIT)

    def test_stable_versions_and_mismatched_manifests(self):
        for version in ('1.0.0', '1.1.0', '2.0.0', '10.20.30'):
            (self.root / 'CMakeLists.txt').write_text('project(KomaMimic VERSION ' + version + ' LANGUAGES CXX)\n')
            (self.root / 'README.md').write_text('# v' + version + '\n')
            for name in ('vcpkg.json', 'cmake/windows-fast/vcpkg.json'):
                release.save_json(self.root / name, {'version-string': version})
            self.assertEqual(release.version_for_tag(self.root, 'v' + version), ('v' + version, version))
        for tag in ('v01.0.0', 'v1.0', 'v1.0.0-rc1', 'v1.0.0+meta', 'v1.0.0\nx=1', 'v1.0.0/../../main', '1.0.0'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.version_for_tag(self.root, tag)
        release.save_json(self.root / 'cmake/windows-fast/vcpkg.json', {'version-string': '1.0.0'})
        with self.assertRaisesRegex(ValueError, 'Version mismatch'):
            release.version_for_tag(self.root, 'v10.20.30')

    def test_checkout_and_tag_identity_use_only_read_only_git(self):
        with mock.patch.object(release.subprocess, 'check_output', side_effect=[COMMIT, '', COMMIT]) as git:
            result = release.validate_checkout(self.root, TAG, COMMIT, True)
        self.assertEqual(result['tag'], TAG)
        self.assertEqual([c.args[0][3] for c in git.call_args_list], ['rev-parse', 'status', 'rev-parse'])
        for responses in ([COMMIT, ' M README.md'], ['b' * 40], [COMMIT, '', 'b' * 40]):
            with mock.patch.object(release.subprocess, 'check_output', side_effect=responses), self.assertRaises(ValueError):
                release.validate_checkout(self.root, TAG, COMMIT, True)

    def test_full_assembly_and_linux_zip_payload_and_modes(self):
        result = self.assemble()
        self.assertEqual(result['commit'], COMMIT)
        self.assertEqual(result['windows_source_eol_differences'], [])
        release.verify_bundle(self.output, TAG, COMMIT)
        with zipfile.ZipFile(self.output / 'KomaMimic-1.0.0-linux-x86_64.zip') as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.read('komamimic.desktop'), (self.inputs / 'linux/komamimic.desktop').read_bytes())
            name = 'sources/KomaMimic-1.0.0-source.tar.gz'
            self.assertEqual(archive.read(name), (self.output / 'KomaMimic-1.0.0-source.tar.gz').read_bytes())
            self.assertEqual((archive.getinfo('KomaMimic-1.0.0-linux-x86_64.AppImage').external_attr >> 16) & 0o777, 0o755)
            for line in archive.read('SHA256SUMS').decode().splitlines():
                checksum, name = line.split('  ')
                self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), checksum)

    def test_windows_crlf_uses_only_canonical_application_source(self):
        self.artifact('windows', {n: b.replace(b'\n', b'\r\n') for n, b in self.contents.items()})
        result = self.assemble()
        self.assertEqual(set(result['windows_source_eol_differences']), set(self.contents))
        self.assertEqual((self.output / 'KomaMimic-1.0.0-source.tar.gz').read_bytes(),
                         (self.inputs / 'source/KomaMimic-1.0.0-source.tar.gz').read_bytes())

    def test_os_commit_version_and_dirty_mismatches_prevent_output(self):
        for system, values in [('windows', {'commit': 'b' * 40}), ('macos', {'version': '1.1.0'}),
                               ('linux', {'commit': 'b' * 40}), ('windows', {'source_tree_dirty': True})]:
            with self.subTest(system=system, values=values):
                self.artifact(system)
                self.change_manifest(system, **values)
                with self.assertRaises(ValueError):
                    self.assemble()
                self.assertFalse(self.output.exists())
                self.artifact(system)

    def test_corrupt_missing_or_duplicate_checksum_prevents_output(self):
        directory = self.inputs / 'macos'
        path = directory / 'KomaMimic-1.0.0-macos-arm64.dmg'
        path.write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.assemble()
        self.artifact('macos')
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            self.assemble()
        self.artifact('macos')
        sums = directory / 'SHA256SUMS'
        sums.write_text(sums.read_text() * 2)
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_real_source_changes_are_not_accepted_as_line_endings(self):
        for system in ('windows', 'macos', 'linux'):
            modified = dict(self.contents)
            modified['README.md'] += b'changed content\n'
            self.artifact(system, modified)
            with self.subTest(system=system), self.assertRaisesRegex(ValueError, 'source content differs'):
                self.assemble()
            self.assertFalse(self.output.exists())
            self.artifact(system)
        with self.assertRaises(ValueError):
            release.compare_source({'binary': b'a\0\r\nb'}, {'binary': b'a\0\nb'}, True)

    def test_canonical_source_must_match_checkout(self):
        (self.root / 'README.md').write_text('# KomaMimic v1.0.0\nchanged\n')
        with self.assertRaisesRegex(ValueError, 'source content differs'):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_source_archive_traversal_and_duplicate_members_are_rejected(self):
        directory = self.inputs / 'source'
        path = directory / 'KomaMimic-1.0.0-source.tar.gz'
        for names in (['KomaMimic-1.0.0/../escape'], ['KomaMimic-1.0.0/README.md'] * 2):
            with tarfile.open(path, 'w:gz') as archive:
                for name in names:
                    member = tarfile.TarInfo(name)
                    member.size = 1
                    archive.addfile(member, io.BytesIO(b'x'))
            self.sums(directory, [path.name, 'SOURCE_MANIFEST.json'])
            with self.assertRaises(ValueError):
                release.read_source(directory, '1.0.0')

    def test_missing_dependency_source_is_rejected(self):
        (self.inputs / 'linux/sources/libarchive-3.8.10.tar.gz').unlink()
        with self.assertRaises(ValueError):
            self.assemble()
        self.assertFalse(self.output.exists())

    def test_output_is_never_overwritten(self):
        self.output.mkdir()
        marker = self.output / 'keep.txt'
        marker.write_text('keep')
        with self.assertRaises(FileExistsError):
            self.assemble()
        self.assertEqual(marker.read_text(), 'keep')

    def test_bundle_tampering_and_extra_files_block_publication(self):
        self.assemble()
        for action in ('change', 'extra'):
            path = self.output / ('RELEASE_NOTES.md' if action == 'change' else 'unexpected.log')
            before = path.read_bytes() if path.exists() else None
            path.write_bytes(b'unexpected')
            remote = FakeGitHub()
            with self.subTest(action=action), self.assertRaises(ValueError):
                release.publish(self.output, TAG, COMMIT, remote)
            self.assertEqual(remote.events, [])
            if before is None:
                path.unlink()
            else:
                path.write_bytes(before)

    def test_symlinked_inputs_are_rejected(self):
        directory = self.inputs / 'source'
        path = directory / 'SOURCE_MANIFEST.json'
        original = path.read_bytes()
        path.unlink()
        outside = self.base / 'outside.json'
        outside.write_bytes(original)
        try:
            path.symlink_to(outside)
        except OSError as error:
            if os.name != 'nt':
                raise
            self.skipTest('Windows symlink creation is unavailable: ' + str(error))
        with self.assertRaises(ValueError):
            release.read_source(directory, '1.0.0')

    def test_unsafe_checksum_names_are_rejected(self):
        for name in ('../secret', '/secret', 'C:/secret', 'x\\y', 'a\nb', 'a//b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                release.relative_name(name)

    def test_publish_occurs_only_after_all_asset_hashes_are_verified(self):
        self.assemble()
        remote = FakeGitHub()
        release.publish(self.output, TAG, COMMIT, remote)
        self.assertEqual(remote.events[0], 'create-draft')
        self.assertEqual(remote.events[-1], 'publish')
        self.assertFalse(remote.record['draft'])
        self.assertEqual(len(remote.record['assets']), len(list(self.output.iterdir())))
        events = list(remote.events)
        release.publish(self.output, TAG, COMMIT, remote)
        self.assertEqual(events, remote.events)

    def test_created_draft_survives_missing_immediate_tag_lookup(self):
        self.assemble()
        remote = FakeGitHub()
        # GitHub creation returns the draft, but a subsequent tag/list lookup
        # can still miss it. Keep the create response and verify by its ID.
        remote.release = mock.Mock(return_value=None)
        release.publish(self.output, TAG, COMMIT, remote)
        self.assertFalse(remote.record['draft'])
        self.assertEqual(remote.release.call_count, 1)
        self.assertEqual(remote.events.count('create-draft'), 1)

    def test_failed_upload_stays_draft_and_can_resume_without_replacing_assets(self):
        self.assemble()
        remote = FakeGitHub()
        remote.fail_upload = 'RELEASE_NOTES.md'
        with self.assertRaisesRegex(RuntimeError, 'Injected'):
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertTrue(remote.record['draft'])
        self.assertNotIn('publish', remote.events)
        existing = [a['name'] for a in remote.record['assets']]
        self.assertTrue(existing)
        remote.fail_upload = None
        release.publish(self.output, TAG, COMMIT, remote)
        for name in existing:
            self.assertEqual(remote.events.count('upload:' + name), 1)
        self.assertFalse(remote.record['draft'])

    def test_existing_different_public_release_is_never_modified(self):
        self.assemble()
        remote = FakeGitHub()
        release.publish(self.output, TAG, COMMIT, remote)
        remote.record['assets'][0]['digest'] = 'sha256:' + '0' * 64
        events = list(remote.events)
        with self.assertRaisesRegex(ValueError, 'asset differs'):
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertEqual(events, remote.events)

    def test_moved_tag_never_publishes_completed_draft(self):
        self.assemble()
        remote = FakeGitHub()
        remote.move_tag = True
        with self.assertRaisesRegex(ValueError, 'tag moved'):
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertTrue(remote.record['draft'])
        self.assertNotIn('publish', remote.events)

    def test_uploaded_byte_mismatch_blocks_publication(self):
        self.assemble()
        remote = FakeGitHub()
        upload = remote.upload
        def corrupt_upload(tag, path):
            upload(tag, path)
            remote.record['assets'][-1]['digest'] = 'sha256:' + '0' * 64
        remote.upload = corrupt_upload
        with self.assertRaisesRegex(ValueError, 'asset differs'):
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertTrue(remote.record['draft'])
        self.assertNotIn('publish', remote.events)

    def test_existing_draft_for_another_commit_is_untouched(self):
        self.assemble()
        remote = FakeGitHub()
        remote.create(TAG, 'b' * 40, self.output / 'RELEASE_NOTES.md')
        events = list(remote.events)
        with self.assertRaisesRegex(ValueError, 'another commit'):
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertEqual(remote.events, events)

    def test_annotated_remote_tags_are_peeled_to_commit(self):
        remote = release.GitHub('owner/repository')
        with mock.patch.object(remote, 'api', side_effect=[{'object': {'type': 'tag', 'sha': 'b' * 40}},
                                                         {'object': {'type': 'commit', 'sha': COMMIT}}]):
            self.assertEqual(remote.tag_commit(TAG), COMMIT)

    def test_github_create_preserves_response_and_uses_structured_json(self):
        remote = release.GitHub('owner/repository')
        notes = self.base / 'notes.md'
        text = 'Release notes\n日本語 "quotes", $(literal), `literal`\n'
        notes.write_text(text, encoding='utf-8')
        asset = self.base / 'asset +.zip'
        asset.write_bytes(b'raw binary fixture\0\xff')
        record = dict(id=42, tag_name=TAG, draft=True, prerelease=False)
        result = subprocess.CompletedProcess([], 0, json.dumps(record), '')
        with mock.patch.object(release.subprocess, 'run', return_value=result) as run:
            self.assertEqual(remote.create(TAG, COMMIT, notes), record)
            remote.upload(42, asset)
            remote.publish(42)
        create, upload, publish = [c.args[0] for c in run.call_args_list]
        self.assertEqual(create, ['gh', 'api', 'repos/owner/repository/releases',
                                  '--method', 'POST', '--input', '-', '-H', 'Content-Type: application/json'])
        self.assertEqual(json.loads(run.call_args_list[0].kwargs['input']), dict(
            tag_name=TAG, target_commitish=COMMIT, draft=True, prerelease=False,
            name='KomaMimic ' + TAG, body=text))
        self.assertEqual(upload[2], 'https://uploads.github.com/repos/owner/repository/releases/42/assets?name=asset%20%2B.zip')
        self.assertEqual(upload[3:], ['--method', 'POST', '--input', str(asset),
                                     '-H', 'Content-Type: application/octet-stream',
                                     '-H', 'Content-Length: ' + str(asset.stat().st_size)])
        self.assertNotIn('--clobber', upload)
        self.assertEqual(publish, ['gh', 'api', 'repos/owner/repository/releases/42',
                                  '--method', 'PATCH', '--input', '-', '-H', 'Content-Type: application/json'])
        self.assertEqual(json.loads(run.call_args_list[2].kwargs['input']), {'draft': False})
        self.assertNotIn('--latest', publish)

    def test_missing_remote_digest_is_checked_using_downloaded_bytes(self):
        remote = release.GitHub('owner/repository')
        def download(command, **kwargs):
            kwargs['stdout'].write(b'asset bytes')
        with mock.patch.object(release.subprocess, 'run', side_effect=download):
            result = remote.asset_digest({'id': 42}, self.base)
        self.assertEqual(result, 'sha256:' + hashlib.sha256(b'asset bytes').hexdigest())

    def test_api_errors_are_not_treated_as_an_absent_release(self):
        remote = release.GitHub('owner/repository')
        for message in ('not found (HTTP 404)', 'forbidden (HTTP 403)', 'network error'):
            response = subprocess.CompletedProcess([], 1, '', message)
            with mock.patch.object(release.subprocess, 'run', return_value=response):
                if '404' in message:
                    self.assertIsNone(remote.api('/releases/tags/' + TAG, missing=True))
                else:
                    with self.assertRaises(RuntimeError):
                        remote.release(TAG)

    def test_draft_lookup_falls_back_to_paginated_authenticated_listing(self):
        remote = release.GitHub('owner/repository')
        others = [{'tag_name': 'v0.0.' + str(i)} for i in range(100)]
        draft = dict(id=42, tag_name=TAG, draft=True, target_commitish='main', assets=[])
        with mock.patch.object(remote, 'api', side_effect=[None, others, [draft]]) as api:
            self.assertEqual(remote.release(TAG), draft)
        self.assertEqual(api.call_args_list[-1].args[0], '/releases?per_page=100&page=2')
        with mock.patch.object(remote, 'api', side_effect=[None, [draft, draft]]), self.assertRaisesRegex(ValueError, 'Multiple'):
            remote.release(TAG)

    def test_branch_commitish_metadata_uses_remote_tag_identity(self):
        self.assemble()
        remote = FakeGitHub()
        remote.create(TAG, 'main', self.output / 'RELEASE_NOTES.md')
        release.publish(self.output, TAG, COMMIT, remote)
        self.assertFalse(remote.record['draft'])

    def test_manual_event_cannot_invoke_publication_even_from_a_tag(self):
        argv = ['release.py', 'publish', '--tag', TAG, '--commit', COMMIT,
                '--bundle', str(self.output), '--repo', 'owner/repository']
        with mock.patch.object(sys, 'argv', argv), mock.patch.dict(os.environ, {
                'GITHUB_EVENT_NAME': 'workflow_dispatch', 'GITHUB_REF': 'refs/tags/' + TAG}), \
                mock.patch.object(release, 'GitHub') as github, self.assertRaisesRegex(ValueError, 'only enabled'):
            release.main()
        github.assert_not_called()

    def test_get_release_uses_exact_numeric_id_and_rejects_wrong_response(self):
        remote = release.GitHub('owner/repository')
        with mock.patch.object(remote, 'api', return_value={'id': 42}) as api:
            self.assertEqual(remote.get_release(42), {'id': 42})
            api.assert_called_once_with('/releases/42')
        with mock.patch.object(remote, 'api', return_value={'id': 43}), self.assertRaisesRegex(ValueError, 'ID changed'):
            remote.get_release(42)
        for value in (0, -1, True, '42', '../42'):
            with self.subTest(value=value), mock.patch.object(remote, 'api') as api, self.assertRaisesRegex(ValueError, 'Invalid release ID'):
                remote.get_release(value)
            api.assert_not_called()

    def test_missing_created_id_stops_before_upload(self):
        self.assemble()
        remote = FakeGitHub()
        def create(tag, commit, notes):
            return dict(tag_name=TAG, draft=True, prerelease=False, assets=[])
        remote.create = create
        with self.assertRaisesRegex(ValueError, 'Missing release ID'):
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertEqual(remote.events, [])

    def test_resume_uses_only_existing_id_and_keeps_uploaded_assets(self):
        self.assemble()
        remote = FakeGitHub()
        remote.create(TAG, COMMIT, self.output / 'RELEASE_NOTES.md')
        existing = 'RELEASE_NOTES.md'
        remote.upload(42, self.output / existing)
        remote.release = mock.Mock(side_effect=AssertionError('Must not search by tag in recovery'))
        release.publish(self.output, TAG, COMMIT, remote, existing_release_id=42)
        self.assertEqual(remote.events.count('create-draft'), 1)
        self.assertEqual(remote.events.count('upload:' + existing), 1)
        self.assertFalse(remote.record['draft'])

    def test_resume_refuses_missing_draft_wrong_tag_and_wrong_commit(self):
        self.assemble()
        remote = FakeGitHub()
        with self.assertRaisesRegex(ValueError, 'Unknown release ID'):
            release.publish(self.output, TAG, COMMIT, remote, existing_release_id=42)
        self.assertEqual(remote.events, [])
        for tag, commit in (('v1.1.0', COMMIT), (TAG, 'b' * 40)):
            remote.create(tag, commit, self.output / 'RELEASE_NOTES.md')
            events = list(remote.events)
            with self.subTest(tag=tag, commit=commit), self.assertRaises(ValueError):
                release.publish(self.output, TAG, COMMIT, remote, existing_release_id=42)
            self.assertEqual(remote.events, events)

    def test_explicit_resume_can_use_older_bundle_without_local_head_or_event_override(self):
        self.assemble()
        remote = FakeGitHub()
        remote.create(TAG, COMMIT, self.output / 'RELEASE_NOTES.md')
        argv = ['release.py', 'resume', '--bundle', str(self.output), '--repo', 'owner/repository', '--release-id', '42']
        with mock.patch.object(sys, 'argv', argv), mock.patch.object(release, 'GitHub', return_value=remote), \
                mock.patch.object(release, 'validate_checkout', side_effect=AssertionError('Old bundle is independent of helper checkout')):
            release.main()
        self.assertFalse(remote.record['draft'])

    def test_failed_creation_api_is_not_misreported_as_absent_release(self):
        remote = release.GitHub('owner/repository')
        response = subprocess.CompletedProcess([], 1, '', 'not found (HTTP 404)')
        with mock.patch.object(release.subprocess, 'run', return_value=response), self.assertRaisesRegex(RuntimeError, 'API failed'):
            remote.api('/releases', missing=True, method='POST', data={})

    def test_upload_refuses_unexpected_host_before_invoking_cli(self):
        remote = release.GitHub('owner/repository')
        with mock.patch.object(release.subprocess, 'run') as run, self.assertRaisesRegex(ValueError, 'upload endpoint'):
            remote.api('https://example.com/repos/owner/repository/assets', method='POST', input_file=Path('asset.zip'))
        run.assert_not_called()

    def test_rest_adapter_end_to_end_when_draft_indexes_are_empty(self):
        self.assemble()
        record, requests = {}, []
        def api_server(command, **options):
            self.assertEqual(command[:2], ['gh', 'api'])
            endpoint = command[2]
            method = command[command.index('--method') + 1]
            requests.append((method, endpoint))
            if endpoint.endswith('/git/ref/tags/' + TAG):
                result = {'object': {'type': 'commit', 'sha': COMMIT}}
            elif endpoint.endswith('/releases/tags/' + TAG):
                if not record or record['draft']:
                    return subprocess.CompletedProcess(command, 1, '', 'not found (HTTP 404)')
                result = record
            elif endpoint.endswith('/releases?per_page=100&page=1'):
                result = []  # The just-created draft is absent from this index.
            elif method == 'POST' and endpoint.endswith('/releases'):
                self.assertFalse(record)
                record.update(json.loads(options['input']), id=42, assets=[])
                result = record
            elif method == 'POST' and '/releases/42/assets?' in endpoint:
                path = Path(command[command.index('--input') + 1])
                self.assertIn('Content-Length: ' + str(path.stat().st_size), command)
                record['assets'].append(dict(name=path.name, state='uploaded', size=path.stat().st_size,
                                              digest='sha256:' + release.digest(path)))
                result = record['assets'][-1]
            elif endpoint.endswith('/releases/42'):
                if method == 'PATCH':
                    record.update(json.loads(options['input']))
                result = record
            else:
                self.fail('Unexpected API request: ' + repr(command))
            return subprocess.CompletedProcess(command, 0, json.dumps(result), '')
        remote = release.GitHub('owner/repository')
        with mock.patch.object(release.subprocess, 'run', side_effect=api_server):
            release.publish(self.output, TAG, COMMIT, remote)
            self.assertFalse(record['draft'])
            mutations = [r for r in requests if r[0] != 'GET']
            self.assertEqual(mutations[-1], ('PATCH', 'repos/owner/repository/releases/42'))
            self.assertEqual(len(record['assets']), len(list(self.output.iterdir())))
            release.publish(self.output, TAG, COMMIT, remote)
        self.assertEqual([r for r in requests if r[0] != 'GET'], mutations)


if __name__ == '__main__':
    unittest.main(verbosity=2)
