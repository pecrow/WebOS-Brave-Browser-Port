import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import tarfile


def run(*arguments):
    return subprocess.check_output(arguments, text=True).strip()


def is_arm64_elf(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    return (
        len(header) == 20
        and header[:6] == b"\x7fELF\x02\x01"
        and struct.unpack("<H", header[18:20])[0] == 183
    )


def needed(path):
    return run("patchelf", "--print-needed", str(path)).splitlines()


parser = argparse.ArgumentParser()
parser.add_argument(
    "--reference-smoke",
    action="store_true",
    help="Prepare the official reference engine for CLI diagnostics, not an installable port",
)
parser.add_argument("--output-dir", type=pathlib.Path,
                    help="New release output directory; existing stages are never overwritten")
options = parser.parse_args()
project = pathlib.Path(__file__).resolve().parents[1]
config = json.loads((project / "build-config.json").read_text())
app_info = json.loads((project / "app/appinfo.json").read_text())
workspace = pathlib.Path.home() / f"lg-tv-brave-v{config['braveVersion']}"
source = workspace / ("baseline" if options.reference_smoke else "src/out/WebOSB9")
runtime = workspace / "arm64-runtime/rootfs"
app_id = config["appId"]
name = "reference-runtime" if options.reference_smoke else app_id
output_directory = options.output_dir.resolve() if options.output_dir else project / "out"
stage = output_directory / name
tv_prefix = (
    "/media/developer/brave-b9-smoke/patched"
    if options.reference_smoke
    else f"/media/developer/apps/usr/palm/applications/{app_id}"
)
interpreter = f"{tv_prefix}/runtime/lib/ld-linux-aarch64.so.1"
required = [
    "brave",
    "chrome_crashpad_handler",
    "resources.pak",
    "icudtl.dat",
    "chrome_100_percent.pak",
    "chrome_200_percent.pak",
    "locales/en-US.pak",
]
if not options.reference_smoke:
    required += ["brave_resources.pak", "brave_100_percent.pak", "brave_200_percent.pak"]
for relative in required:
    if not (source / relative).is_file():
        raise RuntimeError(f"Required browser build artifact is missing: {source / relative}")
if not any((source / file).is_file() for file in ["v8_context_snapshot.bin", "snapshot_blob.bin"]):
    raise RuntimeError("The browser's external V8 startup data is missing")
for executable in ["brave", "chrome_crashpad_handler"]:
    if not is_arm64_elf(source / executable):
        raise RuntimeError(f"The browser executable is not ARM64: {source / executable}")
if stage.exists():
    raise RuntimeError(f"Refusing to overwrite an existing package stage: {stage}")
if not shutil.which("patchelf"):
    raise RuntimeError("patchelf is required to package the private ARM64 runtime")
stage.mkdir(parents=True)
engine = stage / "engine"
engine.mkdir()
library_dir = stage / "runtime/lib"
library_dir.mkdir(parents=True)

files = set(required)
for pattern in ["*.pak", "*.dat", "*.bin", "*.so*", "vk_swiftshader_icd.json", "version", "LICENSE"]:
    for file in source.glob(pattern):
        if not file.is_file():
            continue
        # The launcher explicitly selects Chromium's fallback theme, not desktop Qt.
        if file.name.startswith(("libqt5_shim.so", "libqt6_shim.so")):
            continue
        if pattern == "*.so*" and not is_arm64_elf(file):
            continue
        files.add(file.name)
for optional in ["chrome-management-service", "chrome_management_service"]:
    if (source / optional).is_file():
        files.add(optional)
for relative in sorted(files):
    target = engine / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / relative, target)
for directory in [
    "locales", "resources", "default_apps", "MEIPreload",
    "PrivacySandboxAttestationsPreloaded", "hyphen-data", "lib",
]:
    if (source / directory).is_dir():
        shutil.copytree(source / directory, engine / directory, dirs_exist_ok=True)
if not options.reference_smoke:
    shutil.copyfile(workspace / "src/brave/LICENSE", engine / "LICENSE")

index = {}
for file in (runtime / "usr/lib/aarch64-linux-gnu").rglob("*"):
    if file.is_file():
        index.setdefault(file.name, set()).add(file.resolve())

audio = workspace / "pulse-webos"
if not options.reference_smoke:
    audio_record = json.loads((audio / "build-result.json").read_text())
    patch_hash = hashlib.sha256(
        (project / "patches/pulse-webos-playback.patch").read_bytes()
    ).hexdigest()
    if audio_record["patch_sha256"] != patch_hash:
        raise RuntimeError("Rebuild the private audio client after changing its patch")
    for name in ("libpulse.so.0", "libpulsecommon-16.1.so"):
        metadata = audio_record["libraries"][name]
        original = (audio / metadata["path"]).resolve()
        if not original.is_relative_to(audio.resolve()) or not is_arm64_elf(original):
            raise RuntimeError(f"Invalid private audio build output: {original}")
        if hashlib.sha256(original.read_bytes()).hexdigest() != metadata["sha256"]:
            raise RuntimeError(f"The private audio build output changed: {original}")
        index[name] = {original}

engine_elfs = []
bundled = {}
for file in engine.rglob("*"):
    if file.is_file() and is_arm64_elf(file):
        engine_elfs.append(file)
        if ".so" in file.name:
            bundled[file.name] = file

queue = [dependency for file in engine_elfs for dependency in needed(file)]
# NSS and PulseAudio load these modules at runtime, outside DT_NEEDED.
queue += [
    "ld-linux-aarch64.so.1", "libnssckbi.so", "libsoftokn3.so",
    "libfreebl3.so", "libfreeblpriv3.so", "libpulse.so.0",
]
selected = {}
while queue:
    name = queue.pop()
    if name in selected or name in bundled:
        continue
    candidates = index.get(name, set())
    if len(candidates) != 1:
        raise RuntimeError(f"Missing or ambiguous private dependency {name}: {sorted(candidates)}")
    original = next(iter(candidates))
    if not (original.is_relative_to(runtime.resolve()) or
            (not options.reference_smoke and original.is_relative_to(audio.resolve()))) or not is_arm64_elf(original):
        raise RuntimeError(f"Invalid private ARM64 library: {original}")
    target = library_dir / name
    shutil.copyfile(original, target)
    selected[name] = target
    queue.extend(needed(target))

for file in [*engine_elfs, *selected.values()]:
    if file.name == "ld-linux-aarch64.so.1":
        file.chmod(0o755)
        continue
    headers = run("readelf", "-l", str(file))
    if not options.reference_smoke and file in engine_elfs:
        subprocess.run(["aarch64-linux-gnu-strip", "--strip-unneeded", str(file)],
                       check=True)
    if "INTERP" in headers:
        subprocess.run(["patchelf", "--set-interpreter", interpreter, str(file)], check=True)
    relative = pathlib.Path(os.path.relpath(library_dir, file.parent)).as_posix()
    rpath = "$ORIGIN" if file.parent == library_dir else f"$ORIGIN:$ORIGIN/lib:$ORIGIN/{relative}"
    subprocess.run(
        ["patchelf", "--force-rpath", "--set-rpath", rpath, str(file)], check=True
    )
    file.chmod(0o755)
    for dependency in needed(file):
        if dependency not in selected and dependency not in bundled:
            raise RuntimeError(f"Unresolved packaged dependency: {file.name}: {dependency}")

fonts = runtime / "usr/share/fonts/truetype/dejavu"
if not fonts.is_dir():
    raise RuntimeError(f"Required runtime fonts are missing: {fonts}")
shutil.copytree(fonts, stage / "runtime/fonts")
(stage / "runtime/fonts.conf").write_text(
    '<?xml version="1.0"?>\n'
    "<!DOCTYPE fontconfig SYSTEM 'urn:fontconfig:fonts.dtd'>\n"
    "<fontconfig>\n"
    '  <dir prefix="relative">fonts</dir>\n'
    '  <cachedir prefix="xdg">fontconfig</cachedir>\n'
    "</fontconfig>\n",
    encoding="utf-8",
    newline="\n",
)
def ignore_missing_documentation(directory, names):
    skipped = []
    for name in names:
        file = pathlib.Path(directory) / name
        if file.is_symlink() and not file.exists():
            if any(word in name.lower() for word in ("copyright", "license", "copying")):
                raise RuntimeError(f"Required upstream license is missing: {name}")
            print(f"Skipping dangling upstream documentation link: {name}", flush=True)
            skipped.append(name)
    return skipped


shutil.copytree(runtime / "usr/share/doc", stage / "runtime/licenses", symlinks=True,
                ignore=ignore_missing_documentation)
if not options.reference_smoke:
    audio_license = stage / "runtime/licenses/pulse-webos"
    audio_license.mkdir()
    shutil.copyfile(audio / "source/LICENSE", audio_license / "LICENSE")
    shutil.copyfile(project / "patches/pulse-webos-playback.patch",
                    audio_license / "pulse-webos-playback.patch")
    shutil.copyfile(audio / "build-result.json", audio_license / "build-result.json")
    common_licenses = runtime / "usr/share/common-licenses"
    if not common_licenses.is_dir():
        common_licenses = pathlib.Path("/usr/share/common-licenses")
    shutil.copytree(common_licenses, stage / "runtime/licenses/common-licenses")
    shutil.copyfile(workspace / "src/LICENSE", engine / "CHROMIUM_LICENSE")
    sdl_license = stage / "runtime/licenses/SDL3"
    sdl_license.mkdir()
    shutil.copyfile(workspace / "sdl3-webos/share/licenses/SDL3/LICENSE.txt",
                    sdl_license / "LICENSE.txt")

if not options.reference_smoke:
    for name in ("appinfo.json", "start-native.sh", "update-adblock.js",
                 "adblock-resources.js", "acorn.js", "acorn.LICENSE",
                 "adblock-uBlock.LICENSE", "initialize-profile.js"):
        content = (project / "app" / name).read_bytes().replace(b"\r\n", b"\n")
        (stage / name).write_bytes(content)
    launcher = (project / "app/launch.sh").read_bytes().replace(b"\r\n", b"\n")
    original_data = b"DATA=/media/developer/brave-b9-data\n"
    if launcher.count(original_data) != 1:
        raise RuntimeError("The browser launcher has an unexpected data path")
    launcher = launcher.replace(
        original_data, b"DATA=/media/developer/brave-b9-internal/data\n"
    )
    (stage / "launch-engine.sh").write_bytes(launcher)
    (stage / "launch-engine.sh").chmod(0o755)
    (stage / "launch.sh").write_bytes(
        (project / "app/launch-bundled.sh").read_bytes().replace(b"\r\n", b"\n")
    )
    (stage / "launch.sh").chmod(0o755)
    (stage / "start-native.sh").chmod(0o755)
    for binary in ["webos-display", "browser-exec"]:
        shutil.copyfile(workspace / "display-build" / binary, stage / binary)
        (stage / binary).chmod(0o755)
    subprocess.run(
        ["patchelf", "--set-rpath", "$ORIGIN/display/lib", str(stage / "webos-display")],
        check=True,
    )
    display_lib = stage / "display/lib"
    display_lib.mkdir(parents=True)
    shutil.copyfile(
        workspace / "sdl3-webos/lib/libSDL3.so.0.4.16",
        display_lib / "libSDL3.so.0",
    )
    icon = workspace / "src/brave/app/theme/brave/product_logo_128.png"
    if not icon.is_file():
        raise RuntimeError(f"Required app icon is missing: {icon}")
    shutil.copyfile(icon, stage / "icon.png")
    node = workspace / f"tools/node-v{config['nodeVersion']}-linux-x64/bin/node"
    if not node.is_file():
        raise RuntimeError("Run bootstrap.sh to prepare the pinned Node build tool")
    subprocess.run([str(node), str(project / "scripts/prepare-profile.js"),
                    str(stage / "defaults")], check=True)
    certificates = stage / "runtime/ca-certificates.crt"
    certificate_url = "https://curl.se/ca/cacert.pem"
    subprocess.run([
        "curl", "--fail", "--silent", "--show-error", "--location",
        "--proto", "=https", "--proto-redir", "=https",
        "--connect-timeout", "10", "--max-time", "60",
        certificate_url, "-o", str(certificates),
    ], check=True)
    if certificates.read_bytes().count(b"-----BEGIN CERTIFICATE-----") < 10:
        raise RuntimeError("The public CA bundle is incomplete")
    (stage / "runtime/ca-source.json").write_text(json.dumps({
        "url": certificate_url,
        "sha256": hashlib.sha256(certificates.read_bytes()).hexdigest(),
    }, indent=2) + "\n", encoding="ascii")

for directory in [stage, *(file for file in stage.rglob("*") if file.is_dir()
                           and not file.is_symlink())]:
    directory.chmod(0o755)
for file in stage.rglob("*"):
    if file.is_file() and not file.is_symlink():
        file.chmod(0o755 if file.stat().st_mode & 0o111 else 0o644)

manifest = []
size = 0
for file in sorted(stage.rglob("*")):
    if file.is_file() and not file.is_symlink():
        with file.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        manifest.append(f"{digest}  {file.relative_to(stage).as_posix()}\n")
        size += file.stat().st_size
(stage / "SHA256SUMS").write_text("".join(manifest), encoding="ascii", newline="\n")
if options.reference_smoke:
    transfer = [
        line for line in manifest
        if line.split("  ", 1)[1].strip() in {"engine/brave", "engine/chrome_crashpad_handler"}
        or line.split("  ", 1)[1].startswith(("runtime/lib/", "runtime/fonts/", "runtime/fonts.conf"))
    ]
    (stage / "TRANSFER-SHA256SUMS").write_text(
        "".join(transfer), encoding="ascii", newline="\n"
    )
print(f"Prepared {stage}: {len(manifest)} files, {size / 1024**2:.1f} MiB")
print(f"Private ARM64 runtime: {len(selected)} libraries")
print(f"Interpreter: {interpreter}")

if not options.reference_smoke:
    archive = output_directory / f"{app_id}_{app_info['version']}_arm.ipk"
    if archive.exists():
        raise RuntimeError(f"Refusing to overwrite an existing package: {archive}")
    control_dir = output_directory / f"{app_id}-control"
    control_dir.mkdir()
    (control_dir / "debian-binary").write_text("2.0\n", encoding="ascii")
    (control_dir / "control").write_text(
        f"Package: {app_id}\n"
        f"Version: {app_info['version']}\n"
        "Section: misc\n"
        "Priority: optional\n"
        "Architecture: arm\n"
        f"Maintainer: {app_info['vendor']}\n"
        f"Installed-Size: {(size + 1023) // 1024}\n"
        "Description: Complete native Brave B9 port with reduced browser sandbox\n",
        encoding="ascii",
    )
    def package_metadata(entry):
        entry.uid = entry.gid = 0
        entry.uname = entry.gname = "root"
        entry.mtime = 0
        if entry.isdir():
            entry.mode = 0o755
        elif entry.isfile():
            entry.mode = 0o755 if entry.mode & 0o111 else 0o644
        return entry

    with tarfile.open(control_dir / "control.tar.gz", "w:gz") as output:
        output.add(control_dir / "control", arcname="./control",
                   filter=package_metadata)
    with tarfile.open(control_dir / "data.tar.gz", "w:gz", compresslevel=6) as output:
        output.add(stage, arcname=f"./usr/palm/applications/{app_id}",
                   filter=package_metadata)
    # The TV's old ar extractor requires file-type bits omitted by deterministic mode.
    subprocess.run(
        ["ar", "rcU", str(archive), "debian-binary", "control.tar.gz", "data.tar.gz"],
        cwd=control_dir,
        check=True,
    )
    with archive.open("rb") as stream:
        archive_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    archive.with_suffix(".ipk.sha256").write_text(
        f"{archive_hash}  {archive.name}\n", encoding="ascii"
    )
    print(f"Installable package: {archive} ({archive.stat().st_size / 1024**2:.1f} MiB)")
