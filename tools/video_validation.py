#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check decoded video/audio using an already deployed validation probe."""
import json
from pathlib import Path
import subprocess
import sys
from isolated_environment import isolated_environment

ROOT = Path(__file__).resolve().parents[1]


def verify_videos(probe, fixtures, output, env):
    fixtures, output = Path(fixtures), Path(output)
    # Fixture generation uses the host's tools. The probe below deliberately
    # uses the packager's isolated runtime environment instead.
    with (output / 'video-fixtures.log').open('w', encoding='utf-8') as log:
        generated = subprocess.run([sys.executable, str(ROOT / 'tests/fixtures/generate_video_samples.py'),
                                    '--output', str(fixtures)], stdout=log, stderr=subprocess.STDOUT,
                                   check=False, timeout=180,
                                   env=isolated_environment(output / 'video-environments' / 'fixtures'))
    if generated.returncode:
        raise RuntimeError('Video fixture generation failed; see video-fixtures.log')
    for policy in ('automatic', 'software'):
        runtime = isolated_environment(output / 'video-environments' / policy, env)
        runtime['QT_MEDIA_BACKEND'] = 'ffmpeg'
        runtime.pop('QT_FFMPEG_DECODING_HW_DEVICE_TYPES', None)
        if policy == 'software':
            runtime['QT_FFMPEG_DECODING_HW_DEVICE_TYPES'] = ','
        name = 'video-' + policy
        paths = output / (name + '-paths.json')
        with paths.open('w', encoding='utf-8') as stdout, (output / (name + '-paths.log')).open('w', encoding='utf-8') as stderr:
            checked = subprocess.run([str(probe), '--standard-paths'], env=runtime, cwd=output,
                                     stdout=stdout, stderr=stderr, check=False, timeout=30)
        if checked.returncode:
            raise RuntimeError('Deployed probe directory check failed: ' + policy)
        locations = json.loads(paths.read_text(encoding='utf-8'))
        private = Path(runtime['KOMAMIMIC_TEST_ROOT']).resolve()
        for key in ('cache', 'config', 'data', 'generic_data', 'temp'):
            location = locations.get(key)
            if not location or not Path(location).resolve().is_relative_to(private):
                raise ValueError('OS directory is outside the video test environment: ' + key)
        report = output / (name + '.json')
        with report.open('w', encoding='utf-8') as stdout, (output / (name + '.log')).open('w', encoding='utf-8') as stderr:
            result = subprocess.run([str(probe), '--video', str(fixtures)], env=runtime, cwd=output,
                                    stdout=stdout, stderr=stderr, check=False, timeout=30)
        if result.returncode:
            raise RuntimeError('Deployed video probe failed: ' + policy)
        data = json.loads(report.read_text(encoding='utf-8'))
        if (data.get('all_passed') is not True or data.get('video_decoded') is not True
                or data.get('audio_decoded') is not True or data.get('frames', 0) < 3
                or data.get('audio_buffers', 0) < 2):
            raise ValueError('Deployed video/audio decoding did not pass: ' + policy)
