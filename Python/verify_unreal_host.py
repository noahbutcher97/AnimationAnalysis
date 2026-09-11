"""Build and test the capture plugin in a temporary host outside the project.

Requires a supplied UE 5.6 installation. Only plugin source and the neutral host
are copied. Retains logs and identities; deletes its own validated temporary tree.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def verify(engine, output):
    engine, output = Path(engine).resolve(), Path(output).resolve()
    source = Path(__file__).resolve().parent
    plugin = source.parent
    output.mkdir(parents=True, exist_ok=True)
    hashes = {}
    with tempfile.TemporaryDirectory(prefix="animation-capture-host-") as directory:
        scratch = Path(directory).resolve()
        assert scratch.parent == Path(tempfile.gettempdir()).resolve()
        assert scratch.name.startswith("animation-capture-host-")
        host = scratch / "Host"
        for origin, destination in ((source / "UnrealHost", host), (plugin, host / "Plugins/AnimationAnalysis")):
            for path in origin.rglob("*"):
                if not path.is_file() or path.suffix not in (".cpp", ".h", ".cs", ".uproject", ".uplugin"):
                    continue
                relative = path.relative_to(origin)
                if origin == plugin and relative.parts[0] != "Source" and path.suffix != ".uplugin":
                    continue
                if any(part in ("Intermediate", "Binaries", "Saved") for part in relative.parts):
                    continue
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
                hashes[target.relative_to(host).as_posix()] = hashlib.sha256(target.read_bytes()).hexdigest()
        (output / "host-source.json").write_text(json.dumps(hashes, indent=2))
        commands = []
        def run(arguments, log, timeout):
            command = [str(value) for value in arguments]
            creation = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            with (output / log).open("w", encoding="utf-8") as stream:
                process = subprocess.Popen(command, cwd=host, stdout=stream, stderr=subprocess.STDOUT, creationflags=creation)
                try:
                    result = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise RuntimeError(f"{log} timed out; host verification incomplete")
            commands.append(dict(arguments=command, exit_code=result, log=log))
            (output / "host-commands.json").write_text(json.dumps(commands, indent=2))
            if result:
                raise RuntimeError(f"{log} failed with exit {result}; inspect retained log")
        project = host / "AnimationCaptureHost.uproject"
        run([engine / "Engine/Build/BatchFiles/Build.bat", "AnimationCaptureHostEditor", "Win64", "Development",
             f"-Project={project}", "-NoHotReload", "-WaitMutex"], "host-build.log", 900)
        run([engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe", project,
             "-ExecCmds=Automation RunTests AnimationAnalysis.Capture.Portability;Quit", "-unattended", "-nopause",
             "-NullRHI", "-nosplash", "-stdout", f"-abslog={output / 'host-editor.log'}"], "host-console.log", 180)
        log = (output / "host-editor.log").read_text(encoding="utf-8-sig")
        results = re.findall(r"Test Completed\. Result=\{(\w+)\}.*?Path=\{([^}]+)\}", log)
        expected = {"AnimationAnalysis.Capture.Portability.IndependentSessions", "AnimationAnalysis.Capture.Portability.ExtensionIntegrity"}
        if len(results) != len(expected) or {name for _, name in results} != expected or any(status != "Success" for status, _ in results):
            raise RuntimeError(f"Expected complete successful host results; got {results}")
        manifest_paths = list((host / "Saved/Observations").glob("*/session.json"))
        # Retain small native JSONL/manifests, never engine caches or compiled files.
        for path in (host / "Saved/Observations").rglob("*"):
            if path.is_file() and path.suffix in (".json", ".jsonl", ".txt"):
                target = output / "observations" / path.relative_to(host / "Saved/Observations")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        result = dict(status="verified", host_tests=results, host_source_files=len(hashes),
                      native_manifests=len(manifest_paths), scratch=str(scratch),
                      dependencies="Engine and copied AnimationCapture plugin only")
    assert not scratch.exists()
    result["temporary_cleanup"] = "removed"
    (output / "host-verification.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    verify(args.engine, args.output)
