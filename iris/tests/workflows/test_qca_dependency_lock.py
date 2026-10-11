"""Regression checks for the single-source QCA dependency policy."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class QcaLockTests(unittest.TestCase):
    def test_qca_pin_is_not_lower_than_runtime_minimum(self):
        qca = json.loads((ROOT / "dependencies.lock.json").read_text())["qca"]
        self.assertRegex(qca["tag"], r"^v\d+\.\d+\.\d+$")
        self.assertRegex(qca["commit"], r"^[0-9a-f]{40}$")
        self.assertRegex(qca["minimum_version"], r"^\d+\.\d+\.\d+$")
        tag_version = tuple(map(int, qca["tag"][1:].split(".")))
        minimum_version = tuple(map(int, qca["minimum_version"].split(".")))
        self.assertGreaterEqual(tag_version, minimum_version)

    def test_no_other_action_downloads_its_own_qca_release(self):
        # All installation must pass through load-dependencies, even for
        # Qt/Android SDK packages. No workflow may independently pin a tag.
        root = ROOT / ".github"
        loader = root / "actions/load-dependencies/action.yml"
        self.assertIn("gh release download", loader.read_text())
        for path in [*root.glob("workflows/*.yml"), *root.glob("actions/*/action.yml")]:
            if path == loader:
                continue
            data = path.read_text()
            self.assertNotIn("gh release download", data, str(path))
            self.assertNotRegex(data, r"QCA_TAG:\s*v\d+\.\d+\.\d+", str(path))
            self.assertNotIn("qca-tag:", data, str(path))

    def test_qca_consumers_use_shared_action(self):
        for name in (
            "build-iris-deb",
            "build-iris-desktop",
            "build-iris-android",
        ):
            text = (ROOT / f".github/actions/{name}/action.yml").read_text()
            self.assertIn("uses: ./.github/actions/load-dependencies", text, name)
            self.assertIn("install-mode:", text, name)


if __name__ == "__main__":
    unittest.main()
