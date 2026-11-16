"""Exercise system/bundled JDNS selection without a compiler or network."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MIN_VERSION = json.loads((ROOT / "dependencies.lock.json").read_text())["jdns"]["minimum_version"]


class JDnsSelection(unittest.TestCase):
    def configure(self, mode="AUTO", version=None, qt=6, reject_wrong_qt=True,
                  existing=False, commit=None, extra=(), export_target=True,
                  android=False, bundled_metadata=True, reconfigure_commit=None):
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
            bundled_config = '''
cmake_minimum_required(VERSION 3.19)
add_library(qjdns INTERFACE)
add_library(QJDns::QJDns ALIAS qjdns)
'''
            if bundled_metadata:
                bundled_config += "set_property(TARGET qjdns PROPERTY INTERFACE_QJDNS_QT_MAJOR_VERSION ${JDNS_QT_MAJOR_VERSION})\n"
            (bundled / "CMakeLists.txt").write_text(bundled_config)
            setup = ""
            if version:
                package = root / "package"
                package.mkdir()
                rejection = """
if(NOT QT_DEFAULT_MAJOR_VERSION EQUAL %d)
    set(QJDns_FOUND FALSE)
    set(QJDns_NOT_FOUND_MESSAGE "QJDns wrapper is for Qt %d, not Qt 6")
    return()
endif()
""" % (qt, qt) if reject_wrong_qt else ""
                target = '''
add_library(QJDns::QJDns INTERFACE IMPORTED)
set_property(TARGET QJDns::QJDns PROPERTY INTERFACE_QJDNS_QT_MAJOR_VERSION %d)
''' % qt if export_target else ""
                (package / "QJDnsConfig.cmake").write_text(rejection + target + '''
set(QJDns_VERSION "%s")
''' % version)
                setup += '''
include(CMakePackageConfigHelpers)
write_basic_package_version_file("%s/QJDnsConfigVersion.cmake"
    VERSION "%s" COMPATIBILITY SameMajorVersion)
set(QJDns_DIR "%s")
''' % (package.as_posix(), version, package.as_posix())
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
''' + ('set(ANDROID TRUE)\n' if android else '') + setup + '''
include("${CMAKE_SOURCE_DIR}/cmake/modules/IrisJDns.cmake")
iris_configure_jdns()
''')
            command = ["cmake", "-S", str(root), "-B", str(root / "build")]
            result = subprocess.run(command + (["-DIRIS_SYSTEM_JDNS=" + mode] if mode is not None else []) + list(extra),
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if reconfigure_commit is not None and result.returncode == 0:
                lock["jdns"]["commit"] = reconfigure_commit
                (root / "dependencies.lock.json").write_text(json.dumps(lock))
                result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            result.cache_text = (root / "build/CMakeCache.txt").read_text()
            return result

    def test_iris_enables_jdns_by_default(self):
        # Keep release binaries and source builds from silently dropping remote DNS.
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertRegex(
            cmake,
            r'option\(IRIS_ENABLE_JDNS\s+"[^"]+"\s+ON\)',
        )

    def test_auto_without_system_bundles(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)
        self.assertIn("QJDnsConfig.cmake was not found", result.stdout)
        self.assertIn(f"JDNS {MIN_VERSION} (commit", result.stdout)
        self.assertIn("QJDns linked to Qt 6", result.stdout)

    def test_system_required_is_not_silently_bundled(self):
        result = self.configure(mode="ON")
        self.assertNotEqual(result.returncode, 0)
        # CMake formats fatal diagnostics at terminal width, inserting line breaks.
        # Check the message rather than its incidental whitespace.
        normalized = " ".join(result.stdout.split())
        self.assertIn("System QJDns >= " + MIN_VERSION, normalized)
        self.assertIn("QJDnsConfig.cmake was not found", normalized)

    def test_android_defaults_to_bundled_even_with_system_package(self):
        result = self.configure(mode=None, android=True, version=MIN_VERSION)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)
        self.assertNotIn("using system", result.stdout)

    def test_android_allows_explicit_target_sdk_package(self):
        result = self.configure(mode="ON", android=True, version=MIN_VERSION)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using system", result.stdout)

    def test_stale_bundled_source_reports_missing_metadata(self):
        result = self.configure(mode="OFF", bundled_metadata=False)
        self.assertNotEqual(result.returncode, 0)
        normalized = " ".join(result.stdout.split())
        self.assertIn("does not declare its Qt major version", normalized)
        self.assertIn("FETCHCONTENT_SOURCE_DIR_IRIS_JDNS", normalized)
        self.assertNotIn("_jdns_qt-NOTFOUND", normalized)

    def test_system_same_qt(self):
        result = self.configure(mode="ON", version=MIN_VERSION)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"using system QJDns {MIN_VERSION} (Qt 6 wrapper)", result.stdout)

    def test_off_ignores_system(self):
        result = self.configure(mode="OFF", version=MIN_VERSION)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)

    def test_old_and_wrong_major_packages_fall_back(self):
        for version in ("2.0.6", "3.0.0", "3.1.0", "4.0.0"):
            with self.subTest(version=version):
                result = self.configure(version=version)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("using bundled", result.stdout)
                self.assertIn(f"version {version}", result.stdout)

    def test_wrong_qt_falls_back(self):
        result = self.configure(version=MIN_VERSION, qt=5)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled", result.stdout)
        self.assertIn("QJDns wrapper is for Qt 5, not Qt 6", result.stdout)

    def test_system_config_without_qjdns_target_is_diagnosed(self):
        result = self.configure(version=MIN_VERSION, export_target=False)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("does not export QJDns::QJDns", result.stdout)
        self.assertIn("using bundled", result.stdout)
        required = self.configure(mode="ON", version=MIN_VERSION, export_target=False)
        self.assertNotEqual(required.returncode, 0)
        self.assertIn("does not export QJDns::QJDns", required.stdout)

    def test_existing_or_mispackaged_wrong_qt_target_is_rejected(self):
        for existing in (False, True):
            with self.subTest(existing=existing):
                result = self.configure(version=None if existing else MIN_VERSION, qt=5,
                                        reject_wrong_qt=False, existing=existing)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("QJDns must be built for Qt 6", result.stdout)

    def test_invalid_mode_and_lock_are_rejected(self):
        for args in ({"mode": "sometimes"}, {"commit": "not-a-sha"}):
            with self.subTest(args=args):
                self.assertNotEqual(self.configure(**args).returncode, 0)

    def test_legacy_revision_override_cannot_override_lock(self):
        result = self.configure(mode="OFF", extra=("-DIRIS_JDNS_GIT_TAG=custom-ref",))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("using bundled JDNS " + MIN_VERSION + " (commit ", result.stdout)
        self.assertNotIn("custom-ref", result.stdout)
        self.assertNotIn("IRIS_JDNS_GIT_TAG:", result.cache_text)

    def test_lock_update_is_used_in_existing_build_directory(self):
        result = self.configure(mode="OFF", commit="a" * 40, reconfigure_commit="b" * 40)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("commit " + "b" * 40, result.stdout)
        self.assertNotIn("IRIS_JDNS_GIT_TAG:", result.cache_text)
        self.assertNotIn("IRIS_JDNS_LOCK_DEFAULT:", result.cache_text)

    def test_previous_cached_default_advances_to_lock(self):
        locked = json.loads((ROOT / "dependencies.lock.json").read_text())["jdns"]["commit"]
        for extra in (
            ("-DIRIS_JDNS_GIT_TAG=0b7a9407b9401ed7680817ba3a368ce577ea87f2",),
            ("-DIRIS_JDNS_GIT_TAG=0357d0eebce7f50521350269d8461580e415a6c3",),
            ("-DIRIS_JDNS_GIT_TAG=" + "a" * 40, "-DIRIS_JDNS_LOCK_DEFAULT=" + "a" * 40),
        ):
            with self.subTest(extra=extra):
                result = self.configure(mode="OFF", extra=extra)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("using bundled JDNS " + MIN_VERSION + " (commit " + locked, result.stdout)


if __name__ == "__main__":
    unittest.main()
