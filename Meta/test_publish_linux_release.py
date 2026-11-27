"""Regression tests for the Linux release publisher, with GitHub calls mocked."""

import runpy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = runpy.run_path(Path(__file__).with_name("publish-linux-release.py"))
publish = SCRIPT["publish"]
CHECKSUMS = SCRIPT["CHECKSUMS"]
digest = SCRIPT["digest"]
COMMIT = "a" * 40
VERSION = "0.1.0-agentic"


class PublishLinuxReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def stage(self, *names):
        packages = []
        for name in names:
            file = self.directory / name
            file.write_bytes(name.encode())
            packages.append(file)
        manifest = self.directory / CHECKSUMS
        manifest.write_text("".join(f"{digest(file)}  {file.name}\n" for file in packages), encoding="utf-8")
        return [*packages, manifest]

    def release(self, package_format, run_id="12"):
        publish(self.directory, "example/ladybird", COMMIT, VERSION, package_format, run_id)

    def test_complete_and_partial_formats_use_distinct_tags(self):
        self.stage("ladybird.AppImage")
        with patch.dict(publish.__globals__, github_json=lambda _: None), patch("subprocess.run") as command:
            self.release("appimage-only")
            partial = command.call_args.args[0]
            self.assertIn("--prerelease", partial)
            self.assertIn("--latest=false", partial)
            self.stage("ladybird.AppImage", "ladybird.deb")
            self.release("appimage-and-deb")
            complete = command.call_args.args[0]
            self.assertIn("--latest", complete)
            self.assertNotIn("--prerelease", complete)
            self.assertNotEqual(partial[3], complete[3])
            self.assertIn(COMMIT, complete[3])

    def test_missing_or_changed_assets_fail_before_publishing(self):
        self.stage("ladybird.AppImage")
        with patch("subprocess.run") as command:
            with self.assertRaisesRegex(ValueError, "Expected exactly"):
                self.release("appimage-and-deb")
            (self.directory / "ladybird.AppImage").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "Checksum manifest does not match"):
                self.release("appimage-only")
            command.assert_not_called()

    def test_retry_verifies_existing_release_without_a_write(self):
        files = self.stage("ladybird.deb")
        assets = [{"name": file.name, "digest": f"sha256:{digest(file)}", "state": "uploaded"} for file in files]
        existing = {"assets": assets, "draft": False, "prerelease": True}

        def github_json(endpoint):
            return {"sha": COMMIT} if "/commits/" in endpoint else existing

        with patch.dict(publish.__globals__, github_json=github_json), patch("subprocess.run") as command:
            self.release("deb-only", run_id="13")
            assets[0]["digest"] = "sha256:" + "0" * 64
            with self.assertRaisesRegex(ValueError, "different metadata or assets"):
                self.release("deb-only", run_id="14")
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
