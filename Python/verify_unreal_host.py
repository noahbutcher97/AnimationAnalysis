"""Build and verify an isolated UE 5.6 host, or prepare its retained development copy.

NullRHI verifies lifecycle controls only. --rendered selects D3D11 geometry controls;
rendered skips never become passes. All runs require a fresh evidence directory.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import time

from host_tools import (LIFECYCLE_TESTS, RENDERED_TESTS, ProcessFailure, archive_replay,
                        check_test_results, checked_path, create_inventory, prepare_host,
                        run_process, safe_relative, sha256_file, stage_sources, write_json)


def _new_output(output):
    output = checked_path(output)
    if output.exists() and any(output.iterdir()):
        raise ValueError("Use a new empty evidence directory; existing results must not be overwritten")
    output.mkdir(parents=True, exist_ok=True)
    return output


def _expected_tests(rendered, expected_tests):
    expected = tuple(expected_tests) if expected_tests is not None else LIFECYCLE_TESTS + (RENDERED_TESTS if rendered else ())
    if not expected or len(set(expected)) != len(expected) or any(
            not re.fullmatch(r"AnimationAnalysis\.Capture\.[A-Za-z0-9_.]+", name) for name in expected):
        raise ValueError("Tests must be unique exact AnimationAnalysis.Capture paths")
    return expected


def editor_arguments(engine, host, output, *, rendered=False, expected_tests=None, interactive=False):
    engine, host, output = Path(engine), Path(host), Path(output)
    executable = "UnrealEditor.exe" if interactive else "UnrealEditor-Cmd.exe"
    arguments = [str(engine / "Engine/Binaries/Win64" / executable), str(host / "AnimationCaptureHost.uproject"),
                 "/Engine/Maps/Entry", "-nosplash", "-nopause", f"-abslog={output / 'host-editor.log'}"]
    if interactive or rendered:
        arguments += ["-d3d11", "-windowed", "-ResX=640", "-ResY=480", "-NoVSync"]
    else:
        arguments += ["-NullRHI"]
    if not interactive:
        tests = _expected_tests(rendered, expected_tests)
        arguments += [f"-ExecCmds=Automation RunTests {'+'.join(tests)};Quit", "-unattended", "-stdout"]
    return arguments


def _build(engine, host, output, timeout, commands):
    run_process([engine / "Engine/Build/BatchFiles/Build.bat", "AnimationCaptureHostEditor", "Win64", "Development",
                 f"-Project={host / 'AnimationCaptureHost.uproject'}", "-NoHotReload", "-NoHotReloadFromIDE",
                 "-MaxParallelActions=2", "-WaitMutex"],
                host, output, "host-build.log", timeout, commands)


def _identity(engine, host, output, hashes):
    write_json(output / "host-source.json", hashes)
    tools = Path(__file__).resolve().parent
    write_json(output / "tool-source.json", {name: sha256_file(tools / name)
                                           for name in ("verify_unreal_host.py", "host_tools.py")})
    version_file = engine / "Engine/Build/Build.version"
    version = json.loads(version_file.read_text(encoding="utf-8-sig")) if version_file.is_file() else None
    identity = {"engine": str(engine), "engine_version": version, "host": str(host),
                "platform": platform.platform(), "python_version": platform.python_version(),
                "host_source_files": len(hashes), "dependencies": "Engine and copied AnimationCapture plugin only"}
    write_json(output / "host-environment.json", identity)
    return identity


def observation_snapshot(host):
    """Fingerprint pre-existing observations so a retained run cannot claim them."""
    origin = checked_path(host / "Saved/Observations")
    if not origin.exists():
        return {"directories": [], "files": {}}
    paths = sorted(path.relative_to(origin).as_posix() for path in origin.rglob("*") if path.is_file())
    return {"directories": sorted(path.name for path in origin.iterdir() if path.is_dir()),
            "files": {name: sha256_file(safe_relative(origin, name)) for name in paths}}


def _confirmed_process_cleanup(commands):
    return bool(commands and commands[-1].get("status") in ("timed_out", "interrupted")
                and commands[-1].get("tree_cleanup_complete") is True)


class _RetentionReads:
    """Bound only transient source-read permission failures after owned cleanup."""
    def __init__(self, output, report, retry_eligible):
        self.output = Path(output)
        self.report = report
        report.update(status="not_needed", retry_eligible=retry_eligible,
                      max_attempts=4 if retry_eligible else 1, max_retry_wait_seconds=1.75,
                      retry_wait_seconds=0.0, attempts=[], recovered_reads=[])

    def save(self):
        write_json(self.output / "retention-attempts.json", self.report)

    def read(self, operation, path, purpose):
        for attempt in range(1, self.report["max_attempts"] + 1):
            try:
                result = operation()
            except PermissionError as error:
                self.report["status"] = "failed"
                entry = {"path": str(path), "operation": purpose, "attempt": attempt,
                         "status": "permission_error", "error": str(error)}
                self.report["attempts"].append(entry)
                remaining = self.report["max_retry_wait_seconds"] - self.report["retry_wait_seconds"]
                if attempt == self.report["max_attempts"] or remaining <= 0:
                    self.save()
                    raise
                delay = min((.25, .5, 1.0)[attempt - 1], remaining)
                entry["retry_after_seconds"] = delay
                self.report["status"] = "retrying"
                self.save()
                time.sleep(delay)
                self.report["retry_wait_seconds"] += delay
            else:
                if attempt > 1:
                    self.report["status"] = "recovered"
                    self.report["attempts"].append({"path": str(path), "operation": purpose,
                                                    "attempt": attempt, "status": "succeeded"})
                    self.report["recovered_reads"].append({"path": str(path), "operation": purpose})
                return result


def _retain_observations(host, output, *, delete_pngs=False, before=None,
                         retry_permission_errors=False, retention_reads=None):
    """Retain once; retries apply solely to source reads, never archival/mutations."""
    origin = checked_path(host / "Saved/Observations")
    before = before or {"directories": [], "files": {}}
    reader = _RetentionReads(output, retention_reads if retention_reads is not None else {}, retry_permission_errors)
    try:
        for name, digest in before["files"].items():
            source = safe_relative(origin, name)
            if reader.read(lambda: sha256_file(source), source, "prior_inventory_hash") != digest:
                raise ValueError(f"Prior-run observation was modified during this run: {name}")
        if not origin.exists():
            return {"status": "no_artifacts", "verified_entries": 0, "deleted_pngs": []}
        # The dedicated run-owned observation subtree defines this run's inventory.
        paths = sorted(path.relative_to(origin).as_posix() for path in origin.rglob("*")
                       if path.is_file() and path.relative_to(origin).as_posix() not in before["files"])
        if any(name.split("/")[0] in before["directories"] for name in paths):
            raise ValueError("New observations were written into a previous run directory")
        if not paths:
            return {"status": "no_artifacts", "verified_entries": 0, "deleted_pngs": []}
        if len({name.casefold() for name in paths}) != len(paths):
            raise ValueError("Duplicate replay paths differ only by case")
        files = []
        for name in paths:
            source = safe_relative(origin, name)
            # Inventory exactly one file so retrying a locked read does not rerun
            # earlier reads or any copy, ZIP creation, verification or deletion.
            entry = reader.read(lambda: create_inventory(origin, [name]), source, "source_inventory")
            files.extend(entry["files"])
        destination = output / "observations"
        for entry in files:
            source = safe_relative(origin, entry["path"])
            target = safe_relative(destination, entry["path"], must_exist=False)
            target.parent.mkdir(parents=True, exist_ok=True)
            with reader.read(lambda: source.open("rb"), source, "copy_source_open") as input_stream:
                with target.open("wb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
            if sha256_file(target) != entry["sha256"]:
                raise ValueError(f"Observation changed during retention: {source}")
        inventory = create_inventory(destination, paths)
        write_json(output / "replay-inventory.json", inventory)
        retention = archive_replay(destination, inventory, output / "replay-evidence.zip", delete_pngs=delete_pngs)
        write_json(output / "replay-retention.json", retention)
        return retention
    finally:
        reader.save()


def _test(engine, host, output, rendered, expected, timeout, commands, result):
    process_error = None
    try:
        run_process(editor_arguments(engine, host, output, rendered=rendered, expected_tests=expected),
                    host, output, "host-console.log", timeout, commands)
    except ProcessFailure as error:
        process_error = error
        result["process_failure"] = error.record
    log_path = output / "host-editor.log"
    log = log_path.read_text(encoding="utf-8-sig", errors="replace") if log_path.exists() else ""
    result.update(check_test_results(log, expected, rendered=rendered))
    if process_error:
        result.update(status="failed", process_failure=process_error.record)
        raise process_error
    if result["status"] != "verified":
        raise RuntimeError(f"Host result is {result['status']}; inspect host-verification.json and retained editor log")


def _check_staged_sources(host, hashes):
    for name, digest in hashes.items():
        if sha256_file(safe_relative(host, name)) != digest:
            raise ValueError(f"Staged source changed during verification: {name}")


def verify(engine, output, *, rendered=False, expected_tests=None, build_timeout=900,
           test_timeout=300, delete_pngs=False):
    """Preserves the original verify(engine, output) NullRHI entry point."""
    expected = _expected_tests(rendered, expected_tests)
    engine, output = checked_path(engine), _new_output(output)
    plugin = Path(__file__).resolve().parent.parent
    scratch = checked_path(tempfile.mkdtemp(prefix="animation-capture-host-"))
    if scratch.parent != checked_path(tempfile.gettempdir()) or not scratch.name.startswith("animation-capture-host-"):
        raise ValueError("Temporary host scope could not be established")
    host, commands = scratch / "Host", []
    result = {"status": "running", "mode": "rendered" if rendered else "nullrhi", "expected_tests": list(expected),
              "scratch": str(scratch), "temporary_cleanup": "pending"}
    error = None
    preserve = False
    try:
        hashes = stage_sources(plugin, host)
        result.update(_identity(engine, host, output, hashes))
        _build(engine, host, output, build_timeout, commands)
        _test(engine, host, output, rendered, expected, test_timeout, commands, result)
        _check_staged_sources(host, hashes)
    except Exception as caught:
        error = caught
        if result["status"] not in ("skipped", "failed"):
            result["status"] = "failed"
        result["error"] = str(caught)
        preserve = isinstance(caught, ProcessFailure) and caught.record.get("tree_cleanup_complete") is False
    finally:
        try:
            result["retention"] = _retain_observations(host, output, delete_pngs=delete_pngs,
                retry_permission_errors=_confirmed_process_cleanup(commands),
                retention_reads=result.setdefault("retention_reads", {}))
            result["native_manifests"] = len(list((output / "observations").rglob("session.json")))
        except Exception as caught:
            preserve = True
            result.update(status="failed", retention_error=str(caught))
            error = error or caught
        if preserve:
            result["temporary_cleanup"] = "preserved_for_recovery"
        else:
            try:
                # Revalidate the absolute temporary root before recursive removal.
                if checked_path(scratch).parent != checked_path(tempfile.gettempdir()) or not scratch.name.startswith("animation-capture-host-"):
                    raise ValueError("Temporary cleanup scope changed")
                shutil.rmtree(scratch)
                result["temporary_cleanup"] = "removed"
            except Exception as caught:
                result.update(status="failed", temporary_cleanup="failed", cleanup_error=str(caught))
                error = error or caught
        write_json(output / "host-verification.json", result)
    if error:
        raise RuntimeError(f"{error}; evidence retained at {output}") from error
    print(json.dumps(result, indent=2))
    return result


def prepare(engine, output, host, *, plugin=None, stage_only=False, launch=False,
            run_tests=False, rendered=False, expected_tests=None, build_timeout=900,
            test_timeout=300, delete_pngs=False):
    if stage_only and (launch or run_tests) or launch and run_tests:
        raise ValueError("Stage-only, interactive launch and automated run are separate workflows")
    expected = _expected_tests(rendered, expected_tests)
    plugin = checked_path(plugin or Path(__file__).resolve().parent.parent)
    engine, host, output = checked_path(engine), checked_path(host), _new_output(output)
    if output.is_relative_to(host) or host.is_relative_to(output):
        raise ValueError("Evidence and retained host directories must be separate")
    result = {"status": "running", "build_status": "not_run", "runtime_status": "not_run", "host": str(host)}
    commands = []
    before = None
    failure = None
    try:
        hashes = prepare_host(plugin, host)
        result.update(_identity(engine, host, output, hashes))
        if run_tests:
            before = observation_snapshot(host)
            write_json(output / "host-observations-before.json", before)
        if not stage_only:
            result["build_status"] = "running"
            _build(engine, host, output, build_timeout, commands)
            result["build_status"] = "succeeded"
        _check_staged_sources(host, hashes)
        result["status"] = "prepared"
        if run_tests:
            result["runtime_status"] = "running"
            _test(engine, host, output, rendered, expected, test_timeout, commands, result)
            result["runtime_status"] = result["status"]
            _check_staged_sources(host, hashes)
        if launch:
            arguments = editor_arguments(engine, host, output, interactive=True)
            # The editor is intentionally visible and user-owned after launch.
            with (output / "host-console.log").open("w", encoding="utf-8") as stream:
                process = subprocess.Popen(arguments, cwd=host, stdout=stream, stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
            commands.append({"arguments": arguments, "pid": process.pid, "status": "launched_interactively",
                             "lifetime": "User closes the editor; no automation deadline applies"})
            write_json(output / "host-commands.json", commands)
            result.update(status="launched", editor_pid=process.pid, runtime_status="interactive_unverified")
    except Exception as error:
        failure = error
        if result["status"] != "skipped":
            result["status"] = "failed"
        if result["build_status"] == "running":
            result["build_status"] = "failed"
        if result["runtime_status"] == "running":
            result["runtime_status"] = result["status"]
        result["error"] = str(error)
        raise
    finally:
        if run_tests and before is not None:
            try:
                result["retention"] = _retain_observations(host, output, delete_pngs=delete_pngs, before=before,
                    retry_permission_errors=_confirmed_process_cleanup(commands),
                    retention_reads=result.setdefault("retention_reads", {}))
            except Exception as error:
                result.update(status="failed", retention_error=str(error))
                write_json(output / "host-verification.json", result)
                if failure is None:
                    raise
        write_json(output / "host-verification.json", result)
    print(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rendered", action="store_true", help="Run the exact lifecycle, renderer, async and interactive controls using D3D11")
    parser.add_argument("--test", action="append", help="Override the exact expected test list; repeat for each full path")
    parser.add_argument("--prepare", type=Path, help="Stage and build a retained host beneath this checkout's Saved directory")
    parser.add_argument("--stage-only", action="store_true", help="With --prepare, copy sources without building")
    parser.add_argument("--launch", action="store_true", help="With --prepare, build and open the interactive editor")
    parser.add_argument("--run-tests", action="store_true", help="With --prepare, build and run automation in the retained host")
    parser.add_argument("--build-timeout", type=float, default=900, help="Build deadline in seconds (default 900)")
    parser.add_argument("--test-timeout", type=float, default=300, help="Editor test deadline in seconds (default 300)")
    parser.add_argument("--delete-pngs", action="store_true", help="Delete only inventoried evidence PNGs after verified replay archival")
    args = parser.parse_args()
    if (args.stage_only or args.launch or args.run_tests) and not args.prepare:
        parser.error("--stage-only, --launch and --run-tests require --prepare")
    options = dict(rendered=args.rendered, expected_tests=args.test, build_timeout=args.build_timeout,
                   test_timeout=args.test_timeout, delete_pngs=args.delete_pngs)
    try:
        if args.prepare:
            prepare(args.engine, args.output, args.prepare, stage_only=args.stage_only,
                    launch=args.launch, run_tests=args.run_tests, **options)
        else:
            verify(args.engine, args.output, **options)
    except (RuntimeError, ValueError, OSError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
