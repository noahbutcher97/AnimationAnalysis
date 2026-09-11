"""Repository launcher tests; intentionally outside the installed package suite."""
import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class HostToolsTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec("host_tools"), "host tooling is not implemented")
        import host_tools
        self.tools = host_tools
        self.temporary = tempfile.TemporaryDirectory(prefix="animation-host-tools-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def file(self, relative, contents=b"data"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def plugin(self):
        for name in ("AnimationAnalysis.uplugin", "Source/Capture/Public/Capture.h",
                     "Python/UnrealHost/AnimationCaptureHost.uproject",
                     "Python/UnrealHost/Source/Host/Host.Build.cs"):
            self.file("plugin/" + name)
        return self.root / "plugin"

    def test_staging_excludes_generated_and_unrelated_source_files(self):
        plugin = self.plugin()
        self.file("plugin/Saved/Other/Other.uplugin")
        self.file("plugin/Source/Capture/Intermediate/Generated.cpp")
        self.file("plugin/Python/UnrealHost/Saved/Generated.cpp")
        self.file("plugin/Source/Capture/model.uasset")
        self.file("plugin/Shaders/Readback.usf")
        self.file("plugin/Shaders/Shared.ush")
        self.file("plugin/Shaders/Unrelated.cpp")
        hashes = self.tools.stage_sources(plugin, self.root / "host")
        self.assertEqual(set(hashes), {"AnimationCaptureHost.uproject", "Source/Host/Host.Build.cs",
            "Plugins/AnimationAnalysis/AnimationAnalysis.uplugin",
            "Plugins/AnimationAnalysis/Source/Capture/Public/Capture.h",
            "Plugins/AnimationAnalysis/Shaders/Readback.usf", "Plugins/AnimationAnalysis/Shaders/Shared.ush"})
        self.assertEqual(hashes["AnimationCaptureHost.uproject"],
                         "3a6eb0790f39ac87c94f3856b2dd2c5d110e6811602261a9a923d3bb23adc8b7")
        self.assertTrue((self.root / "host/Content").is_dir())

    def test_retained_host_rejects_unmanaged_or_modified_files(self):
        plugin = self.plugin()
        host = plugin / "Saved/DevelopmentHost"
        self.tools.prepare_host(plugin, host)
        (host / "AnimationCaptureHost.uproject").write_text("local edit")
        with self.assertRaises(ValueError):
            self.tools.prepare_host(plugin, host)
        unmanaged = plugin / "Saved/Unmanaged"
        unmanaged.mkdir()
        (unmanaged / "notes.txt").write_text("keep")
        with self.assertRaises(ValueError):
            self.tools.prepare_host(plugin, unmanaged)
        with self.assertRaises(ValueError):
            self.tools.prepare_host(plugin, plugin / "Source/BadHost")

    def test_prepare_refreshes_changed_sources_preserving_build_outputs(self):
        plugin = self.plugin()
        host = plugin / "Saved/DevelopmentHost"
        self.tools.prepare_host(plugin, host)
        self.file("plugin/Saved/DevelopmentHost/Binaries/test.dll", b"compiled")
        (plugin / "Source/Capture/Public/Capture.h").unlink()
        self.file("plugin/Source/Capture/Public/New.h", b"new source")
        hashes = self.tools.prepare_host(plugin, host)
        self.assertFalse((host / "Plugins/AnimationAnalysis/Source/Capture/Public/Capture.h").exists())
        self.assertIn("Plugins/AnimationAnalysis/Source/Capture/Public/New.h", hashes)
        self.assertEqual((host / "Binaries/test.dll").read_bytes(), b"compiled")

    def test_results_reject_duplicates_missing_unexpected_failures_and_skips(self):
        expected = ("AnimationAnalysis.Capture.One", "AnimationAnalysis.Capture.Two")
        lines = [f"Test Completed. Result={{Success}} Test={{Control}} Path={{{name}}}" for name in expected]
        good = "\n".join(lines)
        self.assertEqual(self.tools.check_test_results(good, expected)["status"], "verified")
        for bad in (lines[0], good + "\n" + lines[0], good.replace("Success", "Fail", 1),
                    good + "\nTest Completed. Result={Success} Path={AnimationAnalysis.Capture.Other}"):
            self.assertEqual(self.tools.check_test_results(bad, expected)["status"], "failed")
        skipped = self.tools.check_test_results(good + "\nANIMATION_ANALYSIS_RENDERED_SKIP: No viewport", expected, rendered=True)
        self.assertEqual(skipped["status"], "skipped")
        failed_and_skipped = self.tools.check_test_results(
            good.replace("Success", "Fail", 1) + "\nANIMATION_ANALYSIS_RENDERED_SKIP: No viewport", expected, rendered=True)
        self.assertEqual(failed_and_skipped["status"], "failed")
        self.assertEqual(self.tools.check_test_results(good, expected, rendered=True)["status"], "failed")
        rendered = "LogRHI: Using Default RHI: D3D11\n" + good
        self.assertEqual(self.tools.check_test_results(rendered, expected, rendered=True)["status"], "verified")

    def test_replay_round_trip_verifies_all_entries_before_png_cleanup(self):
        run = self.root / "run"
        self.file("run/capture/frame.png", b"image bytes")
        self.file("run/capture/depth.bin", b"depth bytes")
        self.file("run/capture/session.json", b"{}")
        self.file("run/unlisted.png", b"keep")
        inventory = self.tools.create_inventory(run, ["capture/frame.png", "capture/depth.bin", "capture/session.json"])
        archive = self.root / "replay.zip"
        result = self.tools.archive_replay(run, inventory, archive, delete_pngs=True)
        self.assertEqual(result["verified_entries"], 3)
        self.assertEqual(result["deleted_pngs"], ["capture/frame.png"])
        self.assertFalse((run / "capture/frame.png").exists())
        self.assertTrue((run / "capture/depth.bin").exists())
        self.assertTrue((run / "unlisted.png").exists())
        self.assertEqual(self.tools.verify_archive(archive, inventory)["verified_entries"], 3)

    def test_replay_refuses_tampering_and_paths_outside_inventory_root(self):
        run = self.root / "run"
        png = self.file("run/frame.png", b"image")
        inventory = self.tools.create_inventory(run, ["frame.png"])
        png.write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.tools.archive_replay(run, inventory, self.root / "bad.zip", delete_pngs=True)
        self.assertTrue(png.exists())
        for path in ("../elsewhere.png", "C:/elsewhere.png", "/elsewhere.png", "a/../frame.png", "frame.png:stream"):
            with self.assertRaises(ValueError):
                self.tools.create_inventory(run, [path])
        with self.assertRaises(ValueError):
            self.tools.create_inventory(run, ["frame.png", "frame.png"])
        png.write_bytes(b"image")
        with self.assertRaises(ValueError):
            self.tools.archive_replay(self.root, inventory, self.root / "wrong-run.zip", delete_pngs=True)

    def test_archive_extra_or_modified_entries_never_verify(self):
        run = self.root / "run"
        self.file("run/frame.png", b"image")
        inventory = self.tools.create_inventory(run, ["frame.png"])
        archive = self.root / "replay.zip"
        self.tools.archive_replay(run, inventory, archive)
        with zipfile.ZipFile(archive, "a") as saved:
            saved.writestr("unlisted.txt", "unexpected")
        with self.assertRaises(ValueError):
            self.tools.verify_archive(archive, inventory)
        altered = self.root / "altered.zip"
        with zipfile.ZipFile(altered, "w") as saved:
            saved.writestr("_replay_inventory.json", json.dumps(inventory))
            saved.writestr("frame.png", b"wrong")
        with self.assertRaises(ValueError):
            self.tools.verify_archive(altered, inventory)

    def test_reparse_points_cannot_redirect_staging_or_replay_cleanup(self):
        run = self.root / "run"
        run.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "frame.png").write_bytes(b"keep")
        link = run / "linked"
        if os.name == "nt":
            result = subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(outside)],
                                    capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        else:
            link.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.tools.create_inventory(run, ["linked/frame.png"])
        with self.assertRaises(ValueError):
            self.tools.stage_sources(self.plugin(), link / "host")
        self.assertEqual((outside / "frame.png").read_bytes(), b"keep")

    def test_timeout_kills_spawned_child_and_retains_failure_record(self):
        child_marker = self.root / "child-survived.txt"
        script = self.file("parent.py", (
            "import subprocess, sys, time\n"
            "subprocess.Popen([sys.executable, '-c', "
            "\"import pathlib,time;time.sleep(1.5);pathlib.Path(\" + repr(sys.argv[1]) + \" ).write_text('survived')\"])\n"
            "print('child spawned', flush=True)\n"
            "time.sleep(30)\n").encode())
        records = []
        with self.assertRaises(self.tools.ProcessFailure):
            self.tools.run_process([sys.executable, script, child_marker], self.root, self.root / "logs", "timeout.log", .6, records)
        time.sleep(1.1)
        self.assertFalse(child_marker.exists())
        self.assertEqual(records[0]["status"], "timed_out")
        self.assertTrue(records[0]["tree_cleanup_complete"])
        self.assertIn("child spawned", (self.root / "logs/timeout.log").read_text())
        self.assertEqual(json.loads((self.root / "logs/host-commands.json").read_text())[0]["status"], "timed_out")

    def test_process_failure_and_launch_failure_are_retained(self):
        for arguments in ([sys.executable, "-c", "raise SystemExit(7)"], [self.root / "missing.exe"]):
            records = []
            with self.assertRaises(self.tools.ProcessFailure):
                self.tools.run_process(arguments, self.root, self.root / "logs", "failed.log", 5, records)
            self.assertEqual(records[0]["status"], "failed")
            self.assertTrue((self.root / "logs/host-commands.json").exists())

    def test_timeout_cleanup_exception_is_recorded_as_unconfirmed_cleanup(self):
        original_cleanup = self.tools._terminate_tree

        def lose_cleanup_confirmation(process):
            original_cleanup(process)
            raise OSError("cleanup confirmation unavailable")

        records = []
        with patch.object(self.tools, "_terminate_tree", side_effect=lose_cleanup_confirmation):
            with self.assertRaises(self.tools.ProcessFailure):
                self.tools.run_process([sys.executable, "-c", "import time; time.sleep(30)"],
                                      self.root, self.root / "logs", "timeout.log", .1, records)
        self.assertEqual(records[0]["status"], "timed_out")
        self.assertFalse(records[0]["tree_cleanup_complete"])
        self.assertIn("cleanup confirmation unavailable", records[0]["tree_cleanup_detail"])

    def test_missing_engine_reports_failure_and_removes_isolated_host(self):
        import verify_unreal_host
        output = self.root / "verification"
        with self.assertRaises((RuntimeError, OSError)):
            verify_unreal_host.verify(self.root / "missing-engine", output)
        self.assertTrue((output / "host-verification.json").is_file(), "missing-engine failure evidence is not retained")
        report = json.loads((output / "host-verification.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["temporary_cleanup"], "removed")
        self.assertFalse(Path(report["scratch"]).exists())
        self.assertTrue((output / "host-source.json").is_file())

    def test_stage_only_prepares_without_claiming_a_build_or_runtime_pass(self):
        import verify_unreal_host
        self.assertTrue(hasattr(verify_unreal_host, "prepare"), "retained-host workflow is not implemented")
        plugin = self.plugin()
        output = self.root / "preparation"
        with contextlib.redirect_stdout(io.StringIO()):
            result = verify_unreal_host.prepare(self.root / "missing-engine", output,
                plugin / "Saved/DevelopmentHost", plugin=plugin, stage_only=True)
        self.assertEqual(result["status"], "prepared")
        self.assertEqual(result["build_status"], "not_run")
        self.assertEqual(result["runtime_status"], "not_run")
        self.assertTrue((plugin / "Saved/DevelopmentHost/AnimationCaptureHost.uproject").is_file())
        with self.assertRaises(ValueError):
            verify_unreal_host.prepare(self.root / "missing-engine", output,
                plugin / "Saved/DevelopmentHost", plugin=plugin, stage_only=True)

    def test_mode_arguments_select_exact_tests_and_interactive_entry_map(self):
        import verify_unreal_host
        self.assertTrue(hasattr(verify_unreal_host, "editor_arguments"), "mode-aware launch arguments are not implemented")
        expected = ("AnimationAnalysis.Capture.One", "AnimationAnalysis.Capture.Two")
        for rendered in (False, True):
            args = verify_unreal_host.editor_arguments(self.root, self.root / "host", self.root / "evidence",
                                                      rendered=rendered, expected_tests=expected)
            self.assertEqual("-NullRHI" in args, not rendered)
            self.assertEqual("-d3d11" in args, rendered)
            self.assertIn("-ExecCmds=Automation RunTests AnimationAnalysis.Capture.One+AnimationAnalysis.Capture.Two;Quit", args)
            self.assertIn("/Engine/Maps/Entry", args)
        interactive = verify_unreal_host.editor_arguments(self.root, self.root / "host", self.root / "evidence", interactive=True)
        self.assertFalse(any("Automation" in str(value) for value in interactive))
        self.assertIn("/Engine/Maps/Entry", interactive)

    def test_retained_run_archives_only_new_observations(self):
        import verify_unreal_host
        self.assertTrue(hasattr(verify_unreal_host, "observation_snapshot"), "retained run inventory isolation is not implemented")
        host = self.root / "host"
        self.file("host/Saved/Observations/prior/frame.png", b"old image")
        before = verify_unreal_host.observation_snapshot(host)
        self.file("host/Saved/Observations/current/frame.png", b"new image")
        self.file("host/Saved/Observations/current/session.json", b"{}")
        output = self.root / "evidence"
        result = verify_unreal_host._retain_observations(host, output, before=before)
        self.assertEqual(result["verified_entries"], 2)
        self.assertFalse((output / "observations/prior").exists())
        self.assertTrue((output / "observations/current/frame.png").is_file())
        self.file("host/Saved/Observations/prior/frame.png", b"overwritten")
        with self.assertRaises(ValueError):
            verify_unreal_host._retain_observations(host, self.root / "bad-evidence", before=before)


if __name__ == "__main__":
    unittest.main()
