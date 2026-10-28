"""Fetch the WebView2 SDK files the build needs into third_party/webview2/build/native, pinned by SHA-256.

The repository does not carry the SDK's binaries. build.bat runs this when WebView2.h is missing; it can also be run by
hand. The package is downloaded from NuGet, its hash is compared with the pinned one before anything is unpacked, and only the
files in WANTED are written (so a hostile archive cannot place files elsewhere). Standard library only.
python tools/fetch_webview2.py            download if missing
python tools/fetch_webview2.py --force    download again
Set CCA_WEBVIEW2_NUPKG to a local .nupkg to work offline (the hash is still checked).
"""
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

VERSION = "1.0.4191.47"
SHA256 = "f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0"
URL = f"https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/{VERSION}/microsoft.web.webview2.{VERSION}.nupkg"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(ROOT, "third_party", "webview2")
WANTED = [
    "build/native/include/WebView2.h",
    "build/native/include/WebView2EnvironmentOptions.h",
    "build/native/x64/WebView2Loader.dll",
    "build/native/x64/WebView2Loader.dll.lib",
    "build/native/x64/WebView2LoaderStatic.lib",
]


def main():
    force = "--force" in sys.argv
    header = os.path.join(TARGET, WANTED[0])
    if os.path.exists(header) and not force:
        return 0
    local = os.environ.get("CCA_WEBVIEW2_NUPKG")
    try:
        if local:
            with open(local, "rb") as handle:
                data = handle.read()
        else:
            print(f"下載 WebView2 SDK {VERSION}…")
            request = urllib.request.Request(URL, headers={"User-Agent": "CCA-build"})
            with urllib.request.urlopen(request, timeout=120) as response:
                data = response.read(64 * 1024 * 1024)
    except OSError as error:
        print(f"無法取得 WebView2 SDK：{error}\n可從 {URL} 手動下載，設定 CCA_WEBVIEW2_NUPKG 指向它後再試一次。", file=sys.stderr)
        return 1
    digest = hashlib.sha256(data).hexdigest()
    if digest != SHA256:
        print(f"WebView2 套件的 SHA-256 不符：\n  取得 {digest}\n  預期 {SHA256}", file=sys.stderr)
        return 1
    with zipfile.ZipFile(io.BytesIO(data)) as package:
        for name in WANTED:
            destination = os.path.join(TARGET, *name.split("/"))
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            with open(destination, "wb") as out:
                out.write(package.read(name))
    print(f"WebView2 SDK {VERSION} 已放入 third_party/webview2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
