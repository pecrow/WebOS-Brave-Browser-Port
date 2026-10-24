import pathlib
import shlex
import subprocess
import sys

project = pathlib.Path(__file__).resolve().parents[1]
workspace = pathlib.Path.home() / "lg-tv-brave-v1.97.59"
source = workspace / "src"
output = source / "out/WebOSB9"
destination = source / "ui/ozone/platform/webos"
subprocess.run(
    [sys.executable, str(project / "scripts/build-port.py"), "--export-only"], check=True
)

variables = {}
for line in (output / "obj/ui/ozone/platform/webos/webos.ninja").read_text().splitlines():
    if line.startswith("build "):
        break
    if " = " in line:
        key, value = line.split(" = ", 1)
        variables[key] = value.replace("$:", ":").replace("$$", "$")

flags = []
for key in ["defines", "include_dirs", "cflags"]:
    flags.extend(shlex.split(variables[key]))
compiler_directory = source / "third_party/llvm-build/Release+Asserts/bin"
for file in sorted(destination.iterdir()):
    if file.suffix not in {".c", ".cc"}:
        continue
    compiler = compiler_directory / ("clang" if file.suffix == ".c" else "clang++")
    language_flags = shlex.split(variables["cflags_c" if file.suffix == ".c" else "cflags_cc"])
    if file.suffix == ".cc":
        language_flags.extend(shlex.split(variables["module_deps"]))
    print("Checking " + file.name, flush=True)
    # Chromium's path-based safety checks require an in-tree source path.
    compiler_input = pathlib.Path("../..") / file.relative_to(source)
    result = subprocess.run(
        [str(compiler), *flags, *language_flags, "-fsyntax-only", str(compiler_input)],
        cwd=output,
    )
    if result.returncode:
        sys.exit(result.returncode)
print("All native Ozone translation units passed the pinned compiler checks.")
