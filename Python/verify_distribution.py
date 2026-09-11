"""Build and exercise an installed wheel outside its source checkout.

The only retained files are the wheel and verification logs under --output.
All build/install work is scoped to a validated temporary directory. Optional
build/image dependencies are installed there; the invoking environment is unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


def verify(output):
    source = Path(__file__).resolve().parent
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='animation-analysis-isolation-') as directory:
        scratch = Path(directory).resolve()
        # TemporaryDirectory recursively cleans its own tree on exit. Establish
        # its exact absolute scope before creating any build/install artifacts.
        assert scratch.parent == Path(tempfile.gettempdir()).resolve()
        assert scratch.name.startswith('animation-analysis-isolation-')
        staged = scratch / 'source'
        staged.mkdir()
        for name in ('pyproject.toml', 'README.md'):
            shutil.copyfile(source/name, staged/name)
        for subdirectory in ('src', 'tests'):
            for path in (source/subdirectory).rglob('*.py'):
                target = staged/path.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        commands = []
        def run(arguments, log):
            command = [str(value) for value in arguments]
            result = subprocess.run(command, cwd=scratch, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            (output/log).write_text(result.stdout, encoding='utf-8')
            commands.append(dict(arguments=command, returncode=result.returncode, log=log))
            (output/'commands.json').write_text(json.dumps(commands, indent=2))
            if result.returncode:
                raise RuntimeError(f'{log} failed with exit {result.returncode}: {result.stdout[-2000:]}')
            return result.stdout
        run([sys.executable, '-m', 'venv', scratch/'environment'], 'venv.log')
        binaries = scratch/'environment'/('Scripts' if sys.platform == 'win32' else 'bin')
        python = binaries/('python.exe' if sys.platform == 'win32' else 'python')
        run([python, '-I', '-m', 'pip', 'wheel', '--no-deps', '--wheel-dir', scratch/'wheels', staged], 'wheel.log')
        wheels = list((scratch/'wheels').glob('animation_analysis-*.whl'))
        if len(wheels) != 1:
            raise RuntimeError('Expected exactly one wheel')
        wheel = wheels[0]
        with zipfile.ZipFile(wheel) as saved:
            entries = saved.namelist()
            if any(not name.startswith(('animation_analysis/', 'animation_analysis-')) for name in entries):
                raise RuntimeError('Unexpected wheel member')
            if any('/profiles/' in name or name.endswith(('.uasset', '.uproject')) for name in entries):
                raise RuntimeError('Project data leaked into the distribution')
        run([python, '-I', '-m', 'pip', 'install', '--no-index', '--no-deps', wheel], 'install.log')
        isolation = run([python, '-I', '-c', '''import importlib.util,json,pathlib,sys
import animation_analysis
from animation_analysis.surfaces import measure_surface_relation
from animation_analysis.pixel_alignment import SegmentAlignmentSettings
assert importlib.util.find_spec("PIL") is None
assert importlib.util.find_spec("visual_analysis") is None
location=pathlib.Path(animation_analysis.__file__).resolve()
assert location.is_relative_to(pathlib.Path(sys.prefix).resolve())
result=measure_surface_relation(bytes([5,0,41]),[10]*3,[10]*3,3,1,5,41,.1)
assert result["minimum_visible_pixel_center_distance_px"]==2
SegmentAlignmentSettings().validate()
print(json.dumps(dict(package=str(location),isolated=sys.flags.isolated,optional_images_installed=False)))'''], 'isolation.log')
        run([python, '-I', '-m', 'unittest', 'discover', '-s', staged/'tests', '-v'], 'core-tests.log')
        for command in ('animation-review', 'animation-surface-review', 'animation-segment-calibrate', 'animation-segment-detect'):
            executable = binaries/(command+'.exe' if sys.platform == 'win32' else command)
            run([executable, '--help'], command+'-help.log')
        run([python, '-I', '-m', 'pip', 'install', str(wheel)+'[images]'], 'images-install.log')
        run([python, '-I', '-m', 'unittest', 'discover', '-s', staged/'tests', '-v'], 'images-tests.log')
        retained = output/wheel.name
        shutil.copyfile(wheel, retained)
        result = dict(status='verified', wheel=retained.name, wheel_sha256=hashlib.sha256(retained.read_bytes()).hexdigest(),
                      wheel_members=entries, isolation=json.loads(isolation), command_count=len(commands),
                      scratch=str(scratch), temporary_cleanup='pending')
    result['temporary_cleanup'] = 'removed' if not scratch.exists() else 'failed'
    assert result['temporary_cleanup'] == 'removed'
    (output/'distribution-verification.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k:v for k,v in result.items() if k != 'wheel_members'}, indent=2))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    verify(parser.parse_args().output)
