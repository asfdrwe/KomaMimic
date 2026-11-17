# SPDX-License-Identifier: GPL-3.0-only
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import video_validation as video


class VideoValidationTests(unittest.TestCase):
    def run_probe(self, data=None, code=0):
        temporary = tempfile.TemporaryDirectory(prefix='video-validation-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        seen = []
        def run(command, **options):
            seen.append((command, options))
            if '--standard-paths' in command:
                options['stdout'].write(json.dumps(dict.fromkeys(
                    ('cache', 'config', 'data', 'generic_data', 'temp'), options['env']['HOME'])))
                return mock.Mock(returncode=0)
            if '--video' in command:
                options['stdout'].write(json.dumps(data if data is not None else dict(
                    all_passed=True, video_decoded=True, audio_decoded=True, frames=3, audio_buffers=2)))
                return mock.Mock(returncode=code)
            return mock.Mock(returncode=0)
        return root, seen, run

    def test_both_policies_use_deployed_probe_and_isolated_environment(self):
        root, seen, run = self.run_probe()
        with mock.patch.object(video.subprocess, 'run', side_effect=run):
            video.verify_videos(root / 'app/probe', root / 'fixtures', root,
                                dict(PATH='isolated', QT_FFMPEG_DECODING_HW_DEVICE_TYPES='vulkan'))
        self.assertEqual(len(seen), 5)
        self.assertTrue(Path(seen[0][1]['env']['HOME']).is_relative_to(root))
        self.assertNotEqual(seen[0][1]['env']['PATH'], 'isolated')  # host fixture tools
        self.assertEqual(seen[1][0][0], str(root / 'app/probe'))
        self.assertNotIn('QT_FFMPEG_DECODING_HW_DEVICE_TYPES', seen[1][1]['env'])
        self.assertEqual(seen[3][1]['env']['QT_FFMPEG_DECODING_HW_DEVICE_TYPES'], ',')
        self.assertEqual(seen[3][1]['env']['PATH'], 'isolated')
        for command, options in seen:
            for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME',
                        'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'TMPDIR', 'TMP', 'TEMP'):
                self.assertTrue(Path(options['env'][key]).is_relative_to(root), key)
        self.assertNotEqual(seen[1][1]['env']['HOME'], seen[3][1]['env']['HOME'])

    def test_probe_outside_private_directories_fails_before_decode(self):
        root, seen, normal = self.run_probe()
        def run(command, **options):
            if '--standard-paths' in command:
                options['stdout'].write(json.dumps(dict.fromkeys(
                    ('cache', 'config', 'data', 'generic_data', 'temp'), str(root / 'normal-profile'))))
                return mock.Mock(returncode=0)
            return normal(command, **options)
        with mock.patch.object(video.subprocess, 'run', side_effect=run), self.assertRaisesRegex(ValueError, 'outside'):
            video.verify_videos(root / 'probe', root / 'fixtures', root, {})
        self.assertFalse(any('--video' in command for command, options in seen))

    def test_loaded_or_audio_only_is_not_a_success(self):
        for data in (dict(all_passed=True),
                     dict(all_passed=True, video_decoded=False, audio_decoded=True, frames=0, audio_buffers=10),
                     dict(all_passed=True, video_decoded=True, audio_decoded=False, frames=10, audio_buffers=0)):
            with self.subTest(data=data):
                root, seen, run = self.run_probe(data)
                with mock.patch.object(video.subprocess, 'run', side_effect=run), self.assertRaises(ValueError):
                    video.verify_videos(root / 'probe', root / 'fixtures', root, {})

    def test_crash_and_timeout_fail_verification(self):
        root, seen, run = self.run_probe(code=7)
        with mock.patch.object(video.subprocess, 'run', side_effect=run), self.assertRaises(RuntimeError):
            video.verify_videos(root / 'probe', root / 'fixtures', root, {})
        with mock.patch.object(video.subprocess, 'run', side_effect=subprocess.TimeoutExpired('probe', 30)), self.assertRaises(subprocess.TimeoutExpired):
            video.verify_videos(root / 'probe', root / 'fixtures', root, {})
