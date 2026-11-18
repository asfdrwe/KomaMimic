#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check decoded video/audio using an already deployed validation probe."""
import json
from pathlib import Path
import subprocess
import sys
from isolated_environment import isolated_environment

ROOT = Path(__file__).resolve().parents[1]


def read_report(path, policy):
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(data, dict):
            raise ValueError('Expected a JSON object')
        return data
    except (OSError, ValueError) as error:
        raise RuntimeError('Missing or invalid deployed video report: ' + policy +
                           '; ' + str(path) + '; see ' + str(path.with_suffix('.log'))) from error


def verify_videos(probe, fixtures, output, env, *, generate_fixtures=True):
    fixtures, output = Path(fixtures).resolve(), Path(output).resolve()
    # Fixture generation uses the host's tools. The probe below deliberately
    # uses the packager's isolated runtime environment instead.
    with (output / 'video-fixtures.log').open('w', encoding='utf-8') as log:
        if generate_fixtures:
            generated = subprocess.run([sys.executable, str(ROOT / 'tests/fixtures/generate_video_samples.py'),
                                        '--output', str(fixtures)], stdout=log, stderr=subprocess.STDOUT,
                                       check=False, timeout=180,
                                       env=isolated_environment(output / 'video-environments' / 'fixtures'))
            if generated.returncode:
                raise RuntimeError('Video fixture generation failed; see video-fixtures.log')
        else:
            log.write('Using pre-generated video fixtures: ' + str(fixtures) + '\n')
            if not (fixtures / 'sample.mp4').is_file():
                raise ValueError('Prepared video sample is missing: ' + str(fixtures / 'sample.mp4'))
    for policy in ('automatic', 'software'):
        runtime = isolated_environment(output / 'video-environments' / policy, env)
        runtime['QT_MEDIA_BACKEND'] = 'ffmpeg'
        runtime.pop('QT_FFMPEG_DECODING_HW_DEVICE_TYPES', None)
        if policy == 'software':
            runtime['QT_FFMPEG_DECODING_HW_DEVICE_TYPES'] = ','
        name = 'video-' + policy
        paths = output / (name + '-paths.json')
        paths.unlink(missing_ok=True)
        with (output / (name + '-paths-stdout.log')).open('w', encoding='utf-8') as stdout, (output / (name + '-paths.log')).open('w', encoding='utf-8') as stderr:
            checked = subprocess.run([str(probe), '--standard-paths', '--output', str(paths)], env=runtime, cwd=output,
                                     stdout=stdout, stderr=stderr, check=False, timeout=30)
        if checked.returncode:
            raise RuntimeError('Deployed probe directory check failed: ' + policy)
        locations = read_report(paths, policy)
        private = Path(runtime['KOMAMIMIC_TEST_ROOT']).resolve()
        for key in ('cache', 'config', 'data', 'generic_data', 'temp'):
            location = locations.get(key)
            if not location or not Path(location).resolve().is_relative_to(private):
                raise ValueError('OS directory is outside the video test environment: ' + key)
        report = output / (name + '.json')
        report.unlink(missing_ok=True)
        with (output / (name + '-stdout.log')).open('w', encoding='utf-8') as stdout, (output / (name + '.log')).open('w', encoding='utf-8') as stderr:
            result = subprocess.run([str(probe), '--video', str(fixtures), '--output', str(report)], env=runtime, cwd=output,
                                    stdout=stdout, stderr=stderr, check=False, timeout=30)
        if result.returncode:
            raise RuntimeError('Deployed video probe failed: ' + policy)
        data = read_report(report, policy)
        if (data.get('all_passed') is not True or data.get('video_decoded') is not True
                or data.get('audio_decoded') is not True or data.get('frames', 0) < 3
                or data.get('audio_buffers', 0) < 2):
            raise ValueError('Deployed video/audio decoding did not pass: ' + policy)
