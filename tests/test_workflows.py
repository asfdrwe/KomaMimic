# SPDX-License-Identifier: GPL-3.0-only
"""A push or manual check on dev2 cannot deploy Pages or publish a Release."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
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

    def test_pages_publish_and_upload_require_main_and_checked_branch_runs(self):
        text = self.text('pages.yml')
        expressions = [condition(job(text, 'deploy'))]
        expressions += re.findall(r'if: (.+)\n\s+uses: actions/upload-artifact@', text)
        self.assertEqual(len(expressions), 2)
        for expression in expressions:
            for ref, event in [('refs/heads/dev2', 'push'), ('refs/heads/dev2', 'workflow_dispatch'),
                               ('refs/heads/main-v1', 'push'), ('refs/heads/main-v1', 'workflow_dispatch'),
                               ('refs/heads/main', 'pull_request'), ('refs/pull/1/merge', 'pull_request'),
                               ('refs/tags/v2.0.0', 'push')]:
                with self.subTest(ref=ref, event=event):
                    self.assertFalse(allowed(expression, ref, event))
            self.assertTrue(allowed(expression, 'refs/heads/main', 'push'))
            self.assertTrue(allowed(expression, 'refs/heads/main', 'workflow_dispatch'))
        self.assertIn('tools/build_docs.py', text)
        self.assertIn('fetch-depth: 0', text)
        self.assertIn('group: komamimic-pages-publish', job(text, 'deploy'))
        self.assertIn('contents: write', job(text, 'deploy'))
        self.assertEqual(text.count("if: steps.site.outputs.ready == 'true'"), 2)
        self.assertIn('actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9', text)
        self.assertIn('actions/deploy-pages@368f82528645a54fb793d4d04e342629a3f51346', text)
        build = job(text, 'build')
        self.assertIn('from build_docs import build_guide', build)
        self.assertNotIn('python tools/build_docs.py --output', build)
        self.assertNotRegex(build, r'ref:\s*(?:main-v1|v1\.7\.3)')

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
            if file.name != 'pages.yml':
                self.assertNotIn('git push', text)
            self.assertNotRegex(text, r'ref:\s*(?:main|refs/heads/main)\s')
            if file.name not in ('pages.yml', 'release.yml'):
                self.assertNotRegex(text, r'(?:contents|pages|id-token): write')
        self.assertEqual(len(groups), len(set(groups)))

    @unittest.skipUnless(shutil.which('git') and shutil.which('rsync'), 'Requires Git and rsync')
    def test_v2_publication_preserves_v1_and_initializes_missing_branch(self):
        step = self.text('pages.yml').split('      - name: Update v2 files in gh-pages\n', 1)[1]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1].split('\n      - name:', 1)[0])
        with tempfile.TemporaryDirectory(prefix='koma-pages-v2-') as temporary:
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
            git('init', '-b', 'main', str(source))
            (source / 'source-only.txt').write_text('Must not be published')
            git('-C', str(source), 'add', '.')
            git('-C', str(source), 'commit', '-m', 'Source fixture')
            git('-C', str(source), 'remote', 'add', 'origin', str(remote))
            env['SOURCE_COMMIT'] = git('-C', str(source), 'rev-parse', 'HEAD')

            def publish(name, files):
                runtime = base / name
                site = runtime / 'komamimic-v2'
                site.mkdir(parents=True)
                for filename, data in files.items():
                    path = site / filename
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                output = runtime / 'output'
                result = subprocess.run(['bash', '-c', script], cwd=source,
                                        env=dict(env, RUNNER_TEMP=str(runtime), GITHUB_OUTPUT=str(output)),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                return output.read_text()

            self.assertEqual(publish('first', {'index.html': b'v2 first', 'en/index.html': b'English first',
                                               'obsolete.html': b'remove this'}), 'ready=false\n')
            tree = git('--git-dir=' + str(remote), 'ls-tree', '-r', '--name-only', 'gh-pages')
            self.assertNotIn('source-only.txt', tree)
            self.assertNotIn('\nindex.html', tree)
            self.assertIn('v2/index.html', tree)
            git('-C', str(source), 'fetch', 'origin', 'gh-pages')
            seed = base / 'seed'
            git('-C', str(source), 'worktree', 'add', '--detach', str(seed), 'FETCH_HEAD')
            v1_files = {'index.html': b'v1 Japanese', 'en/index.html': b'v1 English',
                        'v1/index.html': b'v1 alias', 'assets/keep.bin': b'v1\x00retained\xff'}
            for filename, data in v1_files.items():
                path = seed / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            git('-C', str(seed), 'add', '.')
            git('-C', str(seed), 'commit', '-m', 'Existing v1 site')
            git('-C', str(source), 'push', 'origin', git('-C', str(seed), 'rev-parse', 'HEAD') + ':gh-pages')

            files = {'index.html': b'v2 updated', 'en/index.html': b'v2 English updated'}
            self.assertEqual(publish('second', files), 'ready=true\n')
            for filename, data in v1_files.items():
                actual = subprocess.check_output(['git', '--git-dir=' + str(remote), 'show',
                                                   'gh-pages:' + filename], env=env)
                self.assertEqual(actual, data)
            for filename, data in files.items():
                self.assertEqual(git('--git-dir=' + str(remote), 'show', 'gh-pages:v2/' + filename), data.decode())
            self.assertNotIn('obsolete.html', git('--git-dir=' + str(remote), 'ls-tree', '-r', '--name-only', 'gh-pages'))
            commit = git('--git-dir=' + str(remote), 'rev-parse', 'gh-pages')
            publish('unchanged', files)
            self.assertEqual(git('--git-dir=' + str(remote), 'rev-parse', 'gh-pages'), commit)

            git('-C', str(source), 'remote', 'set-url', 'origin', str(base / 'missing.git'))
            runtime = base / 'failed-remote'
            runtime.mkdir()
            failed = subprocess.run(['bash', '-c', script], cwd=source,
                                    env=dict(env, RUNNER_TEMP=str(runtime)),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse((runtime / 'komamimic-pages').exists())

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

    def test_release_prepares_inputs_once_before_both_native_build_workflows(self):
        release = self.text('release.yml')
        validate = job(release, 'validate')
        self.assertIn('tools/fetch_release_inputs.py --output tmp/release-pinned-inputs', validate)
        self.assertIn('name: komamimic-release-pinned-inputs', validate)
        self.assertIn('path: tmp/release-pinned-inputs/*', validate)
        for name in ('build', 'appimage'):
            self.assertRegex(job(release, name), r'needs: validate')
        self.assertIn('prepared_inputs: komamimic-release-pinned-inputs', job(release, 'appimage'))
        rocky = self.text('rocky8-probe.yml')
        prepare = rocky.index('Verify prepared inputs before installing build dependencies')
        self.assertLess(prepare, rocky.index('Install Rocky and Raven dependencies'))
        self.assertIn("if: ${{ inputs.prepared_inputs == '' }}", rocky)
        self.assertIn('name: ${{ inputs.prepared_inputs }}', rocky)
        self.assertIn('--verify-only', rocky)
        self.assertIn('--prepared-tools tmp/appimage-input/pinned-inputs', rocky)
        self.assertIn('tmp/rocky8-probe/pinned-inputs/appimagetool', rocky)
        self.assertIn('tmp/rocky8-probe/pinned-inputs/runtime', rocky)
        self.assertNotIn('curl --fail', rocky)

    def test_release_downloads_are_same_run_and_preserve_all_required_failure_gates(self):
        release = self.text('release.yml')
        self.assertIn('needs: [validate, build, appimage]', job(release, 'assemble'))
        self.assertIn('needs: [validate, build, appimage, assemble]', job(release, 'publish'))
        for name in ('release.yml', 'rocky8-probe.yml', 'build.yml'):
            text = self.text(name)
            for block in re.findall(r'uses: actions/download-artifact@[^\n]+\n(.*?)(?=\n      -|\Z)', text, re.S):
                self.assertNotRegex(block, r'(?:run-id|github-token|repository):')
        self.assertIn("LIBARCHIVE_VERSION: '3.8.11'", self.text('rocky8-probe.yml'))

    def test_only_release_skips_the_duplicate_fedora_build(self):
        build = self.text('build.yml')
        expression = condition(job(build, 'linux'))
        for value in ('false', "''"):
            self.assertTrue(allowed(expression.replace('inputs.skip_linux', value).replace('false', 'False').replace('true', 'True'),
                                    'refs/heads/dev2', 'workflow_dispatch'))
        self.assertFalse(allowed(expression.replace('inputs.skip_linux', 'True').replace('true', 'True'),
                                 'refs/heads/dev2', 'workflow_dispatch'))
        self.assertRegex(build, r'skip_linux:[\s\S]*?default: false')
        release_build = job(self.text('release.yml'), 'build')
        self.assertIn('skip_linux: true', release_build)
        self.assertIn('prepared_inputs: komamimic-release-pinned-inputs', release_build)
        self.assertIn('ctest --test-dir build-ci', job(build, 'macos'))
        self.assertIn('ctest --test-dir', job(self.text('rocky8-probe.yml'), 'rocky8'))

    def test_native_packages_reuse_ctest_inputs_and_enforce_updated_libarchive(self):
        build = self.text('build.yml')
        for name, directory in (('windows', 'build-windows-fast'), ('macos', 'build-ci')):
            target = job(build, name)
            self.assertIn('--video-fixtures ' + directory + '/tests/video-fixtures', target)
            self.assertIn('-DKOMAMIMIC_MIN_LIBARCHIVE_VERSION=3.8.11', target)
        windows = job(build, 'windows')
        self.assertIn('tools/setup_windows_libarchive.py @overlayArgs', windows)
        self.assertIn('-DVCPKG_OVERLAY_PORTS=$env:GITHUB_WORKSPACE/.ci/libarchive-overlay', windows)
        self.assertIn('results/windows-libarchive.log', windows.split('Upload diagnostics', 1)[1])
