import argparse
import hashlib
import io
import json
import pathlib
import posixpath
import tarfile


def verify(package):
    files = {}
    links = {}
    directories = set()
    small = {}
    control = None
    members = []
    required = {
        "appinfo.json", "launch.sh", "launch-engine.sh", "start-native.sh",
        "browser-exec", "webos-display", "webos-media", "engine/brave", "engine/resources.pak",
        "engine/icudtl.dat", "engine/LICENSE", "display/lib/libSDL3.so.0",
        "runtime/lib/ld-linux-aarch64.so.1", "runtime/lib/libpulse.so.0",
        "runtime/lib/libpulsecommon-16.1.so", "runtime/fonts.conf",
        "runtime/ca-certificates.crt", "update-adblock.js",
        "adblock-resources.js", "acorn.js", "acorn.LICENSE",
        "adblock-uBlock.LICENSE", "initialize-profile.js",
        "defaults/profile/Local State", "defaults/profile/Default/Preferences",
        "SHA256SUMS",
    }
    forbidden = {
        "Cookies", "History", "Login Data", "Sessions", "Session Storage",
        "SingletonLock", "SingletonSocket", "SingletonCookie",
    }
    with package.open("rb") as source:
        if source.read(8) != b"!<arch>\n":
            raise ValueError("Not an IPK ar archive")
        while header := source.read(60):
            if len(header) != 60 or header[58:] != b"`\n":
                raise ValueError("Invalid ar member header")
            name = header[:16].decode("ascii").strip().rstrip("/")
            size = int(header[48:58])
            if size < 0 or name in members:
                raise ValueError("Invalid or duplicate ar member")
            members.append(name)
            if int(header[40:48], 8) & 0o170000 != 0o100000:
                raise ValueError("ar member lacks file-type bits required by the TV")
            start = source.tell()
            if name == "debian-binary":
                if source.read(size) != b"2.0\n":
                    raise ValueError("Unsupported package version")
            elif name == "control.tar.gz":
                with tarfile.open(fileobj=io.BytesIO(source.read(size)), mode="r:gz") as archive:
                    entries = [entry for entry in archive if entry.isfile()]
                    if len(entries) != 1 or entries[0].name.lstrip("./") != "control":
                        raise ValueError("Unexpected control archive")
                    text = archive.extractfile(entries[0]).read().decode("ascii")
                    control = dict(line.split(": ", 1) for line in text.splitlines())
            elif name == "data.tar.gz":
                if not control:
                    raise ValueError("Package control must precede payload")
                prefix = pathlib.PurePosixPath("usr/palm/applications") / control["Package"]
                with tarfile.open(fileobj=source, mode="r|gz") as archive:
                    for entry in archive:
                        path = pathlib.PurePosixPath(entry.name)
                        if ".." in path.parts or path.is_absolute():
                            raise ValueError("Unsafe archive path")
                        relative = path.relative_to(prefix).as_posix()
                        if entry.uid or entry.gid or entry.uname != "root" or entry.gname != "root":
                            raise ValueError("Unexpected package ownership")
                        if not entry.issym() and entry.mode & 0o022:
                            raise ValueError("Group/other-writable application file")
                        if forbidden.intersection(path.parts):
                            raise ValueError("Personal profile/session data in package")
                        if entry.isfile():
                            if relative in files or relative in links:
                                raise ValueError("Duplicate package file")
                            stream = archive.extractfile(entry)
                            digest = hashlib.sha256()
                            chunks = [] if relative in required else None
                            for block in iter(lambda: stream.read(1024 * 1024), b""):
                                digest.update(block)
                                if chunks is not None and entry.size <= 1024 * 1024:
                                    chunks.append(block)
                            files[relative] = digest.hexdigest()
                            if chunks is not None and entry.size <= 1024 * 1024:
                                small[relative] = b"".join(chunks)
                        elif entry.issym():
                            target = posixpath.normpath(
                                posixpath.join(posixpath.dirname(relative), entry.linkname))
                            if entry.linkname.startswith("/") or target == ".." or target.startswith("../"):
                                raise ValueError("External package symlink")
                            links[relative] = target
                        elif entry.isdir():
                            directories.add(relative)
                        else:
                            raise ValueError("Unsupported package entry type")
            else:
                raise ValueError("Unexpected IPK member: " + name)
            source.seek(start + size + size % 2)
    if members != ["debian-binary", "control.tar.gz", "data.tar.gz"]:
        raise ValueError("Incomplete IPK")
    if missing := required.difference(files):
        raise ValueError("Missing bundled files: " + ", ".join(sorted(missing)))
    manifest = dict(line.split("  ", 1)[::-1]
                    for line in small["SHA256SUMS"].decode("ascii").splitlines())
    expected = {name: digest for name, digest in files.items() if name != "SHA256SUMS"}
    if manifest != expected:
        raise ValueError("Package manifest does not cover/match every regular file")
    for name, target in links.items():
        seen = {name}
        while target in links:
            if target in seen:
                raise ValueError("Cyclic package symlink")
            seen.add(target)
            target = links[target]
        if target not in files and target not in directories:
            raise ValueError("Broken package symlink: " + name)
    info = json.loads(small["appinfo.json"])
    if (info["version"] != control["Version"] or info["id"] != control["Package"] or
            info["vendor"] != "Pecrow" or control["Maintainer"] != "Pecrow" or
            info["main"] != "webos-media"):
        raise ValueError("Inconsistent release metadata")
    helper = small["webos-media"]
    if helper[:6] != b"\x7fELF\x01\x01" or helper[18:20] != b"\x28\x00":
        raise ValueError("Native media helper is not ARM32")
    for name in ["launch.sh", "launch-engine.sh", "start-native.sh"]:
        script = small[name]
        if b"\r" in script or b"--remote-debugging" in script:
            raise ValueError("Invalid production launcher")
        if b"bridge-64to32" in script:
            raise ValueError("Launcher depends on an external application")
    if (b"\nDATA=/media/developer/brave-b9-internal/data\n" not in
            small["launch-engine.sh"]):
        raise ValueError("Engine launcher does not use the internal profile")
    if b"brave-b9-usb" in small["launch.sh"] or b"brave-b9-usb" in small["start-native.sh"]:
        raise ValueError("Bootstrap depends on an external USB application")
    if b"brave-b9-internal/app" in small["launch.sh"]:
        raise ValueError("Launcher still expects an external payload")
    if (b"export BRAVE_WEBOS_NATIVE_MEDIA=1\n" not in small["launch-engine.sh"] or
            b'export BRAVE_WEBOS_MEDIA_SOCKET="$RUN/media.sock"\n' not in
            small["launch-engine.sh"]):
        raise ValueError("Native hardware playback is not enabled")
    state = json.loads(small["defaults/profile/Local State"])
    preferences = json.loads(small["defaults/profile/Default/Preferences"])
    if (set(state) != {"performance_tuning"} or
            set(preferences) != {"profile", "homepage", "homepage_is_newtabpage",
                                 "browser", "session", "brave"}):
        raise ValueError("Unexpected content in generated profile defaults")
    homepage = "https://github.com/pecrow/WebOS-Brave-Browser-Port/"
    if (preferences["homepage"] != homepage or
            preferences["homepage_is_newtabpage"] is not False or
            preferences["browser"] != {
                "show_home_button": True, "theme": {"color_scheme2": 2}} or
            preferences["session"] != {
                "restore_on_startup": 4, "startup_urls": [homepage]}):
        raise ValueError("Repository home/startup defaults do not match")
    if preferences["brave"] != {"dark_mode_migrated": True, "darker_mode": True}:
        raise ValueError("Black browser theme defaults do not match")
    memory = state["performance_tuning"]["high_efficiency_mode"]
    if memory != {"state": 2, "aggressiveness": 1}:
        raise ValueError("Memory Saver defaults do not match the TV baseline")
    autoplay = preferences["profile"]["content_settings"]["exceptions"]["autoplay"]
    if any(autoplay[key]["setting"] != 2 for key in
           ["[*.]youtube.com,*", "https://www.youtube.com:443,*"]):
        raise ValueError("YouTube autoplay defaults do not match")
    for component, filename in [
        ("mfddibmblmbccpadfndgakiopmmhebop", "resources.json"),
        ("gkboaolpopklhgplhaaiboijnklogmbc", "list_catalog.json"),
    ]:
        if not any(name.startswith("defaults/profile/" + component + "/") and
                   name.endswith("/" + filename) for name in files):
            raise ValueError("Offline ad-block resources are missing")
    return {"version": info["version"], "files": len(files), "symlinks": len(links),
            "bytes": package.stat().st_size, "complete": True}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=pathlib.Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.package), indent=2))
