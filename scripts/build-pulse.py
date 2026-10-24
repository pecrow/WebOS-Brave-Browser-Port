import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys


def run(*arguments, **kwargs):
    return subprocess.run(arguments, check=True, **kwargs)


def checkout(destination, repository, tag, revision):
    if not destination.exists():
        run("git", "clone", "--quiet", "--depth", "1", "--branch", tag,
            f"https://github.com/{repository}.git", str(destination))
    actual = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=destination, text=True
    ).strip()
    if actual != revision:
        raise RuntimeError(f"Unexpected source revision in {destination}")


project = pathlib.Path(__file__).resolve().parents[1]
workspace = pathlib.Path.home() / "lg-tv-brave-v1.97.59"
root = workspace / "pulse-webos"
root.mkdir(exist_ok=True)
source = root / "source"
build = root / "build"
meson = workspace / "tools/meson-1.4.2"
pulse_revision = "e5ad31e873eed62bc580a86a61177047f9e8c491"
checkout(source, "pulseaudio/pulseaudio", "v16.1", pulse_revision)
checkout(meson, "mesonbuild/meson", "1.4.2",
         "8280db82f22fa236c172458ca22771b28f2cbe7c")
patch_file = project / "patches/pulse-webos-playback.patch"
patch = patch_file.read_text(encoding="utf-8")
applied = subprocess.run(
    ["git", "apply", "--reverse", "--check", "-"],
    cwd=source, input=patch, text=True, capture_output=True
)
if applied.returncode != 0:
    run("git", "apply", "--check", "-", cwd=source, input=patch, text=True)
    run("git", "apply", "-", cwd=source, input=patch, text=True)

runtime = workspace / "arm64-runtime"
libraries = runtime / "rootfs/usr/lib/aarch64-linux-gnu"
if not (libraries / "libsndfile.so.1").is_file():
    raise RuntimeError("Prepare the existing ARM64 runtime before building audio")
dependencies = root / "dependencies"
dependencies.mkdir(exist_ok=True)
packages = list(dependencies.glob("libsndfile1-dev_*_arm64.deb"))
if not packages:
    apt = runtime / "apt"
    run(
        "apt-get",
        "-o", f"Dir::Etc::sourcelist={apt / 'sources.list'}",
        "-o", "Dir::Etc::sourceparts=-",
        "-o", f"Dir::State::lists={apt / 'lists'}",
        "-o", "APT::Architecture=arm64",
        "-o", "APT::Architectures::=arm64",
        "-o", "Debug::NoLocking=1",
        "download", "libsndfile1-dev:arm64",
        cwd=dependencies,
    )
    packages = list(dependencies.glob("libsndfile1-dev_*_arm64.deb"))
if len(packages) != 1:
    raise RuntimeError("Expected exactly one ARM64 libsndfile development package")
headers = dependencies / "root"
run("dpkg-deb", "--extract", str(packages[0]), str(headers))
if not (headers / "usr/include/sndfile.h").is_file():
    raise RuntimeError("The audio dependency package has no sndfile header")
pkgconfig = root / "pkgconfig"
pkgconfig.mkdir(exist_ok=True)
(pkgconfig / "sndfile.pc").write_text(
    "Name: sndfile\nDescription: Private ARM64 libsndfile\nVersion: 1.2.2\n"
    f"Libs: -L{libraries} -l:libsndfile.so.1\n"
    f"Cflags: -I{headers / 'usr/include'}\n",
    encoding="utf-8",
)
cross = root / "aarch64.ini"
cross.write_text(
    "[binaries]\n"
    "c = 'aarch64-linux-gnu-gcc'\n"
    "cpp = 'aarch64-linux-gnu-g++'\n"
    "ar = 'aarch64-linux-gnu-ar'\n"
    "strip = 'aarch64-linux-gnu-strip'\n"
    "pkg-config = 'pkg-config'\n"
    "[host_machine]\n"
    "system = 'linux'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\n"
    "endian = 'little'\n"
    "[properties]\nneeds_exe_wrapper = true\n"
    "[built-in options]\n"
    f"c_link_args = ['-L{libraries}', '-Wl,-rpath-link,{libraries}']\n"
    f"cpp_link_args = ['-L{libraries}', '-Wl,-rpath-link,{libraries}']\n",
    encoding="utf-8",
)
environment = dict(
    os.environ,
    PKG_CONFIG_LIBDIR=str(pkgconfig),
    PKG_CONFIG_PATH="",
    PKG_CONFIG_SYSROOT_DIR="",
    PATH=f"{workspace / 'tools/m4/usr/bin'}{os.pathsep}{os.environ['PATH']}",
)
command = [
    sys.executable, str(meson / "meson.py"), "setup",
    str(build), str(source), "--cross-file", str(cross),
    "--buildtype=release", "--prefix=/usr", "--sysconfdir=/etc",
    "--auto-features=disabled", "-Ddaemon=false", "-Dclient=true",
    "-Ddoxygen=false", "-Dman=false", "-Dtests=false",
]
if (build / "meson-private/coredata.dat").exists():
    command.append("--reconfigure")
run(*command, env=environment)
run("ninja", "-C", str(build), "-j", "4", "src/pulse/libpulse.so.0.24.2",
    env=environment)
outputs = {
    "libpulse.so.0": build / "src/pulse/libpulse.so.0.24.2",
    "libpulsecommon-16.1.so": build / "src/libpulsecommon-16.1.so",
}
record = {
    "revision": pulse_revision,
    "patch_sha256": hashlib.sha256(patch_file.read_bytes()).hexdigest(),
    "libraries": {},
}
output_directory = root / "lib"
output_directory.mkdir(exist_ok=True)
for name, file in outputs.items():
    if not file.is_file():
        raise RuntimeError(f"Missing audio build output: {file}")
    target = output_directory / name
    shutil.copyfile(file, target)
    run("aarch64-linux-gnu-strip", "--strip-unneeded", str(target))
    run("patchelf", "--force-rpath", "--set-rpath", "$ORIGIN", str(target))
    record["libraries"][name] = {
        "path": str(target.relative_to(root)),
        "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
    }
(root / "build-result.json").write_text(json.dumps(record, indent=2) + "\n",
                                       encoding="utf-8")
print("Built private webOS-compatible PulseAudio client:", root)
