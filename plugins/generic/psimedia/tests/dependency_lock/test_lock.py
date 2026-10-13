"""Dependency resolution and consumer regressions, independent of media compilation."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("resolver", ROOT / ".github/scripts/resolve-dependencies.py")
resolver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resolver)


class DependencyLockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get("PSIMEDIA_TEST_TMPDIR"))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "dependencies.lock.json"
        self.lock = json.loads((ROOT / "dependencies.lock.json").read_text())
        self.write_lock()

    def write_lock(self):
        self.path.write_text(json.dumps(self.lock), encoding="utf-8")

    def test_api_and_windows_outputs_come_from_lock(self):
        lock = resolver.read_lock(self.path)
        for profile in ("modern", "win7"):
            values = resolver.resolve(lock, profile)
            self.assertEqual(values["PSI_API_REF"], lock["psi"]["commit"])
            self.assertEqual(values["QT_VERSION"], lock["windows"][profile]["qt"]["version"])
            self.assertEqual(values["GSTREAMER_VERSION"], lock["windows"][profile]["gstreamer"]["version"])
        self.assertNotIn("QT_MAJOR", resolver.resolve(lock))

    def test_override_cannot_silently_change_psi_api(self):
        self.assertEqual(resolver.resolve(self.lock, requested_psi_ref=self.lock["psi"]["commit"]),
                         resolver.resolve(self.lock))
        for ref in ("master", "a" * 40, "branch\ninjected=value"):
            with self.subTest(ref=ref), self.assertRaisesRegex(ValueError, "disagrees"):
                resolver.resolve(self.lock, requested_psi_ref=ref)

    def test_rejects_invalid_source_pins_and_schema(self):
        original = copy.deepcopy(self.lock)
        for key, value in (("commit", "master"), ("commit", "a" * 39),
                           ("repository", "psi-im/psi\nunsafe=value")):
            self.lock = copy.deepcopy(original)
            self.lock["psi"][key] = value
            self.write_lock()
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                resolver.read_lock(self.path)
        self.lock = original
        self.lock["schema_version"] = 999
        self.write_lock()
        with self.assertRaisesRegex(ValueError, "schema"):
            resolver.read_lock(self.path)

    def test_rejects_unsafe_or_inconsistent_installers(self):
        original = copy.deepcopy(self.lock)
        for field, value in (("sha256", "bad"), ("file", "../installer.exe"),
                             ("file", "gstreamer-1.0-msvc-x86_64-other.exe")):
            self.lock = copy.deepcopy(original)
            self.lock["windows"]["modern"]["gstreamer"]["installers"][0][field] = value
            self.write_lock()
            with self.subTest(field=field), self.assertRaises(ValueError):
                resolver.read_lock(self.path)
        self.lock = original
        self.lock["windows"]["modern"]["gstreamer"]["base_url"] = "https://example.test/installer"
        self.write_lock()
        with self.assertRaisesRegex(ValueError, "source"):
            resolver.read_lock(self.path)

    def test_win7_cannot_use_modern_sdk_or_qt6(self):
        original = copy.deepcopy(self.lock)
        self.lock["windows"]["win7"]["gstreamer"] = copy.deepcopy(self.lock["windows"]["modern"]["gstreamer"])
        self.write_lock()
        with self.assertRaisesRegex(ValueError, "Win7 cannot"):
            resolver.read_lock(self.path)
        self.lock = original
        self.lock["windows"]["win7"]["qt"] = copy.deepcopy(self.lock["windows"]["modern"]["qt"])
        self.write_lock()
        with self.assertRaisesRegex(ValueError, "Qt major"):
            resolver.read_lock(self.path)

    def test_installer_cache_is_independent_of_psi_and_qt(self):
        before = resolver.resolve(self.lock, "modern")["GSTREAMER_CACHE_KEY"]
        self.lock["psi"]["commit"] = "a" * 40
        self.lock["windows"]["modern"]["qt"]["version"] = "6.9.0"
        self.assertEqual(before, resolver.resolve(self.lock, "modern")["GSTREAMER_CACHE_KEY"])
        self.lock["windows"]["modern"]["gstreamer"]["installers"][0]["sha256"] = "a" * 64
        self.assertNotEqual(before, resolver.resolve(self.lock, "modern")["GSTREAMER_CACHE_KEY"])
        self.assertNotEqual(before, resolver.resolve(self.lock, "win7")["GSTREAMER_CACHE_KEY"])

    def test_cli_exports_output_env_and_json_from_same_lock(self):
        env_file, outputs = self.root / "env.txt", self.root / "outputs.txt"
        env = dict(os.environ, GITHUB_ENV=str(env_file), GITHUB_OUTPUT=str(outputs))
        args = [sys.executable, str(ROOT / ".github/scripts/resolve-dependencies.py"),
                "--lock-path", str(self.path), "--profile", "win7"]
        result = subprocess.run(args, env=env, capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, env_file.read_text())
        self.assertEqual(result.stdout, outputs.read_text())
        result = subprocess.run(args + ["--json"], env=env, capture_output=True, text=True, check=True)
        data = json.loads(result.stdout)
        self.assertEqual(data["gstreamer"], self.lock["windows"]["win7"]["gstreamer"])
        self.assertEqual(data["values"], resolver.resolve(self.lock, "win7"))
        self.assertEqual(env_file.read_text(), outputs.read_text())
        result = subprocess.run(args + ["--psi-ref", "master"], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("disagrees", result.stderr)

    def test_checkout_revision_and_clean_headers_are_enforced(self):
        git_env = dict(os.environ, GIT_AUTHOR_NAME="test", GIT_AUTHOR_EMAIL="test@example.test",
                       GIT_COMMITTER_NAME="test", GIT_COMMITTER_EMAIL="test@example.test")
        def git(*args):
            return subprocess.check_output(["git", "-C", str(self.root), *args], env=git_env, text=True).strip()
        git("init", "-q")
        header = self.root / "plugins/include/psimediaprovider.h"
        header.parent.mkdir(parents=True)
        header.write_text("// fixture\n")
        git("add", "plugins")
        git("-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "API fixture")
        self.lock["psi"]["commit"] = git("rev-parse", "HEAD")
        resolver.verify_psi(self.lock, self.root)
        header.write_text("// modified\n")
        with self.assertRaisesRegex(ValueError, "local modifications"):
            resolver.verify_psi(self.lock, self.root)
        header.write_text("// fixture\n")
        self.lock["psi"]["commit"] = "a" * 40
        with self.assertRaisesRegex(ValueError, "locked commit"):
            resolver.verify_psi(self.lock, self.root)

    def test_workflows_do_not_contain_copied_source_or_sdk_pins(self):
        pins = [self.lock["psi"]["commit"]]
        for profile in self.lock["windows"].values():
            pins += [profile["qt"]["version"], profile["gstreamer"]["version"]]
            pins += [item["sha256"] for item in profile["gstreamer"]["installers"]]
        for workflow in (ROOT / ".github/workflows").glob("*.yml"):
            for pin in pins:
                self.assertNotIn(pin, workflow.read_text(), str(workflow))
        for name in ("rtp-regressions.yml", "windows-msvc.yml"):
            text = (ROOT / ".github/workflows" / name).read_text()
            self.assertIn("./.github/actions/load-dependencies", text)
            self.assertIn("--verify-psi-dir _psi", text)
            self.assertIn("steps.dependencies.outputs.psi_ref", text)


if __name__ == "__main__":
    unittest.main()
