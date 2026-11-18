#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fetch pinned AppImage inputs before building, or verify same-run artifacts offline."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def verify(entry, path):
    actual = digest(path)
    if actual != entry['sha256']:
        raise ValueError('Pinned input checksum mismatch: ' + str(path)
                         + '; expected=' + entry['sha256'] + '; actual=' + actual)


def download(entry, target):
    """Retry transient transport errors; mirrors must provide identical pinned bytes."""
    target = Path(target)
    errors = []
    for url in (entry['url'], *entry.get('mirrors', [])):
        for attempt in range(3):
            print('Fetching ' + url + ' (attempt ' + str(attempt + 1) + ')', flush=True)
            try:
                request = urllib.request.Request(url, headers={'User-Agent': 'KomaMimic-release-check'})
                with urllib.request.urlopen(request, timeout=30) as source, target.open('wb') as destination:
                    shutil.copyfileobj(source, destination)
            except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
                target.unlink(missing_ok=True)
                errors.append(url + ': ' + str(error))
                print(errors[-1], flush=True)
                if isinstance(error, urllib.error.HTTPError) and error.code not in (408, 429, 500, 502, 503, 504):
                    break
                if attempt < 2:
                    time.sleep(2)
                continue
            # A checksum failure is final: do not hide it with a different mirror.
            try:
                verify(entry, target)
            except ValueError:
                target.unlink()
                raise
            return
    raise RuntimeError('Cannot download pinned input ' + target.name + ':\n' + '\n'.join(errors))


def configuration():
    entries = {}
    for name in ('appimage-sources.json', 'appimage-tools.json'):
        entries.update(json.loads((ROOT / 'tools' / name).read_text(encoding='utf-8')))
    return entries


def prepare(output, verify_only=False):
    output = Path(output)
    if not verify_only:
        output.mkdir(parents=True, exist_ok=False)
    for name, entry in configuration().items():
        if verify_only:
            verify(entry, output / name)
        else:
            download(entry, output / name)
        print('Verified ' + name + ' sha256=' + entry['sha256'], flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-only', action='store_true', help='Reject missing/changed inputs without downloading')
    args = parser.parse_args()
    prepare(args.output.resolve(), args.verify_only)
