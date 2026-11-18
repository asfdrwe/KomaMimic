# SPDX-License-Identifier: GPL-3.0-only
"""A push or manual check on dev2 cannot deploy Pages or publish a Release."""
import ast
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def job(text, name):
    match = re.search(r'^  ' + re.escape(name) + r':\n(.*?)(?=^  [a-z][a-z_-]*:|\Z)', text, re.M | re.S)
    if not match:
        raise ValueError('Missing workflow job: ' + name)
    return match[1]


def condition(text):
    match = re.search(r'^    if: (.+)$', text, re.M)
    if not match:
        raise ValueError('Publishing job has no condition')
    return match[1]


def allowed(expression, ref, event, ref_type='branch'):
    expression = expression.strip().removeprefix('${{').removesuffix('}}').strip()
    for key, value in dict(event_name=event, ref_type=ref_type, ref=ref).items():
        expression = expression.replace('github.' + key, repr(value))
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    tree = ast.parse(expression, mode='eval')
    nodes = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.Compare, ast.Eq, ast.NotEq, ast.Constant)
    if any(not isinstance(node, nodes) for node in ast.walk(tree)):
        raise ValueError('Unsupported publishing expression')
    return eval(compile(tree, '<workflow condition>', 'eval'), {'__builtins__': {}}, {})


class WorkflowIsolationTests(unittest.TestCase):
    def text(self, name):
        return (ROOT / '.github/workflows' / name).read_text(encoding='utf-8')

    def test_pages_publish_and_upload_exclude_dev2_and_pull_requests(self):
        text = self.text('pages.yml')
        expressions = [condition(job(text, 'deploy'))]
        expressions += re.findall(r'if: (.+)\n\s+uses: actions/upload-pages-artifact@', text)
        self.assertEqual(len(expressions), 2)
        for expression in expressions:
            for ref, event in [('refs/heads/dev2', 'push'), ('refs/heads/dev2', 'workflow_dispatch'),
                               ('refs/heads/main', 'pull_request'), ('refs/pull/1/merge', 'pull_request'),
                               ('refs/tags/v2.0.0', 'push')]:
                with self.subTest(ref=ref, event=event):
                    self.assertFalse(allowed(expression, ref, event))
            self.assertTrue(allowed(expression, 'refs/heads/main', 'push'))
            self.assertTrue(allowed(expression, 'refs/heads/main', 'workflow_dispatch'))
        self.assertIn('tools/build_docs.py', text)
        self.assertIn('fetch-depth: 0', text)

    def test_release_publish_requires_a_tag_push_not_a_branch_or_manual_run(self):
        text = self.text('release.yml')
        expression = condition(job(text, 'publish'))
        for ref in ('refs/heads/main', 'refs/heads/dev2'):
            self.assertFalse(allowed(expression, ref, 'push'))
            self.assertFalse(allowed(expression, ref, 'workflow_dispatch'))
        self.assertFalse(allowed(expression, 'refs/tags/v2.0.0', 'workflow_dispatch', 'tag'))
        self.assertTrue(allowed(expression, 'refs/tags/v2.0.0', 'push', 'tag'))
        triggers = text.split('permissions:', 1)[0]
        self.assertRegex(triggers, r'push:\s*tags:')
        self.assertNotIn('branches:', triggers)
        self.assertIn('tools/build_docs.py', job(text, 'validate'))
        self.assertIn('git merge-base --is-ancestor "$RELEASE_COMMIT" refs/remotes/origin/main',
                      job(text, 'validate'))

    def test_workflows_separate_refs_and_only_publishers_have_write_permissions(self):
        groups = []
        for file in sorted((ROOT / '.github/workflows').glob('*.yml')):
            text = file.read_text(encoding='utf-8')
            group = re.search(r'^  group: (.+)$', text, re.M)[1]
            self.assertIn('${{ github.ref }}', group)
            groups.append(group)
            self.assertIn('contents: read', text.split('jobs:', 1)[0])
            self.assertNotIn('pull_request_target:', text)
            self.assertNotIn('git push', text)
            self.assertNotRegex(text, r'ref:\s*(?:main|refs/heads/main)\s')
            if file.name not in ('pages.yml', 'release.yml'):
                self.assertNotRegex(text, r'(?:contents|pages|id-token): write')
        self.assertEqual(len(groups), len(set(groups)))

    def test_downloads_stay_in_the_current_run_and_v1_tag_is_rejected_by_v2(self):
        text = self.text('release.yml')
        for block in re.findall(r'uses: actions/download-artifact@[^\n]+\n(.*?)(?=\n      -|\Z)', text, re.S):
            self.assertNotRegex(block, r'(?:run-id|github-token|repository):')
        import sys
        sys.path.insert(0, str(ROOT / 'tools'))
        from release import version_for_tag
        tag, version = version_for_tag(ROOT)
        self.assertTrue(version.startswith('2.'))
        self.assertEqual(tag, 'v' + version)
        with self.assertRaises(ValueError):
            version_for_tag(ROOT, 'v1.7.3')

    def test_macos_uses_short_private_temp_and_package_errors_are_saved(self):
        text = self.text('build.yml')
        macos = job(text, 'macos')
        self.assertNotIn('runner.temp', macos.split('    steps:', 1)[0])
        ctest = macos.split('Full CTest (including single-instance IPC regression)', 1)[1].split('      - name:', 1)[0]
        for name in ('TMPDIR', 'TMP', 'TEMP'):
            self.assertRegex(ctest, r'(?m)^          ' + name + r': \$\{\{ runner.temp \}\}$')
        self.assertIn('set -o pipefail', macos)
        self.assertIn('tee stage-macos/package.log', macos)
        self.assertIn('tools/setup_macos_multimedia.py --output build-macos-multimedia', macos)
        self.assertIn('-DQt6Multimedia_DIR=', macos)
        self.assertIn("stream.write('QT_PLUGIN_PATH=' + report['plugins']", macos)
        self.assertIn('build-macos-multimedia/*.log', macos)
        self.assertRegex(macos, r'brew install [^\n]*\bvulkan-headers\b')
        self.assertIn('build-macos-multimedia/build/CMakeCache.txt', macos)
        windows = job(text, 'windows')
        self.assertIn('Tee-Object -FilePath results/windows-package.log', windows)
        self.assertIn('results/windows-package.log', windows.split('Upload diagnostics', 1)[1])

    def test_appimage_reuses_builder_video_samples_and_retains_probe_diagnostics(self):
        text = self.text('rocky8-probe.yml')
        producer = text.split('name: komamimic-appimage-input', 1)[1].split('if-no-files-found:', 1)[0]
        self.assertIn('tmp/rocky8-probe/build/tests/video-fixtures/*', producer)
        consumer = job(text, 'appimage')
        download = consumer.split('uses: actions/download-artifact@', 1)[1].split('      - name:', 1)[0]
        self.assertIn('name: komamimic-appimage-input', download)
        self.assertIn('path: tmp/appimage-input', download)
        self.assertIn('--video-fixtures tmp/appimage-input/build/tests/video-fixtures', consumer)
        diagnostics = consumer.split('name: diagnostics-appimage-ubuntu22-x64', 1)[1]
        self.assertIn('tmp/appimage-work/verify/*.json', diagnostics)
        self.assertIn('tmp/appimage-work/verify/*.log', diagnostics)
