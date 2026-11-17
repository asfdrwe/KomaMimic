#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate original, small video inputs for tests; never used by the app."""
import argparse
import json
from pathlib import Path
import subprocess


def generate(output, ffmpeg):
    output.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([ffmpeg, '-version'], text=True).splitlines()[0]
    commands = []

    def encode(name, args):
        command = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', *args, str(output / name)]
        subprocess.run(command, check=True, timeout=60)
        commands.append(dict(file=name, args=args))

    colors = ['-f', 'lavfi', '-i', 'color=red:s=96x64:r=10:d=2',
              '-f', 'lavfi', '-i', 'color=green:s=96x64:r=10:d=2',
              '-f', 'lavfi', '-i', 'color=blue:s=96x64:r=10:d=2']
    concat = ['-filter_complex', '[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]', '-map', '[v]']
    encode('colors.avi', colors + concat + ['-c:v', 'mjpeg', '-q:v', '2', '-threads', '1'])
    encode('sample.mp4', ['-i', str(output / 'colors.avi'), '-f', 'lavfi', '-i',
                         'sine=frequency=440:sample_rate=48000:duration=6', '-c:v', 'mpeg4',
                         '-q:v', '2', '-c:a', 'aac', '-threads', '1', '-shortest'])
    for name, text in [('first.srt', '日本語 track one'), ('second.srt', 'English track two')]:
        (output / name).write_text('1\n00:00:00,100 --> 00:00:05,800\n' + text + '\n', encoding='utf-8')
    inputs = ['-f', 'lavfi', '-i', 'color=red:s=96x64:r=10:d=6',
              '-f', 'lavfi', '-i', 'color=blue:s=96x64:r=10:d=6',
              '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=6',
              '-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=48000:duration=6']
    encode('multi.mkv', inputs + ['-i', str(output / 'first.srt'), '-i', str(output / 'second.srt'),
                                 '-map', '0:v', '-map', '1:v', '-map', '2:a', '-map', '3:a',
                                 '-map', '4:s', '-map', '5:s', '-c:v', 'mpeg4', '-g', '60', '-threads', '1',
                                 '-c:a', 'pcm_s16le', '-c:s', 'srt', '-metadata:s:a:0', 'language=jpn',
                                 '-metadata:s:a:1', 'language=eng', '-metadata:s:s:0', 'language=jpn',
                                 '-metadata:s:s:1', 'language=eng'])
    encode('programs.ts', inputs + ['-map', '0:v', '-map', '2:a', '-map', '1:v', '-map', '3:a',
                                   '-c:v', 'mpeg2video', '-threads', '1', '-c:a', 'mp2',
                                   '-program', 'program_num=101:st=0:st=1',
                                   '-program', 'program_num=202:st=2:st=3'])
    encode('animation.gif', ['-i', str(output / 'colors.avi'), '-threads', '1'])
    encode('sar.mkv', ['-i', str(output / 'colors.avi'), '-vf', 'setsar=2/1', '-c:v', 'ffv1', '-threads', '1'])
    (output / 'VIDEO-FIXTURES.json').write_text(json.dumps(dict(
        generator='generate_video_samples.py', ffmpeg=version, duration_ms=6000, fps=10,
        color_intervals=[[0, 2000, 'red'], [2000, 4000, 'green'], [4000, 6000, 'blue']],
        tracks=dict(video=2, audio_hz=[440, 880], subtitles=['jpn', 'eng']), files=commands,
    ), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    generate(args.output.resolve(), args.ffmpeg)
