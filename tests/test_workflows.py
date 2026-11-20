# SPDX-License-Identifier: GPL-3.0-only
"""Keep v1 maintenance checks separate from the combined site's publisher."""
import ast
from fnmatch import fnmatchcase
from pathlib import Path
import re
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

    def test_v1_documentation_has_no_pages_publisher(self):
        text = self.text('pages.yml')
        self.assertIn('python -m mkdocs build --strict', text)
        self.assertIn('contents: read', text)
        self.assertNotRegex(text, r'(?m)^\s+(?:contents|pages|id-token): write\s*$')
        self.assertNotRegex(text, r'uses: actions/(?:upload-pages-artifact|deploy-pages)@')
        self.assertNotIn('github-pages', text)
        self.assertNotRegex(text, r'(?m)^  deploy:')

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
