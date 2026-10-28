"""Build a portable release directory and a hash manifest; this does not publish anything."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import zipfile


SUFFIXES = ("", "oss", "luna", "terra", "sol", "astra")
# Special editions run their own version line and are offered by choice, never as an automatic update.
EDITIONS = ("", "haiku", "sonnet", "opus", "fable")


def parse_version(value):
    match = re.fullmatch(r"(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})(?:-(oss|luna|terra|sol|astra)?)?", value)
    if not match:
        raise ValueError("Invalid CCA version")
    return tuple(map(int, match.group(1, 2, 3))) + (SUFFIXES.index(match.group(4) or ""),)


def next_version(value, major_update=False):
    major, minor, patch, annotation = parse_version(value)
    if major_update and annotation < 5:
        suffix = "astra"
    elif annotation < 4:
        suffix = SUFFIXES[annotation + 1]
    else:
        patch += 1
        suffix = ""
    result = f"{major}.{minor}.{patch}-{suffix}"
    parse_version(result)
    return result


def header_version(text, edition):
    """The CCA_VERSION of one edition: the special block of src/version.hpp, or the ordinary #else branch."""
    if edition:
        block = re.search(r"#if defined\(CCA_EDITION_" + edition.upper() + r"\)(.*?)#(?:else|elif|endif)", text, re.S)
    else:
        block = re.search(r"#else(.*?)#endif", text, re.S)
    found = re.search(r'#define CCA_VERSION "([^"]+)"', block[1] if block else text)
    if not found:
        raise ValueError("CCA_VERSION not found for edition " + (edition or "ordinary"))
    return found[1]


def android_release(args, root, parser):
    """A Haiku release folder: the APK and a manifest whose only file is the Android package."""
    if args.edition != "haiku" or not args.output:
        parser.error("--apk needs --edition haiku and --output")
    if args.output.exists():
        parser.error("Output directory already exists; use a new release directory")
    text = (root / "android" / "haiku" / "AndroidManifest.xml").read_text(encoding="utf-8")
    version = re.search(r'android:versionName="([^"]+)"', text)[1]
    parse_version(version)
    data = args.apk.read_bytes()
    with zipfile.ZipFile(args.apk) as apk:
        if "AndroidManifest.xml" not in apk.namelist() or "classes.dex" not in apk.namelist():
            parser.error(f"Not an Android package: {args.apk}")
    name = f"CCA-Haiku-{version}.apk"
    args.output.mkdir(parents=True)
    shutil.copy2(args.apk, args.output / name)
    manifest = {"schema": 1, "version": version, "edition": "haiku", "platform": "android", "notes": args.notes,
                "files": {"android": {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}}}
    (args.output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(args.output.resolve())


def resource_field(path, name):
    version = ctypes.WinDLL("version", use_last_error=True)
    size_fn = version.GetFileVersionInfoSizeW
    size_fn.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
    size_fn.restype = ctypes.c_ulong
    ignored = ctypes.c_ulong()
    size = size_fn(str(path.resolve()), ctypes.byref(ignored))
    if not size or size > 65536:
        return ""
    data = ctypes.create_string_buffer(size)
    read = version.GetFileVersionInfoW
    read.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    if not read(str(path.resolve()), 0, size, data):
        return ""
    query = version.VerQueryValueW
    query.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint)]
    value, length = ctypes.c_void_p(), ctypes.c_uint()
    if not query(data, "\\StringFileInfo\\040404b0\\" + name, ctypes.byref(value), ctypes.byref(length)) or not length.value:
        return ""
    return ctypes.wstring_at(value.value, length.value - 1)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--desktop", type=Path)
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--notes", default="")
    parser.add_argument("--next", metavar="VERSION")
    parser.add_argument("--major-update", action="store_true")
    parser.add_argument("--edition", default="", choices=EDITIONS, help="special edition built with CCA_EDITION")
    parser.add_argument("--apk", type=Path, help="Android package (Haiku) instead of the Windows executables")
    args = parser.parse_args()
    if args.next:
        print(next_version(args.next, args.major_update))
        return
    root = Path(__file__).resolve().parent.parent
    if args.apk:
        return android_release(args, root, parser)
    if not all((args.desktop, args.cli, args.output)):
        parser.error("--desktop, --cli and --output are required")
    version = header_version((root / "src/version.hpp").read_text(encoding="utf-8"), args.edition)
    parse_version(version)
    if args.output.exists():
        parser.error("Output directory already exists; use a new release directory")
    files = {}
    for kind, source, name in (("desktop", args.desktop, "ChatClaudeAgent.exe"),
                               ("cli", args.cli, "ChatClaudeAgent-CLI.exe")):
        data = source.read_bytes()
        if data[:2] != b"MZ":
            parser.error(f"Not an executable: {source}")
        if (resource_field(source, "ProductName") != "ChatClaudeAgent" or
                resource_field(source, "ProductVersion") != version or
                resource_field(source, "CCAEdition") != args.edition or
                resource_field(source, "OriginalFilename") != name):
            parser.error(f"Product/version/flavor metadata mismatch: {source}")
        files[kind] = {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    args.output.mkdir(parents=True)
    for kind, source in (("desktop", args.desktop), ("cli", args.cli)):
        shutil.copy2(source, args.output / files[kind]["name"])
    manifest = {"schema": 1, "version": version, "notes": args.notes, "files": files}
    if args.edition:
        manifest["edition"] = args.edition
    (args.output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
