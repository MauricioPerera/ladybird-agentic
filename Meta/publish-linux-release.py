#!/usr/bin/env python3
"""Publish a verified set of Linux packages once per version, commit, and format."""

import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request, urlopen


CHECKSUMS = "SHA256SUMS-linux-ubuntu-2404.txt"
PACKAGE_FORMATS = {
    "appimage-and-deb": (".AppImage", ".deb"),
    "appimage-only": (".AppImage",),
    "deb-only": (".deb",),
}
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:[.~-][0-9A-Za-z._-]+)?")


def digest(file):
    with file.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verified_assets(directory, package_format):
    packages = sorted(file for file in directory.iterdir() if file.is_file() and file.suffix in (".AppImage", ".deb"))
    if sorted(file.suffix for file in packages) != sorted(PACKAGE_FORMATS[package_format]):
        raise ValueError(f"Expected exactly {PACKAGE_FORMATS[package_format]}, got {[file.name for file in packages]}")

    manifest = directory / CHECKSUMS
    declared = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([a-f0-9]{64}) [ *]([^/\\]+)", line)
        if match is None or match[2] in declared:
            raise ValueError("Invalid or duplicate entry in checksum manifest")
        declared[match[2]] = match[1]
    actual = {file.name: digest(file) for file in packages}
    if declared != actual:
        raise ValueError("Checksum manifest does not match the package names and bytes")
    return [*packages, manifest], {**actual, manifest.name: digest(manifest)}


def github_json(endpoint):
    api_url = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    request = Request(
        f"{api_url}/{endpoint}",
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {os.environ['GH_TOKEN']}",
        },
    )
    try:
        with urlopen(request, timeout=30) as response:
            return json.load(response)
    except HTTPError as error:
        if error.code == 404:
            return None
        raise


def publish(directory, repository, commit, version, package_format, run_id):
    if not VERSION.fullmatch(version) or not re.fullmatch(r"[a-f0-9]{40}", commit):
        raise ValueError("Expected a supported version string and a full Git commit SHA")

    files, expected_digests = verified_assets(directory, package_format)
    tag_version = version.replace("~", "%7E").replace("..", ".%2E")
    tag = f"linux-ubuntu-2404-v{tag_version}-{commit}-{package_format}"
    tag_path = quote(tag, safe="")
    complete = package_format == "appimage-and-deb"

    tagged_commit = github_json(f"repos/{repository}/commits/{tag_path}")
    if tagged_commit is not None and tagged_commit["sha"] != commit:
        raise ValueError(f"Tag {tag} points to a different commit")

    existing = github_json(f"repos/{repository}/releases/tags/{tag_path}")
    if existing is not None:
        assets = {
            asset["name"]: asset.get("digest")
            for asset in existing["assets"]
            if asset["state"] == "uploaded"
        }
        expected_assets = {name: f"sha256:{value}" for name, value in expected_digests.items()}
        if (tagged_commit is None or existing["draft"] or existing["prerelease"] == complete
                or len(existing["assets"]) != len(expected_assets) or assets != expected_assets):
            raise ValueError(f"Release {tag} already exists with different metadata or assets")
        print(f"Verified existing release {tag}")
        return

    notes = (
        f"Ubuntu 24.04 x86_64 Distribution build from commit `{commit}`.\n"
        f"Workflow run: `{run_id}`. Package format: `{package_format}`.\n\n"
        f"```text\n{(directory / CHECKSUMS).read_text(encoding='utf-8').rstrip()}\n```"
    )
    flags = ["--latest"] if complete else ["--prerelease", "--latest=false"]
    subprocess.run(
        ["gh", "release", "create", tag, *(str(file.resolve()) for file in files),
         "--repo", repository, "--target", commit, *flags,
         "--title", f"Ladybird Linux Ubuntu 24.04 ({version} @ {commit[:10]}, {package_format})",
         "--notes", notes],
        check=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("package_format", choices=PACKAGE_FORMATS)
    parser.add_argument("version")
    args = parser.parse_args()
    try:
        publish(args.directory, os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_SHA"],
                args.version, args.package_format, os.environ["GITHUB_RUN_ID"])
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
