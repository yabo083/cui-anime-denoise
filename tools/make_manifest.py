#!/usr/bin/env python3
"""Generate dist/latest.json for the in-app updater.

The manifest format is documented in src/update.h:

    { "version": "1.2.0",
      "url": "https://.../AnimeDenoise-1.2.0-setup.exe",
      "sha256": "9f2c...",
      "size": 24000000,
      "notes": "UTF-8 release notes",
      "page": "https://github.com/..." }

Usage:
    python tools/make_manifest.py dist/AnimeDenoise-1.2.0-setup.exe 1.2.0 notes.md
    python tools/make_manifest.py setup.exe 9.9.9 notes.md \
        --url http://127.0.0.1:8765/update-test.exe -o dist/latest.json

When --url is omitted the GitHub release asset URL is derived from the
repository's `origin` remote; pass --url for any other host (or for a local
test server). Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

VERSION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+){1,3}$")


def sha256_of(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def github_repo(root: Path) -> str | None:
    """Returns 'owner/repo' when `origin` is a GitHub remote."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "remote", "get-url", "origin"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"github\.com[:/]([^/]+)/([^/\s]+?)(?:\.git)?$", out)
    if not m:
        return None
    return f"{m.group(1)}/{m.group(2)}"


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Write dist/latest.json for the Anime Denoise self-updater."
    )
    parser.add_argument("setup", type=Path, help="installer .exe to describe")
    parser.add_argument("version", help="version string, e.g. 1.2.0")
    parser.add_argument("notes", type=Path, help="UTF-8 release notes (plain text)")
    parser.add_argument(
        "--url",
        default=None,
        help="download URL of the installer (default: GitHub release asset)",
    )
    parser.add_argument(
        "--page",
        default=None,
        help="release page URL (default: GitHub release tag)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=root / "dist" / "latest.json",
        help="output manifest (default: <repo>/dist/latest.json)",
    )
    args = parser.parse_args(argv)

    if not VERSION_RE.match(args.version):
        parser.error(f"invalid version {args.version!r} (expected x.y.z)")
    setup = args.setup.resolve()
    if not setup.is_file():
        parser.error(f"installer not found: {setup}")
    if not args.notes.is_file():
        parser.error(f"notes file not found: {args.notes}")

    repo = github_repo(root)
    url = args.url
    page = args.page
    if url is None:
        if not repo:
            parser.error(
                "no --url given and the git origin is not a GitHub remote; "
                "pass --url explicitly"
            )
        url = f"https://github.com/{repo}/releases/download/v{args.version}/{setup.name}"
    if page is None:
        page = (
            f"https://github.com/{repo}/releases/tag/v{args.version}" if repo else ""
        )

    notes = args.notes.read_text(encoding="utf-8-sig").strip()
    sha256, size = sha256_of(setup)

    manifest = {
        "version": args.version,
        "url": url,
        "sha256": sha256,
        "size": size,
        "notes": notes,
        "page": page,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    print(f"wrote {args.output}")
    print(f"  version {args.version}")
    print(f"  url     {url}")
    print(f"  sha256  {sha256}")
    print(f"  size    {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
