#!/usr/bin/env python3
"""Fetch the pinned upstream EchoEar release and extract its monochrome-eye GIFs.

The release ZIP is used only as a source container. No firmware image or
partition table from the 1.75B release is flashed or copied into this project.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

URL = (
    "https://github.com/xiaolonghuster/echoear/releases/download/"
    "echoear-v0.19.0-rc.1/waveshare-install.zip"
)
SHA256 = "57e20c0c0a5a2cf99a1e73d2418ffa4c8153de06ee9db5c2b619cc324ba3f9cb"
EXPECTED = (
    "brk_blink1.gif",
    "brk_happy.gif",
    "brk_sad.gif",
    "brk_angry.gif",
    "brk_sleep.gif",
    "brk_dizzy.gif",
    "brk_blink_fast.gif",
    "brk_blink_slow.gif",
)
MAGIC = b"\x5a\x5a"
GIF_HEADERS = (b"GIF87a", b"GIF89a")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def obtain_archive(cache_dir: Path) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / "echoear-v0.19.0-rc.1-waveshare-install.zip"
    if archive.is_file() and sha256_file(archive) == SHA256:
        return archive

    temporary = archive.with_suffix(".zip.download")
    print("Downloading pinned upstream EchoEar eye-asset source...")
    request = urllib.request.Request(URL, headers={"User-Agent": "echoear-eye-assets/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as out:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                out.write(chunk)
        actual = sha256_file(temporary)
        if actual != SHA256:
            raise RuntimeError(
                f"Upstream ZIP SHA-256 mismatch: expected {SHA256}, got {actual}"
            )
        temporary.replace(archive)
    finally:
        if temporary.exists():
            temporary.unlink()
    return archive


def locate_assets_blob(archive: Path) -> bytes:
    with zipfile.ZipFile(archive) as zf:
        candidates = [
            name for name in zf.namelist()
            if "assets-with-font.bin" in Path(name).name.lower()
        ]
        if len(candidates) != 1:
            raise RuntimeError(
                "Expected exactly one '*assets-with-font.bin' in upstream ZIP; "
                f"found {len(candidates)}: {candidates}"
            )
        return zf.read(candidates[0])


def extract_gifs(blob: bytes) -> dict[str, bytes]:
    if len(blob) < 12:
        raise RuntimeError("Assets blob is too small")
    count = struct.unpack_from("<I", blob, 0)[0]
    if count < 1 or count > 4096:
        raise RuntimeError(f"Implausible assets index count: {count}")
    index_end = 12 + count * 44
    if index_end > len(blob):
        raise RuntimeError("Assets index extends beyond the blob")

    found: dict[str, bytes] = {}
    data_base = index_end
    for index in range(count):
        entry = blob[12 + index * 44 : 12 + (index + 1) * 44]
        raw_name, size, offset, _width, _height = struct.unpack("<32sIIHH", entry)
        name = raw_name.split(b"\0", 1)[0].decode("utf-8", errors="strict")
        if name not in EXPECTED:
            continue

        starts = (data_base + offset, offset)
        lengths = (size, size + 2, max(0, size - 2))
        payload = None
        for start in starts:
            for length in lengths:
                candidate = blob[start : start + length]
                if candidate.startswith(MAGIC):
                    candidate = candidate[2:]
                if candidate.startswith(GIF_HEADERS):
                    payload = candidate
                    break
            if payload is not None:
                break
        if payload is None:
            raise RuntimeError(f"Could not locate valid GIF payload for {name}")
        found[name] = payload

    missing = sorted(set(EXPECTED) - set(found))
    if missing:
        raise RuntimeError(f"Upstream asset index is missing expected GIFs: {missing}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--cache-dir", required=True, type=Path)
    args = parser.parse_args()

    try:
        archive = obtain_archive(args.cache_dir)
        gifs = extract_gifs(locate_assets_blob(archive))
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for name, content in gifs.items():
            (args.output_dir / name).write_bytes(content)
        print(
            f"Prepared {len(gifs)} original EchoEar eye GIFs "
            f"({sum(map(len, gifs.values())):,} bytes) in {args.output_dir}"
        )
        return 0
    except Exception as exc:
        print(f"ERROR: unable to prepare EchoEar eye assets: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
