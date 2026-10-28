#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test public selection, archive contents, and synthetic reader fixtures."""
import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from export_source import export, load_policy, public_files
from check_source import check, check_tracked
from prepare_release import prepare

WORKER = None
PROBE = None
PRIVATE_MARKER = b'PRIVATE ' + b'SENTINEL'


def load_tests(loader, tests, pattern):
    from test_validation import ValidationTests
    ValidationTests.probe = PROBE
    tests.addTests(loader.loadTestsFromTestCase(ValidationTests))
    return tests


class DistributionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = tempfile.TemporaryDirectory(prefix='koma-public-base-')
        cls.source = Path(cls.base.name).resolve() / 'source'
        export(ROOT, cls.source)

    @classmethod
    def tearDownClass(cls):
        cls.base.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='koma-distribution-')
        self.addCleanup(self.temporary.cleanup)
        # public_files() resolves its root; macOS /var aliases /private/var.
        self.directory = Path(self.temporary.name).resolve()
        self.project = self.directory / 'project'
        shutil.copytree(self.source, self.project)

    def write(self, name, data=None):
        if data is None:
            data = PRIVATE_MARKER + b'\n'
        path = self.project / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_private_and_generated_data_never_exported(self):
        for name in ['tmp/book.epub', 'samples/picture.png', 'internal/notes.md',
                     'tests/fixtures/generated/README.txt', 'tests/fixtures/generated/data.json',
                     'tests/fixtures/generated/source.cpp', 'build/diagnostic.txt',
                     'tests/fixtures/unreviewed-book.txt', 'tests/fixtures/unreviewed.json',
                     'tests/fixtures/unreviewed-image.png']:
            self.write(name)
        output = self.directory / 'public'
        export(self.project, output)
        for path in output.rglob('*'):
            if path.is_file():
                self.assertNotIn(PRIVATE_MARKER, path.read_bytes())
        self.assertFalse((output / 'samples').exists())
        self.assertFalse((output / 'tests/fixtures/generated').exists())

    def test_git_tracked_unreviewed_data_is_rejected(self):
        if not shutil.which('git'):
            self.skipTest('Git is unavailable; export/archive checks still run')
        self.write('tests/fixtures/unreviewed-book.txt')
        subprocess.run(['git', 'init', '-q', str(self.project)], check=True, capture_output=True)
        subprocess.run(['git', '-C', str(self.project), 'add', '-f', '--', 'CMakeLists.txt'], check=True, capture_output=True)
        check_tracked(self.project)
        subprocess.run(['git', '-C', str(self.project), 'add', '-f', '--', 'tests/fixtures/unreviewed-book.txt'], check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, 'Tracked files outside'):
            check_tracked(self.project)

    def test_unclassified_document_stops_export_before_writing(self):
        self.write('docs/not-reviewed.md')
        output = self.directory / 'public'
        with self.assertRaisesRegex(ValueError, 'Unclassified'):
            export(self.project, output)
        self.assertFalse(output.exists())

    def test_asset_tampering_stops_export_before_writing(self):
        asset = load_policy(self.project)['reviewed_assets'][0]['path']
        with (self.project / asset).open('ab') as stream:
            stream.write(PRIVATE_MARKER)
        output = self.directory / 'public'
        with self.assertRaisesRegex(ValueError, 'asset hash mismatch'):
            export(self.project, output)
        self.assertFalse(output.exists())

    def test_unclassified_asset_stops_export_before_writing(self):
        self.write('assets/icons/unreviewed.png')
        output = self.directory / 'public'
        with self.assertRaisesRegex(ValueError, 'Unclassified public asset'):
            export(self.project, output)
        self.assertFalse(output.exists())

    def test_policy_rejects_traversal(self):
        path = self.project / 'tools/public_source.json'
        policy = load_policy(self.project)
        for name in ['docs/../../private.md', 'C:/private.txt', 'docs' + chr(92) + 'private.md', 'docs' + chr(0) + 'private.md']:
            with self.subTest(path=name):
                candidate = dict(policy)
                candidate['documents'] = policy['documents'] + [name]
                path.write_text(json.dumps(candidate), encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'Invalid policy path'):
                    public_files(self.project)

    def test_public_private_overlap_is_rejected(self):
        self.write('tmp/secret.txt')
        policy = load_policy(self.project)
        policy['root_files'].append('tmp/secret.txt')
        (self.project / 'tools/public_source.json').write_text(json.dumps(policy), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'overlap'):
            public_files(self.project)

    def test_selected_symlink_is_rejected(self):
        secret = self.directory / 'secret.txt'
        secret.write_bytes(PRIVATE_MARKER)
        try:
            (self.project / 'src/unreviewed.cpp').symlink_to(secret)
        except (OSError, NotImplementedError):
            self.skipTest('Symlink creation is unavailable for this account')
        with self.assertRaisesRegex(ValueError, 'Symlink'):
            public_files(self.project)

    def test_missing_required_source_is_rejected(self):
        (self.project / 'LICENSE').unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            public_files(self.project)

    def test_manifest_matches_source_and_export_is_self_contained(self):
        output = self.directory / 'public'
        manifest = self.directory / 'manifest.json'
        count = export(self.project, output, manifest)
        records = json.loads(manifest.read_text(encoding='utf-8'))['files']
        self.assertEqual(len(records), count)
        self.assertEqual({p.relative_to(output).as_posix() for p in public_files(output)},
                         {entry['path'] for entry in records})
        for entry in records:
            data = (output / entry['path']).read_bytes()
            self.assertEqual(data, (self.project / entry['path']).read_bytes())
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'])
        with contextlib.redirect_stdout(io.StringIO()):
            check(output)

    def test_existing_outputs_are_never_overwritten(self):
        output = self.directory / 'public'
        output.mkdir()
        marker = output / 'keep.txt'
        marker.write_text('keep', encoding='utf-8')
        with self.assertRaises(FileExistsError):
            export(self.project, output)
        self.assertEqual(marker.read_text(encoding='utf-8'), 'keep')
        manifest = self.directory / 'manifest.json'
        manifest.write_text('keep', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'new file'):
            export(self.project, self.directory / 'another-output', manifest)
        self.assertFalse((self.directory / 'another-output').exists())
        with self.assertRaisesRegex(ValueError, 'outside'):
            export(self.project, self.directory / 'not-created',
                   self.project / 'tools/../src/export-inventory.json')
        self.assertFalse((self.directory / 'not-created').exists())

    def test_source_archive_has_exact_public_members_and_is_reproducible(self):
        self.write('tmp/book.epub')
        first, second = self.directory / 'release-a', self.directory / 'release-b'
        with contextlib.redirect_stdout(io.StringIO()):
            prepare(self.project, first, 0)
            prepare(self.project, second, 0)
        for path in first.iterdir():
            self.assertEqual(path.read_bytes(), (second / path.name).read_bytes())
        manifest = json.loads((first / 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
        records = {entry['path']: entry['sha256'] for entry in manifest['files']}
        self.assertEqual(set(records), {p.relative_to(self.project).as_posix() for p in public_files(self.project)})
        with tarfile.open(next(first.glob('*.tar.gz'))) as archive:
            members = archive.getmembers()
            self.assertEqual(len(members), len(records))
            for member in members:
                self.assertTrue(member.isfile())
                relative = member.name.split('/', 1)[1]
                data = archive.extractfile(member).read()
                self.assertEqual(hashlib.sha256(data).hexdigest(), records[relative])
                self.assertNotIn(PRIVATE_MARKER, data)

    def test_public_fixture_generator_and_reference_pixels(self):
        generator = self.project / 'tests/fixtures/generate_script_samples.py'
        first, second = self.directory / 'fixtures-a', self.directory / 'fixtures-b'
        command = [sys.executable, str(generator), '--output', str(first)]
        if WORKER:
            command += ['--verify-worker', str(WORKER)]
        result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)
        if WORKER:
            self.assertEqual(result.stdout.count('Verified every RGBA pixel:'), 12)
        subprocess.run([sys.executable, str(generator), '--output', str(second)],
                       check=True, capture_output=True, timeout=60)
        files = sorted(path.name for path in first.iterdir())
        self.assertEqual(len(files), 25)  # 12 encoded files, 12 reference PNGs, README.
        self.assertEqual(files, sorted(path.name for path in second.iterdir()))
        for name in files:
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes())
        self.assertEqual(first.with_suffix('.zip').read_bytes(), second.with_suffix('.zip').read_bytes())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, help='built script worker; enables pixel-by-pixel decoder verification')
    parser.add_argument('--probe', type=Path, help='built reader probe for generated manual fixtures')
    args = parser.parse_args()
    WORKER = args.worker.resolve() if args.worker else None
    PROBE = args.probe.resolve() if args.probe else None
    unittest.main(argv=[sys.argv[0]], verbosity=2)
