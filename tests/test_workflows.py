# SPDX-License-Identifier: GPL-3.0-only
"""Publish v1 documentation without replacing the v2 portion of the site."""
import ast
from fnmatch import fnmatchcase
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MaintenanceWorkflowTests(unittest.TestCase):
    def text(self, name):
        return (ROOT / '.github/workflows' / name).read_text(encoding='utf-8')

    def test_v1_checks_target_the_maintenance_branch(self):
        for name in ('build.yml', 'pages.yml'):
            text = self.text(name)
            triggers = text.split('\npermissions:', 1)[0]
            for event in ('push', 'pull_request'):
                with self.subTest(workflow=name, event=event):
                    block = re.search(r'^  ' + event + r':\n(.*?)(?=^  \S|\Z)',
                                      triggers, re.M | re.S).group(1)
                    branches = re.search(r'^    branches: \[([^\]]+)\]', block, re.M).group(1)
                    self.assertEqual([b.strip() for b in branches.split(',')], ['main-v1'])
            self.assertRegex(triggers, r'(?m)^  workflow_dispatch:')
            self.assertIn('unittest discover -s tests -p test_workflows.py', text)
        self.assertRegex(self.text('build.yml'), r'(?m)^  workflow_call:')

    def test_v1_documentation_publishes_only_checked_branch_runs(self):
        text = self.text('pages.yml')
        self.assertIn('python -m mkdocs build --strict', text)
        self.assertIn('contents: read', text)
        deploy = text.split('\n  deploy:', 1)[1]
        self.assertIn('needs: build', deploy)
        self.assertIn('group: komamimic-pages-publish', deploy)
        self.assertIn('cancel-in-progress: false', deploy)
        self.assertIn('contents: write', deploy)
        self.assertIn('pages: write', deploy)
        self.assertIn('id-token: write', deploy)
        self.assertIn('name: github-pages', deploy)
        self.assertEqual(deploy.count("if: steps.site.outputs.ready == 'true'"), 2)
        self.assertNotIn('workflow run', text)
        self.assertNotIn('run-id:', text)
        self.assertIn('actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9', deploy)
        self.assertIn('actions/deploy-pages@368f82528645a54fb793d4d04e342629a3f51346', deploy)
        gates = [re.search(r'^    if: (.+)$', deploy, re.M).group(1)]
        gates += re.findall(r'if: (.+)\n\s+uses: actions/upload-artifact@', text)
        self.assertEqual(len(gates), 2)
        for gate in gates:
            for ref, event, expected in (
                ('refs/heads/main-v1', 'push', True),
                ('refs/heads/main-v1', 'workflow_dispatch', True),
                ('refs/heads/main-v1', 'pull_request', False),
                ('refs/pull/1/merge', 'pull_request', False),
                ('refs/heads/main', 'push', False),
                ('refs/heads/main', 'workflow_dispatch', False),
                ('refs/tags/v1.8.0', 'push', False),
            ):
                condition = gate.replace('github.ref', repr(ref)).replace('github.event_name', repr(event))
                condition = condition.replace('&&', ' and ').replace('||', ' or ')
                tree = ast.parse(condition, mode='eval')
                allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.Compare, ast.Eq, ast.Constant)
                self.assertTrue(all(isinstance(node, allowed) for node in ast.walk(tree)))
                self.assertEqual(eval(compile(tree, '<pages condition>', 'eval'), {'__builtins__': {}}), expected)

    def publication_script(self):
        step = self.text('pages.yml').split('      - name: Update v1 files in gh-pages\n', 1)[1]
        return textwrap.dedent(step.split('        run: |\n', 1)[1].split('\n      - name:', 1)[0])

    @unittest.skipUnless(shutil.which('git') and shutil.which('rsync'), 'Requires Git and rsync')
    def test_v1_publication_creates_branch_preserves_v2_and_removes_old_v1(self):
        with tempfile.TemporaryDirectory(prefix='koma-pages-') as temporary:
            base = Path(temporary).resolve()
            env = os.environ.copy()
            env.update(HOME=str(base / 'home'), XDG_CONFIG_HOME=str(base / 'config'),
                       GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1')

            def git(*args):
                return subprocess.check_output(['git', '-c', 'user.name=Workflow Test',
                                                '-c', 'user.email=workflow@example.invalid', *args],
                                               cwd=base, env=env, text=True, stderr=subprocess.PIPE).strip()

            remote, source = base / 'remote.git', base / 'source'
            git('init', '--bare', str(remote))
            git('init', '-b', 'main-v1', str(source))
            (source / 'source-only.txt').write_text('Must not be published')
            git('-C', str(source), 'add', '.')
            git('-C', str(source), 'commit', '-m', 'Source fixture')
            git('-C', str(source), 'remote', 'add', 'origin', str(remote))
            env['SOURCE_COMMIT'] = git('-C', str(source), 'rev-parse', 'HEAD')

            def publish(name, files):
                runtime = base / name
                site = runtime / 'komamimic-v1'
                site.mkdir(parents=True)
                for filename, data in files.items():
                    path = site / filename
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                output = runtime / 'output'
                result = subprocess.run(['bash', '-c', self.publication_script()], cwd=source,
                                        env=dict(env, RUNNER_TEMP=str(runtime), GITHUB_OUTPUT=str(output)),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                return output.read_text()

            self.assertEqual(publish('first', {'index.html': b'v1 first', 'en/index.html': b'English first',
                                               'obsolete.html': b'remove this'}), 'ready=false\n')
            self.assertEqual(git('--git-dir=' + str(remote), 'show', 'gh-pages:index.html'), 'v1 first')
            tree = git('--git-dir=' + str(remote), 'ls-tree', '-r', '--name-only', 'gh-pages')
            self.assertNotIn('source-only.txt', tree)
            self.assertIn('.nojekyll', tree)
            git('-C', str(source), 'fetch', 'origin', 'gh-pages')
            seed = base / 'seed'
            git('-C', str(source), 'worktree', 'add', '--detach', str(seed), 'FETCH_HEAD')
            (seed / 'v2/assets').mkdir(parents=True)
            sentinel = b'v2\x00retained\xff'
            (seed / 'v2/assets/keep.bin').write_bytes(sentinel)
            (seed / 'v2/index.html').write_text('v2 retained')
            (seed / 'v2/en').mkdir()
            (seed / 'v2/en/index.html').write_text('v2 English retained')
            git('-C', str(seed), 'add', '.')
            git('-C', str(seed), 'commit', '-m', 'Existing v2 site')
            git('-C', str(source), 'push', 'origin', git('-C', str(seed), 'rev-parse', 'HEAD') + ':gh-pages')

            files = {'index.html': b'v1 links to v2', 'en/index.html': b'English links to v2'}
            self.assertEqual(publish('second', files), 'ready=true\n')
            for filename, data in files.items():
                for prefix in ('', 'v1/'):
                    self.assertEqual(git('--git-dir=' + str(remote), 'show', 'gh-pages:' + prefix + filename), data.decode())
            preserved = subprocess.check_output(['git', '--git-dir=' + str(remote), 'show',
                                                  'gh-pages:v2/assets/keep.bin'], env=env)
            self.assertEqual(preserved, sentinel)
            self.assertEqual(git('--git-dir=' + str(remote), 'show', 'gh-pages:v2/index.html'), 'v2 retained')
            tree = git('--git-dir=' + str(remote), 'ls-tree', '-r', '--name-only', 'gh-pages')
            self.assertNotIn('obsolete.html', tree)
            commit = git('--git-dir=' + str(remote), 'rev-parse', 'gh-pages')
            publish('unchanged', files)
            self.assertEqual(git('--git-dir=' + str(remote), 'rev-parse', 'gh-pages'), commit)

            git('-C', str(source), 'remote', 'set-url', 'origin', str(base / 'missing.git'))
            runtime = base / 'failed-remote'
            runtime.mkdir()
            failed = subprocess.run(['bash', '-c', self.publication_script()], cwd=source,
                                    env=dict(env, RUNNER_TEMP=str(runtime)),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse((runtime / 'komamimic-pages').exists())

    def test_v1_release_tags_and_manual_runs_keep_the_selected_source(self):
        text = self.text('release.yml')
        triggers = text.split('\npermissions:', 1)[0]
        self.assertRegex(triggers, r'push:\s+tags:')
        self.assertNotIn('branches:', triggers)
        pattern = re.search(r"^      - '([^']+)'", triggers, re.M).group(1)
        for tag in ('v1.7.4', 'v1.8.0'):
            self.assertTrue(fnmatchcase(tag, pattern), tag)
        self.assertRegex(triggers, r'(?m)^  workflow_dispatch:')
        self.assertIn('uses: ./.github/workflows/build.yml', text)
        self.assertIn('uses: ./.github/workflows/rocky8-probe.yml', text)
        self.assertNotRegex(text, r'(?m)^\s+ref:\s*(?:main|main-v1)\s*$')
        self.assertIn('unittest discover -s tests -p test_workflows.py', text)
        publisher = text.split('\n  publish:', 1)[1]
        expression = re.search(r'^    if: \$\{\{ (.+) \}\}', publisher, re.M).group(1)
        for event, ref_type, expected in (('push', 'tag', True),
                                          ('push', 'branch', False),
                                          ('workflow_dispatch', 'branch', False),
                                          ('workflow_dispatch', 'tag', False)):
            condition = expression.replace('github.event_name', repr(event))
            condition = condition.replace('github.ref_type', repr(ref_type)).replace('&&', ' and ')
            tree = ast.parse(condition, mode='eval')
            allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Compare, ast.Eq, ast.Constant)
            self.assertTrue(all(isinstance(node, allowed) for node in ast.walk(tree)))
            self.assertEqual(eval(compile(tree, '<release condition>', 'eval'), {'__builtins__': {}}), expected)
