#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate distributable Qt catalogs without changing source or translations."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def check_translations(root):
    root = Path(root)
    catalogs = sorted((root / 'translations').glob('komamimic_*.ts'))
    if not catalogs:
        raise ValueError('No translation catalogs')
    baseline = None
    for path in catalogs:
        tree = ET.parse(path).getroot()
        code = path.stem.removeprefix('komamimic_')
        if tree.get('language') != code or tree.get('sourcelanguage') != 'ja':
            raise ValueError(f'{path.name}: catalog language does not match its filename')
        keys = set()
        for context in tree.findall('context'):
            for message in context.findall('message'):
                source = message.findtext('source', '')
                translated = message.find('translation')
                if translated is None or translated.get('type') in {'unfinished', 'obsolete', 'vanished'}:
                    raise ValueError(f'{path.name}: unfinished translation: {source}')
                values = translated.findall('numerusform') if message.get('numerus') == 'yes' else [translated]
                for value in values:
                    text = ''.join(value.itertext())
                    if not text.strip():
                        raise ValueError(f'{path.name}: empty translation: {source}')
                    placeholders = lambda s: sorted(re.findall(r'%(?:L?[1-9][0-9]*|n)|\{(?:file|directory|page)\}', s))
                    if placeholders(source) != placeholders(text):
                        raise ValueError(f'{path.name}: placeholder mismatch: {source}')
                key = context.findtext('name'), source, message.findtext('comment', '')
                if key in keys:
                    raise ValueError(f'{path.name}: duplicate translation: {source}')
                keys.add(key)
        if baseline is not None and keys != baseline:
            raise ValueError(f'{path.name}: catalogs have different message sets; run update_translations')
        baseline = keys
    print(f'Translations: {len(catalogs)} catalogs, {len(baseline)} messages each; complete and placeholders OK')


if __name__ == '__main__':
    check_translations(Path(__file__).resolve().parents[1])
