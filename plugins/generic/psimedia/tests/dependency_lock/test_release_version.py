"""Regression tests for release metadata synchronization and workflow handoff."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github/scripts/prepare-release-version.py"
RELEASE = ROOT / ".github/workflows/release.yml"
PREPARE = ROOT / ".github/workflows/prepare-release.yml"


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "psiplugin.json"
        self.original = '{\n    "name": "Multimedia",\n    "version": "0.1",\n    "icon": "base64:unchanged"\n}\n'
        self.path.write_text(self.original, encoding="utf-8")

    def invoke(self, tag, *, check=False, stdin=False):
        args = [sys.executable, str(SCRIPT), "--tag", tag]
        if check:
            args.append("--check")
        args.append("-" if stdin else str(self.path))
        return subprocess.run(args, input=self.path.read_text() if stdin else None,
                              text=True, capture_output=True)

    def test_version_updates_only_the_embedded_plugin_field(self):
        for tag in ("v1.2", "v1.2.3", "v10.0.42"):
            with self.subTest(tag=tag):
                self.path.write_text(self.original, encoding="utf-8")
                result = self.invoke(tag)
                self.assertEqual(result.returncode, 0, result.stderr)
                updated = self.path.read_text(encoding="utf-8")
                self.assertEqual(json.loads(updated)["version"], tag[1:])
                self.assertEqual(updated.replace('"version": "' + tag[1:] + '"',
                                                 '"version": "0.1"'), self.original)
                self.assertEqual(self.invoke(tag, check=True).returncode, 0)
                self.assertEqual(self.invoke(tag, check=True, stdin=True).returncode, 0)

    def test_check_rejects_mismatch_without_editing(self):
        self.assertNotEqual(self.invoke("v1.2", check=True).returncode, 0)
        self.assertEqual(self.path.read_text(encoding="utf-8"), self.original)

    def test_rejects_invalid_tags_without_editing(self):
        for tag in ("1.2", "v1", "v1.2.3.4", "v01.2", "v1.02", "v1.2;touch /tmp/a"):
            with self.subTest(tag=tag):
                self.assertNotEqual(self.invoke(tag).returncode, 0)
                self.assertEqual(self.path.read_text(encoding="utf-8"), self.original)

    def test_duplicate_and_missing_version_fields_are_rejected(self):
        self.path.write_text('{"name": "Multimedia"}\n', encoding="utf-8")
        self.assertNotEqual(self.invoke("v1.2").returncode, 0)
        self.path.write_text('{"version": "0.1", "version": "0.2"}\n', encoding="utf-8")
        self.assertNotEqual(self.invoke("v1.2").returncode, 0)

    def test_release_workflows_ensure_metadata_and_tag_consistency(self):
        workflow = PREPARE.read_text(encoding="utf-8")
        release = RELEASE.read_text(encoding="utf-8")
        self.assertIn("git push --atomic", workflow)
        self.assertIn("gh workflow run release.yml", workflow)
        self.assertIn("actions: write", workflow)
        self.assertIn("contents: write", workflow)
        self.assertIn("git cat-file -e", release)
        self.assertIn("git show", release)
        self.assertIn("prepare-release-version.py", release)
        self.assertIn("--check", release)


if __name__ == "__main__":
    unittest.main()
