"""Check version resolution using real Git archives and CMake, without Qt.

Run with: python3 -m unittest discover -s tests/workflows -v
Requires Git and CMake; does not access the network.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]


class SourceArchiveVersion(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="iris-source-version-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_AUTHOR_NAME="Iris test", GIT_AUTHOR_EMAIL="test@example.invalid",
                        GIT_COMMITTER_NAME="Iris test", GIT_COMMITTER_EMAIL="test@example.invalid")
        for name in (".archive-version", ".gitattributes", "cmake/modules/IrisVersion.cmake"):
            destination = self.repo / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (self.repo / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.11.0)\n'
            'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/IrisVersion.cmake")\n'
            'project(version_probe VERSION "${iris_version}" LANGUAGES NONE)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/resolved-version.txt" "${PROJECT_VERSION}")\n')
        self.git("init", "-q")
        self.git("add", ".")
        self.git("commit", "-qm", "Version fixture")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, env=self.env,
                              check=True, capture_output=True, text=True).stdout.strip()

    def archive(self, format="tar.gz"):
        path = self.root / ("source." + format)
        self.git("archive", "--format=" + format, "--prefix=iris/", "-o", str(path), "HEAD")
        destination = self.root / "extracted"
        destination.mkdir()
        if format == "zip":
            with zipfile.ZipFile(path) as archive:
                archive.extractall(destination)
        else:
            # All members come from this test's own freshly created repository.
            with tarfile.open(path) as archive:
                archive.extractall(destination)
        source = destination / "iris"
        self.assertFalse((source / ".git").exists())
        self.assertFalse((source / ".version").exists())
        return source

    def configure(self, source, expected=None, *options):
        result = subprocess.run(
            ["cmake", "-S", str(source), "-B", str(self.root / "build"), *options],
            env=self.env, capture_output=True, text=True, timeout=30)
        if expected is None:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Cannot determine Iris version", result.stdout + result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((self.root / "build/resolved-version.txt").read_text(), expected)

    def test_checkout_ignores_placeholder_and_uses_git(self):
        self.git("tag", "v1.2.3")
        self.assertIn("$Format:", (self.repo / ".archive-version").read_text())
        self.configure(self.repo, "1.2.3")

    def test_worktree_git_file(self):
        self.git("tag", "v1.2.3")
        worktree = self.root / "worktree"
        self.git("worktree", "add", "--detach", str(worktree), "HEAD")
        self.assertTrue((worktree / ".git").is_file())
        self.configure(worktree, "1.2.3")

    def test_tar_archive_lightweight_tag_without_git(self):
        self.git("tag", "v1.2.3")
        source = self.archive()
        self.assertEqual((source / ".archive-version").read_text().strip(), "v1.2.3")
        self.configure(source, "1.2.3", "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")

    def test_zip_archive_annotated_tag_without_git(self):
        self.git("tag", "-a", "v2.4", "-m", "Release")
        source = self.archive("zip")
        self.configure(source, "2.4", "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")

    def test_branch_archive_uses_nearest_release_tag(self):
        self.git("tag", "v1.2.3")
        (self.repo / "next").write_text("Next commit\n")
        self.git("add", "next")
        self.git("commit", "-qm", "After release")
        self.configure(self.archive(), "1.2.3", "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")

    def test_untagged_archive_fails_clearly(self):
        self.git("tag", "experiment")
        self.configure(self.archive(), None, "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")

    def test_plain_copy_does_not_treat_placeholder_as_version(self):
        source = self.root / "copy"
        shutil.copytree(self.repo, source, ignore=shutil.ignore_patterns(".git"))
        self.configure(source, None, "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")

    def test_generated_version_takes_precedence(self):
        self.git("tag", "v1.2.3")
        source = self.archive()
        (source / ".version").write_text("v7.8.9\n")
        self.configure(source, "7.8.9", "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")

    def test_explicit_version_takes_precedence(self):
        self.git("tag", "v1.2.3")
        source = self.archive()
        (source / ".version").write_text("7.8.9\n")
        self.configure(source, "3.4.5", "-DIRIS_VERSION=v3.4.5", "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON")


if __name__ == "__main__":
    unittest.main()
