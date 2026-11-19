"""Check explicit GNUInstallDirs lib paths and the legacy LIB_SUFFIX fallback."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LibraryInstallPath(unittest.TestCase):
    def test_explicit_libdir_wins_over_legacy_suffix(self):
        cases = (
            ("lib64", "64", "/usr/lib64"),
            ("lib", "64", "/usr/lib"),
            ("lib/x86_64-linux-gnu", "64", "/usr/lib/x86_64-linux-gnu"),
            ("lib/my-custom-layout", "64", "/usr/lib/my-custom-layout"),
            ("vendor/private-libraries", "64", "/usr/vendor/private-libraries"),
            ("/opt/psi/jdns/lib", "64", "/opt/psi/jdns/lib"),
            ("lib64", "", "/usr/lib64"),
            (None, "64", "/usr/lib64"),
        )
        for libdir, suffix, expected in cases:
            with self.subTest(libdir=libdir, suffix=suffix):
                with tempfile.TemporaryDirectory() as directory:
                    build = Path(directory) / "build"
                    args = [
                        "cmake", "-S", str(ROOT), "-B", str(build),
                        "-DCMAKE_INSTALL_PREFIX=/usr",
                        "-DBUILD_QJDNS=OFF", "-DBUILD_JDNS_TOOL=OFF",
                        "-DJDNS_ENABLE_INSTALL=ON",
                        f"-DLIB_SUFFIX={suffix}",
                    ]
                    if libdir is not None:
                        args.append(f"-DCMAKE_INSTALL_LIBDIR={libdir}")
                    result = subprocess.run(
                        args, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout)
                    cache = (build / "CMakeCache.txt").read_text()
                    self.assertIn(f"LIB_INSTALL_DIR:PATH={expected}\n", cache)


if __name__ == "__main__":
    unittest.main()
