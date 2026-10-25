import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--full", action="store_true", help="Build Brave, not just the Ozone backend")
mode.add_argument("--export-only", action="store_true", help="Update generated port sources without starting a build")
options = parser.parse_args()

project = pathlib.Path(__file__).resolve().parents[1]
workspace = pathlib.Path.home() / "lg-tv-brave-v1.97.59"
source = workspace / "src"
destination = source / "ui/ozone/platform/webos"
marker = destination / ".brave-webos-generated"
if destination.exists() and not marker.exists():
    raise RuntimeError(f"Refusing to replace an existing platform: {destination}")
destination.mkdir(parents=True, exist_ok=True)
marker.write_text("Generated from brave-webos/src; edit the original sources.\n", encoding="utf-8")
for original in list((project / "src/ozone").iterdir()) + [
    project / "src/display/protocol.c",
    project / "src/display/protocol.h",
    project / "src/media/webos_media_protocol.c",
    project / "src/media/webos_media_protocol.h",
]:
    if original.is_file():
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=destination, delete=False
        ) as generated:
            generated.write(original.read_text(encoding="utf-8"))
        os.replace(generated.name, destination / original.name)

for name in ("webos_renderer.cc", "webos_renderer.h"):
    original = project / "src/media" / name
    target = source / "media/renderers" / name
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="\n", dir=target.parent, delete=False
    ) as generated:
        generated.write(original.read_text(encoding="utf-8"))
    os.replace(generated.name, target)

if options.export_only:
    print("Updated generated port sources; no additional build started.")
    sys.exit(0)

arguments = json.loads((project / "gn-args.json").read_text(encoding="utf-8"))
patches = [
    "ozone-dawn-dependencies.patch",
    "ozone-gpu-init-unused.patch",
    "webos-component-policy.patch",
    "webos-no-supervised-profiles.patch",
    "webos-no-custom-background-ui.patch",
    "webos-no-sandbox-warning.patch",
    "webos-native-media.patch",
    "webos-source-button.patch",
]
if not arguments.get("enable_widevine", True):
    patches.append("no-widevine-permission-helper.patch")
    patches.append("no-widevine-startup-metrics.patch")
if not arguments.get("enable_speech_service", True):
    patches.append("no-speech-test-dependencies.patch")
if not arguments.get("enable_background_mode", True):
    patches.append("no-background-test-dependencies.patch")
if not arguments.get("enable_background_contents", True):
    patches.append("no-background-contents-test-dependencies.patch")
    patches.append("no-background-contents-runtime.patch")
if not arguments.get("enable_media_remoting", True):
    patches.append("no-media-remoting-runtime.patch")
if not arguments.get("enable_print_preview", True):
    patches.append("no-print-preview-tests.patch")
    patches.append("no-print-preview-guest-tests.patch")
if not arguments.get("enable_printing", True):
    patches.append("no-printing-delegate.patch")
    patches.append("no-printing-targets.patch")
    patches.append("no-printing-command-metadata.patch")
    patches.append("no-printing-feature-flags.patch")
    patches.append("no-printing-context-menu.patch")
if not arguments.get("enable_pdf", True):
    patches.append("no-pdf-dependencies.patch")
    patches.append("no-pdf-test-dependencies.patch")
    patches.append("no-pdf-reading-mode.patch")
    patches.append("no-pdf-reading-mode-tree-actions.patch")
    patches.append("no-pdf-summary-promo.patch")
    patches.append("no-pdf-ocr-promo.patch")
    patches.append("no-pdf-feature-flags.patch")
    patches.append("no-pdf-plugin-response-interceptor.patch")
for name in patches:
    patch = (project / "patches" / name).read_text(encoding="utf-8")
    applied = subprocess.run(
        ["git", "apply", "--reverse", "--check", "-"],
        cwd=source,
        input=patch,
        text=True,
        capture_output=True,
        check=False,
    )
    if applied.returncode != 0:
        subprocess.run(
            ["git", "apply", "--check", "-"], cwd=source, input=patch, text=True, check=True
        )
        subprocess.run(
            ["git", "apply", "-"], cwd=source, input=patch, text=True, check=True
        )

command = [
    "pnpm", "run", "build", "Static", "-C", "WebOSB9",
    "--target_os", "linux", "--target_arch", "arm64",
    "--channel", "release", "--ninja", "j:8", "--use_remoteexec", "false",
]
for key, value in arguments.items():
    command.extend(["--gn", f"{key}:{json.dumps(value)}"])
if not options.full:
    command.extend(["--target", "ui/ozone/platform/webos:webos"])

environment = os.environ.copy()
environment["SISO_LIMITS"] = "local=8"
environment["PATH"] = os.pathsep.join([
    str(workspace / "tools/node-v24.16.0-linux-x64/bin"),
    str(workspace / "tools/pnpm/node_modules/.bin"),
    str(source / "brave/vendor/depot_tools"),
    environment["PATH"],
])
subprocess.run(command, cwd=source / "brave", env=environment, check=True)
