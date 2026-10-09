"""Check QCA version selection and the actual split-deb packaging step.

Run with python3 -m unittest discover -s tests/workflows -v.
No network or system package installation is used.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('cmake'), 'CMake is required')
class QcaMinimumVersion(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='iris-qca-min-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        module = self.root / 'cmake/modules/IrisQCA.cmake'
        module.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / 'cmake/modules/IrisQCA.cmake', module)
        (self.root / 'dependencies.lock.json').write_text(json.dumps(
            {'qca': {'tag': 'v3.0.10', 'commit': 'a' * 40, 'minimum_version': '3.0.10'}}))
        (self.root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.11)
project(qca_minimum LANGUAGES NONE)
set(QT_DEFAULT_MAJOR_VERSION 6)
set(IRIS_BUNDLED_QCA OFF)
set(IRIS_SYSTEM_QCA 3 CACHE STRING "QCA generation")
if(PRELOAD_QCA)
    find_package(Qca3-qt6 CONFIG REQUIRED)
endif()
include(cmake/modules/IrisQCA.cmake)
if(GENERATE_SDK)
    set(IRIS_LIB_VERSION_STRING 1.1.2)
    set(IRIS_LIB_VERSION_MAJOR 1)
    set(IRIS_LIB_VERSION_MINOR 1)
    set(IRIS_LIB_VERSION_PATCH 2)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(IrisConfig.cmake.in "${CMAKE_BINARY_DIR}/sdk/IrisConfig.cmake"
                                  INSTALL_DESTINATION lib/cmake/Iris)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/IrisTargets.cmake"
         "add_library(Iris::Iris INTERFACE IMPORTED)\n")
endif()
''')
        shutil.copyfile(ROOT / 'IrisConfig.cmake.in', self.root / 'IrisConfig.cmake.in')
        self.package = self.root / 'prefix/lib/cmake/Qca3-qt6'
        self.package.mkdir(parents=True)
        (self.package / 'Qca3-qt6Config.cmake').write_text(
            'if(NOT TARGET Qca3::Qca)\n'
            '    add_library(Qca3::Qca INTERFACE IMPORTED)\n'
            'endif()\n')

    def configure(self, version, success=True, *options):
        (self.package / 'Qca3-qt6ConfigVersion.cmake').write_text(f'''
set(PACKAGE_VERSION "{version}")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
''')
        result = subprocess.run(['cmake', '-S', str(self.root), '-B', str(self.build),
                                 '-DCMAKE_PREFIX_PATH=' + str(self.root / 'prefix'),
                                 '-DCMAKE_FIND_ROOT_PATH=' + str(self.root),
                                 '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY', *options],
                                capture_output=True, text=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((self.build / 'iris-qca-min-version.txt').read_text().strip(), '3.0.10')
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('>= 3.0.10', result.stdout + result.stderr)

    def test_qca_306_rejected(self):
        self.configure('3.0.6', False)

    def test_qca_309_rejected(self):
        self.configure('3.0.9', False)

    def test_qca_3010_accepted(self):
        self.configure('3.0.10')

    def test_newer_qca_accepted(self):
        self.configure('3.0.11')

    def test_preloaded_old_target_rejected(self):
        self.configure('3.0.9', False, '-DPRELOAD_QCA=ON')

    def check_sdk(self, version, preload=False):
        self.configure('3.0.10', True, '-DGENERATE_SDK=ON')
        # Change the consumer's installed QCA after building the Iris SDK.
        (self.package / 'Qca3-qt6ConfigVersion.cmake').write_text(f"""
set(PACKAGE_VERSION "{version}")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
""")
        qt = self.root / 'prefix/lib/cmake/Qt6'
        qt.mkdir()
        (qt / 'Qt6Config.cmake').write_text('set(Qt6_FOUND TRUE)\n')
        (qt / 'Qt6ConfigVersion.cmake').write_text(
            'set(PACKAGE_VERSION 6.4.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n')
        consumer = self.root / 'consumer'
        consumer.mkdir()
        (consumer / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.11)\nproject(consumer LANGUAGES NONE)\n'
            + ('find_package(Qca3-qt6 CONFIG REQUIRED)\n' if preload else '')
            + 'find_package(Iris CONFIG REQUIRED)\n')
        result = subprocess.run(['cmake', '-S', str(consumer), '-B', str(self.root / 'consumer-build'),
                                 '-DIris_DIR=' + str(self.build / 'sdk'),
                                 '-DCMAKE_PREFIX_PATH=' + str(self.root / 'prefix'),
                                 '-DCMAKE_FIND_ROOT_PATH=' + str(self.root),
                                 '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY'],
                                capture_output=True, text=True, timeout=30)
        if version == '3.0.9':
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('3.0.10', result.stdout + result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sdk_rejects_old_qca(self):
        self.check_sdk('3.0.9')

    def test_sdk_rejects_preloaded_old_qca(self):
        self.check_sdk('3.0.9', preload=True)

    def test_sdk_accepts_newer_qca(self):
        self.check_sdk('3.0.11')

    @unittest.skipUnless(all(shutil.which(name) for name in ('cc', 'dpkg-deb', 'dpkg-shlibdeps')),
                         'C compiler and Debian packaging tools are required')
    def test_real_deb_controls_require_minimum_without_pin_or_duplicate(self):
        self.configure('3.0.10')
        library_dir = self.root / 'stage/usr/lib'
        library_dir.mkdir(parents=True)
        qca = self.root / 'qca.c'
        qca.write_text('int qca(void) { return 1; }\n')
        qca_library = self.root / 'libqca3-qt6.so.3'
        subprocess.run(['cc', '-shared', '-fPIC', str(qca), '-Wl,-soname,libqca3-qt6.so.3',
                        '-o', str(qca_library)], check=True, capture_output=True)
        iris = self.root / 'iris.c'
        iris.write_text('#include <stdio.h>\nint qca(void);\nint iris(void) { puts("test"); return qca(); }\n')
        runtime = library_dir / 'libiris-qt6.so.1.1.2'
        subprocess.run(['cc', '-shared', '-fPIC', str(iris), str(qca_library),
                        '-Wl,-soname,libiris-qt6.so.1', '-Wl,-rpath,' + str(self.root),
                        '-o', str(runtime)], check=True, capture_output=True)
        (library_dir / 'libiris-qt6.so').symlink_to(runtime.name)
        header = self.root / 'stage/usr/include/iris/test.h'
        header.parent.mkdir(parents=True)
        header.write_text('int iris(void);\n')

        action = (ROOT / '.github/actions/build-iris-deb/action.yml').read_text()
        step = action.split('  - name: Build split deb packages\n', 1)[1]
        step = step.split('  - name: Install package smoke test\n', 1)[0]
        script = textwrap.dedent(step.split('    run: |\n', 1)[1])
        script = script.replace('${{ inputs.version }}', '1.1.2')
        result = subprocess.run(['bash', '-c', script], cwd=self.root,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        debs = list((self.root / 'debs').glob('*.deb'))
        self.assertEqual(len(debs), 2)
        for deb in debs:
            depends = subprocess.check_output(['dpkg-deb', '-f', str(deb), 'Depends'], text=True)
            entries = [entry.strip() for entry in depends.split(',')]
            if deb.name.startswith('libiris-qt6-dev_'):
                qca_dependency = 'libqca3-qt6-dev (>= 3.0.10)'
                self.assertIn('libiris-qt6-1 (= 1.1.2)', entries)
            else:
                qca_dependency = 'libqca3-qt6-3 (>= 3.0.10)'
                self.assertTrue(any(entry.startswith('libc6 ') for entry in entries))
            qca_entries = [entry for entry in entries if entry.startswith('libqca3-')]
            self.assertEqual(qca_entries, [qca_dependency])
            self.assertEqual(subprocess.run(['dpkg', '--compare-versions', '3.0.9', 'ge', '3.0.10']).returncode, 1)
            self.assertEqual(subprocess.run(['dpkg', '--compare-versions', '3.0.11', 'ge', '3.0.10']).returncode, 0)


if __name__ == '__main__':
    unittest.main()
