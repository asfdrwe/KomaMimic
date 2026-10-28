#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Export a private-data-free source + synthetic fixtures kit for local testing."""
import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile

from check_source import check
from export_source import export


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(directory):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / 'VALIDATION_MANIFEST.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('kind') != 'synthetic-validation-kit':
        raise ValueError('Not a validation kit manifest')
    seen = set()
    for record in manifest['files']:
        name = record['path']
        relative = PurePosixPath(name)
        if (relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name
                or name in seen or str(relative) != name):
            raise ValueError('Invalid manifest path')
        seen.add(name)
        path = directory / name
        if any(p.is_symlink() for p in (path, *path.parents) if p != directory and directory in p.parents):
            raise ValueError('Symlink in validation input: ' + name)
        if not path.is_file() or digest(path) != record['sha256']:
            raise ValueError('Missing or changed validation input: ' + name)
    print(f'Validation kit: {len(seen)} file hashes OK (builds/results outside manifest)')
    return manifest


def prepare(root, output, worker=None):
    root, output = Path(root).resolve(), Path(output).resolve()
    package = output.with_name(output.name + '.zip')
    sums = output.with_name(output.name + '-SHA256SUMS')
    if output.exists() or package.exists() or sums.exists():
        raise ValueError('Output, ZIP or SHA256SUMS already exists; choose a new output name')
    check(root)
    version = re.search(r'project\(KomaMimic VERSION ([0-9.]+)', (root / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    # export validates all source inputs before writing. No tmp/internal data is read.
    export(root, output / 'source', output / 'SOURCE_MANIFEST.json')
    command = [sys.executable, str(output / 'source/tests/fixtures/generate_validation_samples.py'),
               '--output', str(output / 'fixtures')]
    if worker:
        command += ['--verify-worker', str(Path(worker).resolve())]
    subprocess.run(command, check=True)
    (output / 'START.md').write_text(
        f'# KomaMimic v{version} 実機検証セット\n\n'
        '最初に [実機検証手順](source/docs/developer/platform-validation.md) を読んでください。\n\n'
        'Windowsは [ビルド済みQtとReleaseの手順](source/docs/developer/building.md) を使います。\n\n'
        '- source/: 選別済みの開発ソース。これを各OSでビルドします。\n'
        '- fixtures/: 自作の手動検証入力と基準PNG。原作・私有書籍は含みません。\n'
        '- SOURCE_MANIFEST.json: sourceの版をファイルhashで識別します。\n'
        '- VALIDATION_MANIFEST.json: ソースとfixture全体のhash。\n\n'
        'Windows/macOSの実行ファイルやQt依存一式は入っていません。ネット接続と開発環境が必要です。\n'
        '生成済みの検証入力はアプリ配布物・公開リポジトリへ追加しません。\n'
        '結果・個人の環境情報は各端末に保存し、自動送信しません。\n', encoding='utf-8')
    records = [{'path': p.relative_to(output).as_posix(), 'sha256': digest(p)}
               for p in sorted(output.rglob('*')) if p.is_file() and '__pycache__' not in p.parts]
    (output / 'VALIDATION_MANIFEST.json').write_text(json.dumps(
        {'schema': 1, 'kind': 'synthetic-validation-kit', 'version': version, 'files': records},
        ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    with zipfile.ZipFile(package, 'x', compression=zipfile.ZIP_DEFLATED) as stream:
        for path in sorted(output.rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            info = zipfile.ZipInfo(output.name + '/' + path.relative_to(output).as_posix(), (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (0o100755 if path.suffix in ('.py', '.sh') else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            stream.writestr(info, path.read_bytes())
    sums.write_text(digest(package) + '  ' + package.name + '\n', encoding='ascii')
    verify(output)
    print('Portable ZIP:', package)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--output', type=Path)
    group.add_argument('--verify', type=Path)
    parser.add_argument('--verify-worker', type=Path)
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    else:
        prepare(Path(__file__).resolve().parents[1], args.output, args.verify_worker)
