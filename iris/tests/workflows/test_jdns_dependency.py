"""Exercise system/bundled JDNS selection without a compiler or network."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class JDnsSelection(unittest.TestCase):
    def configure(self, mode="AUTO", version=None, qt=6, reject_wrong_qt=True,
                  existing=False, commit=None, extra=()):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules = root / "cmake/modules"
            modules.mkdir(parents=True)
            shutil.copy(ROOT / "cmake/modules/IrisJDns.cmake", modules)
            lock = json.loads((ROOT / "dependencies.lock.json").read_text())
            if commit is not None:
                lock["jdns"]["commit"] = commit
            (root / "dependencies.lock.json").write_text(json.dumps(lock))
            bundled = root / "bundled"
            bundled.mkdir()
            (bundled / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.19)
add_library(qjdns INTERFACE)
add_library(QJDns::QJDns ALIAS qjdns)
set_property(TARGET qjdns PROPERTY INTERFACE_QJDNS_QT_MAJOR_VERSION ${JDNS_QT_MAJOR_VERSION})
''')
            setup = ""
            if version:
                package = root / "package"
                package.mkdir()
                rejection = """
if(NOT QT_DEFAULT_MAJOR_VERSION EQUAL %d)
    set(QJDns_FOUND FALSE)
    return()
endif()
""" % qt if reject_wrong_qt else ""
                (package / "QJDnsConfig.cmake").write_text(rejection + '''
add_library(QJDns::QJDns INTERFACE IMPORTED)
set_property(TARGET QJDns::QJDns PROPERTY INTERFACE_QJDNS_QT_MAJOR_VERSION %d)
set(QJDns_VERSION "%s")
''' % (qt, version))
                setup += '''
include(CMakePackageConfigHelpers)
write_basic_package_version_file("%s/QJDnsConfigVersion.cmake"
    VERSION "%s" COMPATIBILITY SameMajorVersion)
set(QJDns_DIR "%s")
''' % (package, version, package)
            else:
                setup += "set(CMAKE_DISABLE_FIND_PACKAGE_QJDns TRUE)\n"
            if existing:
                setup += '''
add_library(QJDns::QJDns INTERFACE IMPORTED)
set_property(TARGET QJDns::QJDns PROPERTY INTERFACE_QJDNS_QT_MAJOR_VERSION %d)
''' % qt
            (root / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.19)
project(jdns_selection NONE)
set(QT_DEFAULT_MAJOR_VERSION 6)
set(FETCHCONTENT_SOURCE_DIR_IRIS_JDNS "${CMAKE_SOURCE_DIR}/bundled")
''' + setup + '''
include("${CMAKE_SOURCE_DIR}/cmake/modules/IrisJDns.cmake")
iris_configure_jdns()
''')
            return subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                   "-DIRIS_SYSTEM_JDNS=" + mode, *extra],
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_auto_without_system_bundles(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)

    def test_system_required_is_not_silently_bundled(self):
        result = self.configure(mode="ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("System QJDns >= 3.0.0", result.stdout)

    def test_system_same_qt(self):
        result = self.configure(mode="ON", version="3.0.0")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using system QJDns 3.0.0 for Qt 6", result.stdout)

    def test_off_ignores_system(self):
        result = self.configure(mode="OFF", version="3.0.0")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)

    def test_old_and_wrong_major_packages_fall_back(self):
        for version in ("2.0.6", "4.0.0"):
            with self.subTest(version=version):
                result = self.configure(version=version)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("using bundled", result.stdout)

    def test_wrong_qt_falls_back(self):
        result = self.configure(version="3.0.0", qt=5)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)

    def test_existing_or_mispackaged_wrong_qt_target_is_rejected(self):
        for existing in (False, True):
            with self.subTest(existing=existing):
                result = self.configure(version=None if existing else "3.0.0", qt=5,
                                        reject_wrong_qt=False, existing=existing)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("QJDns must be built for Qt 6", result.stdout)

    def test_invalid_mode_and_lock_are_rejected(self):
        for args in ({"mode": "sometimes"}, {"commit": "not-a-sha"}):
            with self.subTest(args=args):
                self.assertNotEqual(self.configure(**args).returncode, 0)

    def test_explicit_source_override_is_preserved(self):
        result = self.configure(mode="OFF", extra=("-DIRIS_JDNS_GIT_TAG=custom-ref",))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled custom-ref", result.stdout)

    def test_previous_cached_default_advances_to_lock(self):
        locked = json.loads((ROOT / "dependencies.lock.json").read_text())["jdns"]["commit"]
        for extra in (
            ("-DIRIS_JDNS_GIT_TAG=0b7a9407b9401ed7680817ba3a368ce577ea87f2",),
            ("-DIRIS_JDNS_GIT_TAG=" + "a" * 40, "-DIRIS_JDNS_LOCK_DEFAULT=" + "a" * 40),
        ):
            with self.subTest(extra=extra):
                result = self.configure(mode="OFF", extra=extra)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("using bundled " + locked, result.stdout)


if __name__ == "__main__":
    unittest.main()
