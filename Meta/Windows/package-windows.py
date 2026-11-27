"""Stage and package a standalone Windows x64 distribution of Ladybird Agentic."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

from PIL import Image


RUNTIME_EXES = [
    "Ladybird.exe",
    "AudioServer.exe",
    "Compositor.exe",
    "Hub.exe",
    "ImageDecoder.exe",
    "MediaServer.exe",
    "RequestServer.exe",
    "SystemProbe.exe",
    "VideoDecoderServer.exe",
    "WebContent.exe",
    "WebRTCClient.exe",
    "WebDriver.exe",
]

QT_PLUGIN_DIRS = [
    "generic",
    "imageformats",
    "networkinformation",
    "platforms",
    "position",
    "styles",
    "tls",
]

DEFAULT_EDITBIN = Path(
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\editbin.exe"
)
DEFAULT_CRT_DIR = Path(
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
)


def read_pe_subsystem(exe_path: Path) -> int:
    """Read IMAGE_OPTIONAL_HEADER.Subsystem from a PE executable."""
    with exe_path.open("rb") as fh:
        fh.seek(0x3C)
        pe_offset = struct.unpack("<I", fh.read(4))[0]
        fh.seek(pe_offset + 24 + 68)
        return struct.unpack("<H", fh.read(2))[0]


def generate_ico(source_dir: Path, out_ico: Path) -> Path:
    """Create a multi-resolution Windows .ico file from repository artwork."""
    candidates = [
        source_dir / "UI" / "Icons" / "ladybird.png",
        source_dir / "UI" / "Icons" / "macos" / "app_icon.iconset" / "icon_256x256.png",
    ]
    src = next((p for p in candidates if p.is_file()), None)
    if src is None:
        raise FileNotFoundError(f"No icon PNG found in {[str(c) for c in candidates]}")

    out_ico.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(src) as img:
        rgba = img.convert("RGBA")
        rgba.save(
            out_ico,
            format="ICO",
            sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
        )
    return out_ico


def write_readme(readme_path: Path, version: str) -> None:
    """Write end-user Windows instructions for the standalone package."""
    content = f"""Ladybird Agentic {version} — Windows x64 Standalone Package
====================================================================

1. Quick Launch (GUI)
   Open `bin\\Ladybird.exe` by double-clicking it.
   No installer commands, environment variables, or external runtime scripts are required.

2. Windows SmartScreen Note (Unsigned Open-Source Build)
   This open-source build is not yet Authenticode-signed. On first launch, Windows
   Defender SmartScreen may display "Windows protected your PC".
   Click "More info" -> "Run anyway" to proceed.

3. Optional Headless / MCP Server Mode (CLI)
   Example:
     bin\\Ladybird.exe --headless=manual --mcp=7331 --mcp-token "change-me" --mcp-redact-screenshot-media

   Security defaults:
     - eval_js and call_page_tool are disabled unless --mcp-allow-eval-js is passed.
     - Agents default to the isolated "agent" container with Egress Taint Guard enabled.
     - Inspect security state and audit history inside the browser at about:agent.

4. Licenses
   See LICENSE and LICENSES.txt in this directory.
"""
    readme_path.write_text(content, encoding="utf-8")


def stage_package(
    build_dir: Path,
    source_dir: Path,
    out_dir: Path,
    version: str,
    editbin_path: Path,
    crt_dir: Path,
) -> tuple[Path, Path]:
    """Copy runtime binaries, DLLs, Qt plugins, CRT redistributables, and resources into staging."""
    bin_src = build_dir / "bin"
    share_src = build_dir / "share" / "Lagom"
    if not bin_src.is_dir():
        raise FileNotFoundError(f"Missing build bin directory: {bin_src}")
    if not share_src.is_dir():
        raise FileNotFoundError(f"Missing build share/Lagom directory: {share_src}")
    if not editbin_path.is_file():
        raise FileNotFoundError(f"Missing editbin.exe: {editbin_path}")
    if not crt_dir.is_dir():
        raise FileNotFoundError(f"Missing MSVC CRT directory: {crt_dir}")

    package_root_name = f"ladybird-{version}-Windows-x86_64"
    stage_dir = out_dir / "stage" / package_root_name
    if stage_dir.exists():
        shutil.rmtree(stage_dir)

    stage_bin = stage_dir / "bin"
    stage_share = stage_dir / "share" / "Lagom"
    stage_bin.mkdir(parents=True, exist_ok=True)
    stage_share.parent.mkdir(parents=True, exist_ok=True)

    icon_path = generate_ico(source_dir, out_dir / "ladybird.ico")

    # Copy strictly the 12 runtime executables (exclude Test* / CLI dev tools).
    for exe_name in RUNTIME_EXES:
        src_exe = bin_src / exe_name
        if not src_exe.is_file():
            raise FileNotFoundError(f"Missing required runtime executable: {src_exe}")
        shutil.copy2(src_exe, stage_bin / exe_name)

    # Copy runtime DLLs built/deployed in Build/release/bin.
    dlls = sorted(bin_src.glob("*.dll"))
    if not dlls:
        raise RuntimeError(f"No DLLs found in {bin_src}")
    for dll in dlls:
        shutil.copy2(dll, stage_bin / dll.name)

    # Copy Qt plugin folders.
    for plugin_dir_name in QT_PLUGIN_DIRS:
        plugin_src = bin_src / plugin_dir_name
        if not plugin_src.is_dir():
            raise FileNotFoundError(f"Missing Qt plugin directory: {plugin_src}")
        shutil.copytree(plugin_src, stage_bin / plugin_dir_name)

    # Bundle app-local MSVC release CRT DLLs.
    crt_dlls = sorted(crt_dir.glob("*.dll"))
    if not crt_dlls:
        raise RuntimeError(f"No CRT DLLs found in {crt_dir}")
    for crt_dll in crt_dlls:
        shutil.copy2(crt_dll, stage_bin / crt_dll.name)

    # Copy runtime assets (share/Lagom).
    shutil.copytree(share_src, stage_share)

    # Copy root license files, icon, and user README.
    for license_src, dest_name in [
        (source_dir / "LICENSE", "LICENSE"),
        (build_dir / "LICENSES.txt", "LICENSES.txt"),
    ]:
        if not license_src.is_file():
            raise FileNotFoundError(f"Missing license file: {license_src}")
        shutil.copy2(license_src, stage_dir / dest_name)

    shutil.copy2(icon_path, stage_dir / "ladybird.ico")
    write_readme(stage_dir / "README-Windows.txt", version)

    # Switch ONLY the staged Ladybird.exe to WINDOWS_GUI subsystem so double-click opens no console.
    staged_ladybird = stage_bin / "Ladybird.exe"
    subprocess.run(
        [str(editbin_path), "/NOLOGO", "/SUBSYSTEM:WINDOWS", str(staged_ladybird)],
        check=True,
    )
    subsystem = read_pe_subsystem(staged_ladybird)
    if subsystem != 2:
        raise RuntimeError(f"Expected staged Ladybird.exe PE Subsystem=2 (WINDOWS_GUI), got {subsystem}")

    print(
        f"STAGED_OK: exes={len(RUNTIME_EXES)} dlls={len(dlls)} crt_dlls={len(crt_dlls)} SUBSYSTEM={subsystem}"
    )
    return stage_dir, icon_path


def create_portable_zip(stage_dir: Path, out_dir: Path, version: str) -> Path:
    """Create and verify the portable zip archive with top-level package folder."""
    zip_path = out_dir / f"ladybird-{version}-Windows-x86_64-portable.zip"
    if zip_path.exists():
        zip_path.unlink()

    stage_parent = stage_dir.parent
    files = sorted(p for p in stage_dir.rglob("*") if p.is_file())
    with zipfile.ZipFile(
        zip_path,
        mode="w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=6,
        allowZip64=True,
    ) as zf:
        for file_path in files:
            zf.write(file_path, arcname=file_path.relative_to(stage_parent).as_posix())

    with zipfile.ZipFile(zip_path, mode="r") as zf:
        bad_member = zf.testzip()
        if bad_member is not None:
            raise RuntimeError(f"Zip integrity verification failed on member: {bad_member}")

    size_bytes = zip_path.stat().st_size
    print(f"ZIP_OK: {zip_path} ({size_bytes} bytes, {len(files)} files)")
    return zip_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Package Ladybird Agentic for Windows x64")
    parser.add_argument("--build-dir", type=Path, default=Path(r"D:\ladybird\Build\release"))
    parser.add_argument("--source-dir", type=Path, default=Path(r"D:\ladybird"))
    parser.add_argument("--out-dir", type=Path, default=Path(r"D:\ladybird\Build\packages-windows"))
    parser.add_argument("--version", default="0.1.0-agentic")
    parser.add_argument("--editbin", type=Path, default=DEFAULT_EDITBIN)
    parser.add_argument("--crt-dir", type=Path, default=DEFAULT_CRT_DIR)
    parser.add_argument("--skip-installer", action="store_true", help="Reserved flag; portable zip is built")
    args = parser.parse_args()

    stage_dir, _ = stage_package(
        build_dir=args.build_dir,
        source_dir=args.source_dir,
        out_dir=args.out_dir,
        version=args.version,
        editbin_path=args.editbin,
        crt_dir=args.crt_dir,
    )
    create_portable_zip(stage_dir=stage_dir, out_dir=args.out_dir, version=args.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
