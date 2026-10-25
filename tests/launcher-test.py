import os
from pathlib import Path
import pwd
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.geteuid() == 0, "Root is required for ownership fixtures")
class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="brave-launcher-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app = self.root / "app"
        self.data = self.root / "data"
        for name in ("engine", "runtime/lib", "display"):
            (self.app / name).mkdir(parents=True)
        self.executables = [
            "engine/brave", "webos-display", "webos-media", "browser-exec",
            "runtime/lib/ld-linux-aarch64.so.1", "launch-engine.sh",
        ]
        for name in self.executables:
            (self.app / name).write_text("#!/bin/sh\necho STARTED\n")
            (self.app / name).chmod(0o755)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.bin / "luna-send-pub").write_text("#!/bin/sh\nexit 0\n")
        (self.bin / "luna-send-pub").chmod(0o755)
        self.environment = dict(os.environ, PATH=str(self.bin) + ":" + os.environ["PATH"])

    def run_launcher(self, name):
        text = (PROJECT / "app" / name).read_text()
        text = text.replace(
            "APP=/media/developer/apps/usr/palm/applications/org.webosbrew.brave.b9\n",
            f"APP={self.app}\n",
        )
        text = text.replace("DATA=/media/developer/brave-b9-data\n", f"DATA={self.data}\n")
        text = text.replace("CACHE=/media/developer/brave-b9-hot\n", f"CACHE={self.root}/cache\n")
        text = text.replace(
            "RUN=$(mktemp -d /dev/shm/brave-b9-XXXXXX)",
            f"RUN=$(mktemp -d {self.root}/run-XXXXXX)",
        )
        script = self.root / name
        script.write_text(text)
        return subprocess.run(
            ["/bin/sh", str(script)], env=self.environment,
            capture_output=True, text=True, timeout=15,
        )

    def test_boot_reset_is_repaired_and_relaunch_works(self):
        for directory in (self.app, self.app / "engine", self.app / "runtime",
                          self.app / "display"):
            directory.chmod(0o777)
        for name in self.executables:
            (self.app / name).chmod(0o777)
        for _ in range(2):
            result = self.run_launcher("launch-bundled.sh")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("STARTED", result.stdout)
        for name in self.executables:
            self.assertEqual((self.app / name).stat().st_mode & 0o777, 0o755)

    def test_wrong_owner_is_not_repaired(self):
        file = self.app / "engine/brave"
        os.chown(file, pwd.getpwnam("nobody").pw_uid, -1)
        file.chmod(0o777)
        result = self.run_launcher("launch-bundled.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(file.stat().st_mode & 0o777, 0o777)

    def test_symlink_is_not_repaired(self):
        file = self.app / "engine/brave"
        target = self.root / "outside"
        target.write_text("untouched")
        target.chmod(0o777)
        file.unlink()
        file.symlink_to(target)
        result = self.run_launcher("launch-bundled.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.stat().st_mode & 0o777, 0o777)

    def test_unexpected_mode_is_not_repaired(self):
        file = self.app / "engine/brave"
        file.chmod(0o775)
        result = self.run_launcher("launch-bundled.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(file.stat().st_mode & 0o777, 0o775)

    def prepare_data(self, owner):
        self.data.mkdir()
        (self.data / "profile").mkdir()
        marker = self.data / "profile/keep"
        marker.write_text("existing profile")
        os.chown(self.data, owner, -1)
        self.data.chmod(0o777)
        return marker

    def test_profile_boot_reset_preserves_data(self):
        marker = self.prepare_data(pwd.getpwnam("nobody").pw_uid)
        (self.app / "browser-exec").write_text("#!/bin/sh\nexit 0\n")
        result = self.run_launcher("launch.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.data.stat().st_mode & 0o777, 0o700)
        self.assertEqual(marker.read_text(), "existing profile")

    def test_wrong_profile_owner_is_not_repaired(self):
        marker = self.prepare_data(0)
        result = self.run_launcher("launch.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.data.stat().st_mode & 0o777, 0o777)
        self.assertEqual(marker.read_text(), "existing profile")

    def test_profile_symlink_is_not_repaired(self):
        target = self.root / "outside-data"
        target.mkdir()
        os.chown(target, pwd.getpwnam("nobody").pw_uid, -1)
        target.chmod(0o777)
        self.data.symlink_to(target, target_is_directory=True)
        result = self.run_launcher("launch.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.stat().st_mode & 0o777, 0o777)


if __name__ == "__main__":
    unittest.main()
