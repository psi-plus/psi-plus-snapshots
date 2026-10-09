"""Exercise bundled QCA lock changes with real CMake/ExternalProject state."""
import json
import os
import textwrap
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cmake'), 'CMake is required')
class DependencyLock(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        module = self.root / 'cmake/modules/IrisQCA.cmake'
        module.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / 'cmake/modules/IrisQCA.cmake', module)
        (self.root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.11)
project(lock_test LANGUAGES C CXX)
set(QT_DEFAULT_MAJOR_VERSION 6)
set(IRIS_BUNDLED_QCA ON)
include(cmake/modules/IrisQCA.cmake)
file(WRITE "${CMAKE_BINARY_DIR}/selected-ref" "${IRIS_BUNDLED_QCA_GIT_TAG}")
''')
        self.lock('a' * 40)

    def lock(self, commit, minimum_version="3.0.10", tag="v3.0.10"):
        (self.root / 'dependencies.lock.json').write_text(
            json.dumps({'qca': {'tag': tag, 'commit': commit, 'minimum_version': minimum_version}}))

    def configure(self, *args, success=True):
        result = subprocess.run(['cmake', '-S', str(self.root), '-B', str(self.build), *args],
                                capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return (self.build / 'selected-ref').read_text()
        self.assertNotEqual(result.returncode, 0)
        return result.stdout + result.stderr

    def test_lock_change_updates_default_and_clears_stale_state(self):
        self.assertEqual(self.configure(), 'a' * 40)
        stale = self.build / '_deps/qca/stale'
        stale.touch()
        self.assertEqual(self.configure(), 'a' * 40)
        self.assertTrue(stale.exists())
        self.lock('b' * 40)
        self.assertEqual(self.configure(), 'b' * 40)
        self.assertFalse(stale.exists())

    def test_explicit_override_survives_lock_change(self):
        self.assertEqual(self.configure('-DIRIS_BUNDLED_QCA_GIT_TAG=custom'), 'custom')
        stale = self.build / '_deps/qca/stale'
        stale.touch()
        self.lock('b' * 40)
        self.assertEqual(self.configure(), 'custom')
        self.assertTrue(stale.exists())

    def test_override_change_clears_stale_state(self):
        self.configure('-DIRIS_BUNDLED_QCA_GIT_TAG=custom')
        stale = self.build / '_deps/qca/stale'
        stale.touch()
        self.configure('-DIRIS_BUNDLED_QCA_GIT_TAG=another')
        self.assertFalse(stale.exists())

    def test_minimum_is_read_from_lock_and_updates_on_reconfigure(self):
        self.lock('a' * 40, '3.0.11')
        self.configure()
        self.assertEqual((self.build / 'iris-qca-min-version.txt').read_text().strip(), '3.0.11')
        self.lock('a' * 40, '3.0.10')
        self.configure()
        self.assertEqual((self.build / 'iris-qca-min-version.txt').read_text().strip(), '3.0.10')

    def test_build_release_change_does_not_raise_minimum(self):
        self.lock('a' * 40, tag='v3.0.11')
        self.configure()
        self.assertEqual((self.build / 'iris-qca-min-version.txt').read_text().strip(), '3.0.10')

    def test_invalid_minimum_rejected(self):
        for value in (None, '', 'v3.0.10', '3.0', '3.0.10-extra', 310):
            with self.subTest(value=value):
                self.lock('a' * 40, value)
                self.assertIn('QCA minimum_version must be', self.configure(success=False))

    def test_invalid_commit_rejected(self):
        self.lock('not-a-sha')
        self.assertIn('QCA commit must be a full lowercase Git SHA',
                      self.configure(success=False))


class DependencyEnvironment(unittest.TestCase):
    def run_loader(self, tag, commit, minimum_version="3.0.10"):
        action = (ROOT / '.github/actions/load-dependencies/action.yml').read_text()
        script = textwrap.dedent(action.split('      run: |\n', 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'dependencies.lock.json').write_text(json.dumps(
                {'qca': {'tag': tag, 'commit': commit, 'minimum_version': minimum_version}}))
            env = root / 'env'
            env.touch()
            result = subprocess.run(['bash', '-c', script], capture_output=True,
                                    env=dict(os.environ, GITHUB_WORKSPACE=directory,
                                             GITHUB_ENV=str(env)))
            return result.returncode, env.read_text()

    def test_valid_lock_exported(self):
        self.assertEqual(self.run_loader('v3.0.9', 'a' * 40),
                         (0, 'QCA_TAG=v3.0.9\nQCA_COMMIT=' + 'a' * 40 + '\nQCA_MIN_VERSION=3.0.10\n'))

    def test_invalid_minimum_not_partially_exported(self):
        for value in (None, '', 'v3.0.10', '3.0', '3.0.10-extra', '3.0.10\nOTHER=value', 310):
            with self.subTest(value=value):
                code, env = self.run_loader('v3.0.10', 'a' * 40, value)
                self.assertNotEqual(code, 0)
                self.assertEqual(env, '')

    def test_invalid_lock_not_partially_exported(self):
        for tag, commit in [('', 'a' * 40), ('v3.0.9\nOTHER=value', 'a' * 40),
                            (123, 'a' * 40), ('v3.0.9', 'bad-sha')]:
            with self.subTest(tag=tag, commit=commit):
                code, env = self.run_loader(tag, commit)
                self.assertNotEqual(code, 0)
                self.assertEqual(env, '')


if __name__ == '__main__':
    unittest.main()
