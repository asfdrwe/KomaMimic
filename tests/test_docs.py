# SPDX-License-Identifier: GPL-3.0-only
"""The v2 guide must not replace v1 files or their existing URLs."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_docs


class DocumentationTests(unittest.TestCase):
    def test_v1_routes_and_assets_survive_combining_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            v1, v2, output = (root / name for name in ('old', 'new', 'site'))
            for name in ('index.html', 'user/documents/index.html', 'en/user/install/index.html',
                         'assets/screenshots/test.png', 'search/search_index.json'):
                file = v1 / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(('v1 original ' + name).encode())
            (v2 / 'user/video').mkdir(parents=True)
            (v2 / 'index.html').write_text('v2 only')
            (v2 / 'user/video/index.html').write_text('v2 video')
            build_docs.combine(v1, v2, output)
            for file in v1.rglob('*'):
                if file.is_file():
                    for prefix in (output, output / 'v1'):
                        self.assertEqual((prefix / file.relative_to(v1)).read_bytes(), file.read_bytes())
            self.assertEqual((output / 'v2/index.html').read_text(), 'v2 only')
            self.assertTrue((output / 'v2/user/video/index.html').is_file())
            self.assertFalse((output / 'user/video').exists())
            with self.assertRaises(ValueError):
                build_docs.combine(v1, v2, output)

    def test_moved_v1_tag_is_rejected_before_archive_or_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.object(build_docs.subprocess, 'check_output', return_value='0' * 40):
                with self.assertRaisesRegex(ValueError, 'pinned commit'):
                    build_docs.frozen_v1(Path(temporary), Path(temporary) / 'v1')
            self.assertEqual(list(Path(temporary).iterdir()), [])

    @unittest.skipUnless(importlib.util.find_spec('mkdocs'), 'MkDocs integration runs in the documentation job')
    def test_both_real_mkdocs_builds_have_separate_navigation_and_search(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for major in (1, 2):
                source = root / ('source' + str(major))
                (source / 'docs').mkdir(parents=True)
                (source / 'docs/index.md').write_text('# v' + str(major) + ' marker\n', encoding='utf-8')
                (source / 'mkdocs.yml').write_text('site_name: Test\ndocs_dir: docs\nnav:\n  - Home: index.md\n', encoding='utf-8')
                build_docs.build_guide(source, root / ('html' + str(major)), root / (str(major) + '.yml'),
                                       major, 'https://example.test/KomaMimic/')
            build_docs.combine(root / 'html1', root / 'html2', root / 'site')
            self.assertIn('v1 marker', (root / 'site/index.html').read_text())
            self.assertNotIn('v2 marker', (root / 'site/index.html').read_text())
            self.assertIn('v2 marker', (root / 'site/v2/index.html').read_text())
            self.assertEqual((root / 'site/search/search_index.json').read_bytes(),
                             (root / 'site/v1/search/search_index.json').read_bytes())
