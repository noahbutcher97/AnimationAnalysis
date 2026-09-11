"""Source staging, bounded host processes and inventory-based replay retention.

Repository tooling only: this module is not part of the portable Python wheel.
All project, engine, output and replay identities are supplied explicitly.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import stat
import subprocess
import time
import zipfile


LIFECYCLE_TESTS = (
    "AnimationAnalysis.Capture.Portability.IndependentSessions",
    "AnimationAnalysis.Capture.Portability.ExtensionIntegrity",
)
SURFACE_TESTS = (
    "AnimationAnalysis.Capture.Surfaces.LabelOwnership",
    "AnimationAnalysis.Capture.Surfaces.RenderedGeometry",
)
HOST_MARKER = ".animation-analysis-host.json"
ARCHIVE_INVENTORY = "_replay_inventory.json"
GENERATED_DIRECTORIES = {"intermediate", "binaries", "saved", "deriveddatacache", ".git"}


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_path(path):
    """Reject links and Windows reparse points before resolving a local path."""
    absolute = Path(os.path.abspath(path))
    for item in (absolute, *absolute.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Links/reparse points are not permitted: {item}")
    return absolute.resolve()


def safe_relative(root, name, *, must_exist=True):
    """Resolve one canonical portable inventory path within its declared root."""
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        raise ValueError(f"Invalid relative inventory path: {name!r}")
    parts = name.split("/")
    if any(part in ("", ".", "..") or part.rstrip(" .") != part for part in parts):
        raise ValueError(f"Non-canonical relative inventory path: {name!r}")
    if PurePosixPath(name).is_absolute():
        raise ValueError(f"Absolute inventory path: {name!r}")
    root = checked_path(root)
    target = checked_path(root.joinpath(*parts))
    if not target.is_relative_to(root) or target == root:
        raise ValueError(f"Inventory entry escapes root: {name!r}")
    if must_exist and not target.is_file():
        raise ValueError(f"Inventory file is missing: {target}")
    return target


def _source_files(plugin):
    plugin = checked_path(plugin)
    host_source = plugin / "Python/UnrealHost"
    pairs = [(plugin / "AnimationAnalysis.uplugin", "Plugins/AnimationAnalysis/AnimationAnalysis.uplugin"),
             (host_source / "AnimationCaptureHost.uproject", "AnimationCaptureHost.uproject")]
    for origin, prefix in ((plugin / "Source", "Plugins/AnimationAnalysis/Source"),
                           (host_source / "Source", "Source")):
        if not origin.is_dir():
            raise ValueError(f"Required source directory is missing: {origin}")
        for path in sorted(origin.rglob("*")):
            relative = path.relative_to(origin)
            if any(part.lower() in GENERATED_DIRECTORIES for part in relative.parts):
                continue
            if path.is_file() and path.suffix.lower() in {".cpp", ".h", ".hpp", ".inl", ".cs"}:
                pairs.append((path, f"{prefix}/{relative.as_posix()}"))
    shaders = plugin / "Shaders"
    if shaders.is_dir():
        for path in sorted(shaders.rglob("*")):
            relative = path.relative_to(shaders)
            if any(part.lower() in GENERATED_DIRECTORIES for part in relative.parts):
                continue
            if path.is_file() and path.suffix.lower() in {".usf", ".ush"}:
                pairs.append((path, f"Plugins/AnimationAnalysis/Shaders/{relative.as_posix()}"))
    for origin, _ in pairs:
        resolved = checked_path(origin)
        if not resolved.is_file() or not resolved.is_relative_to(plugin):
            raise ValueError(f"Required source file is missing or outside plugin: {origin}")
    return pairs


def stage_sources(plugin, host):
    """Copy the source allowlist used by both isolated and retained hosts."""
    host = checked_path(host)
    pairs = _source_files(plugin)
    hashes = {}
    for origin, relative in pairs:
        target = safe_relative(host, relative, must_exist=False)
        digest = sha256_file(origin)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
        if sha256_file(target) != digest:
            raise ValueError(f"Source changed while staging: {origin}")
        hashes[relative] = digest
    checked_path(host / "Content").mkdir(exist_ok=True)
    return dict(sorted(hashes.items()))


def prepare_host(plugin, host):
    """Refresh an owned host below this checkout's ignored Saved directory."""
    plugin, host = checked_path(plugin), checked_path(host)
    saved = plugin / "Saved"
    if host == saved or not host.is_relative_to(saved):
        raise ValueError("A retained host must be a directory beneath this plugin's Saved directory")
    marker = host / HOST_MARKER
    previous = {}
    if host.exists() and any(host.iterdir()):
        if not marker.is_file():
            raise ValueError(f"Refusing to overwrite an unmanaged host: {host}")
        state = json.loads(checked_path(marker).read_text(encoding="utf-8"))
        if state.get("schema_version") != 1 or state.get("source_root") != str(plugin):
            raise ValueError("Prepared host ownership does not match this checkout")
        previous = state.get("source_hashes", {})
        if not isinstance(previous, dict) or not previous:
            raise ValueError("Prepared host source inventory is invalid")
        for name, digest in previous.items():
            target = safe_relative(host, name)
            if sha256_file(target) != digest:
                raise ValueError(f"Prepared host source was modified; preserve the edit before refreshing: {target}")
    next_names = {relative for _, relative in _source_files(plugin)}
    for name in next_names - previous.keys():
        if safe_relative(host, name, must_exist=False).exists():
            raise ValueError(f"New source would overwrite an unowned host file: {name}")
    hashes = stage_sources(plugin, host)
    for name in previous.keys() - hashes.keys():
        # Every removal comes from the validated prior source inventory, never a glob.
        safe_relative(host, name).unlink()
    write_json(marker, {"schema_version": 1, "source_root": str(plugin), "source_hashes": hashes})
    return hashes


def check_test_results(log, expected, *, rendered=False):
    expected = tuple(expected)
    if not expected or len(set(expected)) != len(expected):
        raise ValueError("Expected tests must be a nonempty exact list without duplicates")
    results = re.findall(r"Test Completed\. Result=\{([^}]+)\}[^\r\n]*?Path=\{([^}]+)\}", log)
    seen = Counter(name for _, name in results)
    missing = sorted(set(expected) - seen.keys())
    unexpected = sorted(seen.keys() - set(expected))
    duplicates = sorted(name for name, count in seen.items() if count != 1)
    unsuccessful = [{"name": name, "result": status} for status, name in results if status != "Success"]
    skip_lines = [line.strip() for line in log.splitlines() if "ANIMATION_ANALYSIS_RENDERED_SKIP" in line]
    d3d11 = bool(re.search(r"LogRHI:[^\r\n]*(?:Using (?:Default )?RHI|RHI Selected)[^\r\n]*D3D11|LogD3D11RHI:[^\r\n]*(?:Creating new Direct3DDevice|Chosen D3D11 Adapter|D3D11 Adapter)", log))
    reasons = []
    if missing or unexpected or duplicates or unsuccessful:
        reasons.append("Automation did not produce exactly one successful result per expected test")
    if rendered and not d3d11:
        reasons.append("No active D3D11 renderer identity found in the log")
    status = ("skipped" if rendered and skip_lines and not unsuccessful
              else ("failed" if reasons else "verified"))
    return {"status": status, "expected_tests": list(expected), "host_tests": results,
            "missing_tests": missing, "unexpected_tests": unexpected, "duplicate_tests": duplicates,
            "unsuccessful_tests": unsuccessful, "rendered_requested": rendered,
            "d3d11_observed": d3d11, "rendered_skips": skip_lines, "reasons": reasons}


class ProcessFailure(RuntimeError):
    def __init__(self, record):
        self.record = record
        super().__init__(f"{record['log']} {record['status']}: {record.get('error', record.get('exit_code'))}")


def _terminate_tree(process):
    if os.name == "nt":
        try:
            cleanup = subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW)
            complete = cleanup.returncode == 0
            detail = cleanup.stdout.strip()
        except (OSError, subprocess.TimeoutExpired) as error:
            complete, detail = False, str(error)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
            complete, detail = True, "Terminated the process group"
        except ProcessLookupError:
            complete, detail = True, "Process group already exited"
    if process.poll() is None:
        process.kill()
    process.wait(timeout=30)
    return complete, detail


def run_process(arguments, cwd, output, log, timeout, records):
    """Wait to a deadline, retaining failures and terminating the timed-out tree."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Process deadline must be a positive finite number of seconds")
    if Path(log).name != log:
        raise ValueError("Process log must be a filename inside its output directory")
    output = checked_path(output)
    output.mkdir(parents=True, exist_ok=True)
    record = {"arguments": [str(value) for value in arguments], "cwd": str(checked_path(cwd)),
              "log": log, "deadline_seconds": timeout, "started_utc": datetime.now(timezone.utc).isoformat(),
              "status": "running", "exit_code": None}
    records.append(record)
    write_json(output / "host-commands.json", records)
    start = time.monotonic()
    process = None
    try:
        options = {"creationflags": subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
        with (output / log).open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(record["arguments"], cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, **options)
            record["pid"] = process.pid
            write_json(output / "host-commands.json", records)
            try:
                record["exit_code"] = process.wait(timeout=timeout)
                record["status"] = "succeeded" if record["exit_code"] == 0 else "failed"
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                record["status"] = "timed_out" if isinstance(error, subprocess.TimeoutExpired) else "interrupted"
                record["error"] = str(error) or "Interrupted by operator"
                record["tree_cleanup_complete"] = False
                try:
                    record["tree_cleanup_complete"], record["tree_cleanup_detail"] = _terminate_tree(process)
                except Exception as cleanup_error:
                    # The caller must preserve its host whenever descendant cleanup
                    # cannot be confirmed, including a failed cleanup operation.
                    record["tree_cleanup_detail"] = str(cleanup_error)
                record["exit_code"] = process.returncode
    except OSError as error:
        record.update(status="failed", error=str(error))
    finally:
        record["elapsed_seconds"] = round(time.monotonic() - start, 6)
        write_json(output / "host-commands.json", records)
    if record["status"] != "succeeded":
        raise ProcessFailure(record)
    return record


def create_inventory(run_root, paths):
    run_root = checked_path(run_root)
    if not run_root.is_dir():
        raise ValueError(f"Replay root does not exist: {run_root}")
    files, seen = [], set()
    for name in sorted(paths):
        target = safe_relative(run_root, name)
        if name.casefold() == ARCHIVE_INVENTORY.casefold() or name.casefold() in seen:
            raise ValueError(f"Duplicate or reserved replay entry: {name}")
        seen.add(name.casefold())
        files.append({"path": name, "size_bytes": target.stat().st_size, "sha256": sha256_file(target)})
    if not files:
        raise ValueError("Replay inventory must explicitly select at least one file")
    return {"schema_version": 1, "run_root": str(run_root), "files": files}


def _validate_inventory(inventory, run_root=None, *, check_files=False):
    if not isinstance(inventory, dict) or inventory.get("schema_version") != 1:
        raise ValueError("Unsupported replay inventory")
    if not isinstance(inventory.get("run_root"), str) or not Path(inventory["run_root"]).is_absolute():
        raise ValueError("Replay inventory requires an absolute run root")
    root = checked_path(inventory["run_root"])
    if run_root is not None and checked_path(run_root) != root:
        raise ValueError("Replay inventory belongs to a different run root")
    files = inventory.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("Replay inventory requires explicit files")
    seen = set()
    for entry in files:
        if not isinstance(entry, dict):
            raise ValueError("Malformed replay inventory entry")
        name, size, digest = entry.get("path"), entry.get("size_bytes"), entry.get("sha256")
        target = safe_relative(root, name, must_exist=check_files)
        if name.casefold() in seen or name.casefold() == ARCHIVE_INVENTORY.casefold():
            raise ValueError(f"Duplicate or reserved replay entry: {name}")
        if type(size) is not int or size < 0 or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"Invalid replay size/hash: {name}")
        seen.add(name.casefold())
        if check_files and (target.stat().st_size != size or sha256_file(target) != digest):
            raise ValueError(f"Replay file no longer matches inventory: {name}")
    return root, files


def verify_archive(archive, inventory):
    """Read and SHA-256 every entry; archive size or a central CRC is insufficient."""
    _, files = _validate_inventory(inventory)
    archive = checked_path(archive)
    with zipfile.ZipFile(archive) as saved:
        names = saved.namelist()
        expected = {entry["path"] for entry in files} | {ARCHIVE_INVENTORY}
        if len(names) != len(expected) or set(names) != expected:
            raise ValueError("Replay archive entries differ from the explicit inventory")
        if json.loads(saved.read(ARCHIVE_INVENTORY)) != inventory:
            raise ValueError("Replay archive inventory differs from the supplied inventory")
        for entry in files:
            info = saved.getinfo(entry["path"])
            digest, size = hashlib.sha256(), 0
            with saved.open(info) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
                    size += len(chunk)
            if size != entry["size_bytes"] or digest.hexdigest() != entry["sha256"]:
                raise ValueError(f"Replay archive entry failed SHA-256 verification: {entry['path']}")
    return {"status": "verified", "archive": str(archive), "archive_sha256": sha256_file(archive),
            "verified_entries": len(files)}


def archive_replay(run_root, inventory, archive, *, delete_pngs=False):
    root, files = _validate_inventory(inventory, run_root, check_files=True)
    archive = checked_path(archive)
    if archive.is_relative_to(root):
        raise ValueError("Replay archive must be outside the replay root")
    if archive.exists():
        raise ValueError(f"Refusing to replace a replay archive: {archive}")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as saved:
        saved.writestr(ARCHIVE_INVENTORY, json.dumps(inventory, indent=2) + "\n")
        for entry in files:
            saved.write(safe_relative(root, entry["path"]), entry["path"])
    result = verify_archive(archive, inventory)
    result["deleted_pngs"] = []
    if delete_pngs:
        # Recheck the complete source inventory before the first deletion. Then
        # resolve and hash each selected PNG again immediately before unlinking.
        _validate_inventory(inventory, run_root, check_files=True)
        for entry in files:
            if PurePosixPath(entry["path"]).suffix.lower() != ".png":
                continue
            target = safe_relative(root, entry["path"])
            if target.stat().st_size != entry["size_bytes"] or sha256_file(target) != entry["sha256"]:
                raise ValueError(f"PNG changed after archive verification: {target}")
            target.unlink()
            result["deleted_pngs"].append(entry["path"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inventory = commands.add_parser("inventory", help="Inventory explicit paths beneath a completed run")
    inventory.add_argument("--run-root", type=Path, required=True)
    inventory.add_argument("--file", action="append", required=True, help="One relative replay path; repeat for every selected file")
    inventory.add_argument("--output", type=Path, required=True)
    archive = commands.add_parser("archive", help="Archive and verify an inventory, optionally deleting its loose PNGs")
    archive.add_argument("--run-root", type=Path, required=True)
    archive.add_argument("--inventory", type=Path, required=True)
    archive.add_argument("--archive", type=Path, required=True)
    archive.add_argument("--delete-pngs", action="store_true")
    check = commands.add_parser("verify-archive", help="Verify every archived entry without requiring loose files")
    check.add_argument("--inventory", type=Path, required=True)
    check.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "inventory":
        result = create_inventory(args.run_root, args.file)
        write_json(args.output, result)
    else:
        manifest = json.loads(args.inventory.read_text(encoding="utf-8"))
        result = (archive_replay(args.run_root, manifest, args.archive, delete_pngs=args.delete_pngs)
                  if args.command == "archive" else verify_archive(args.archive, manifest))
        if args.command == "archive":
            write_json(args.archive.with_suffix(".retention.json"), result)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
