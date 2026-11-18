# SPDX-License-Identifier: GPL-3.0-only
"""Download failures must be detected before native builds without weakening hashes."""
import hashlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock
import urllib.error
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import fetch_release_inputs as inputs


class ReleaseInputTests(unittest.TestCase):
    def entry(self, data=b'pinned release source'):
        return dict(url='https://primary.invalid/source', mirrors=['https://mirror.invalid/source'],
                    sha256=hashlib.sha256(data).hexdigest())

    def test_403_uses_the_next_official_source_without_sleeping(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'source'
            error = urllib.error.HTTPError(self.entry()['url'], 403, 'Forbidden', {}, None)
            with (mock.patch.object(inputs.urllib.request, 'urlopen', side_effect=[error, io.BytesIO(b'pinned release source')]) as fetch,
                  mock.patch.object(inputs.time, 'sleep') as sleep):
                inputs.download(self.entry(), target)
            self.assertEqual([call.args[0].full_url for call in fetch.call_args_list],
                             [self.entry()['url'], self.entry()['mirrors'][0]])
            self.assertEqual(target.read_bytes(), b'pinned release source')
            sleep.assert_not_called()

    def test_transient_failure_is_retried_but_attempts_are_bounded(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'source'
            entry = self.entry()
            entry.pop('mirrors')
            with (mock.patch.object(inputs.urllib.request, 'urlopen', side_effect=urllib.error.URLError('timeout')) as fetch,
                  mock.patch.object(inputs.time, 'sleep')):
                with self.assertRaisesRegex(RuntimeError, 'Cannot download pinned input'):
                    inputs.download(entry, target)
            self.assertEqual(fetch.call_count, 3)
            self.assertFalse(target.exists())

    def test_checksum_failure_cannot_be_hidden_by_mirror_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'source'
            with mock.patch.object(inputs.urllib.request, 'urlopen', return_value=io.BytesIO(b'changed source')) as fetch:
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    inputs.download(self.entry(), target)
            self.assertEqual(fetch.call_count, 1)
            self.assertFalse(target.exists())

    def test_partial_download_is_removed_before_retry(self):
        class Interrupted(io.BytesIO):
            def read(self, *args):
                if self.tell():
                    raise ConnectionResetError('interrupted')
                return super().read(3)
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'source'
            with (mock.patch.object(inputs.urllib.request, 'urlopen', side_effect=[Interrupted(b'partial'), io.BytesIO(b'pinned release source')]),
                  mock.patch.object(inputs.time, 'sleep')):
                inputs.download(self.entry(), target)
            self.assertEqual(target.read_bytes(), b'pinned release source')

    def test_prepared_inputs_are_verified_offline_and_missing_or_changed_inputs_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'source'
            with (mock.patch.object(inputs, 'configuration', return_value={'source': self.entry()}),
                  mock.patch.object(inputs.urllib.request, 'urlopen') as fetch):
                with self.assertRaises(FileNotFoundError):
                    inputs.prepare(root, verify_only=True)
                target.write_bytes(b'changed source')
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    inputs.prepare(root, verify_only=True)
                target.write_bytes(b'pinned release source')
                inputs.prepare(root, verify_only=True)
            fetch.assert_not_called()

    def test_all_four_inputs_are_pinned_and_names_match_the_build_consumers(self):
        config = inputs.configuration()
        self.assertEqual(set(config), {'libarchive-3.8.11.tar.gz', 'patchelf-0.19.2.tar.bz2', 'appimagetool', 'runtime'})
        for name, entry in config.items():
            self.assertRegex(entry['sha256'], '^[a-f0-9]{64}$')
            self.assertTrue(entry['url'].startswith('https://'))
        self.assertEqual(config['libarchive-3.8.11.tar.gz']['sha256'],
                         '900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b')
