#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build"
DIST_DIR = ROOT / "dist" / "releases"


@dataclass(frozen=True)
class ArtifactSpec:
    src_rel: str
    dest_name: str
    required: bool


ARTIFACTS = (
    ArtifactSpec("bootloader/bootloader.bin", "bootloader.bin", True),
    ArtifactSpec("partition_table/partition-table.bin", "partition-table.bin", True),
    ArtifactSpec("ota_data_initial.bin", "ota_data_initial.bin", True),
    ArtifactSpec("xv6_esp32s3.bin", "xv6_esp32s3.bin", True),
    ArtifactSpec("xv6fs.bin", "xv6fs.bin", True),
    ArtifactSpec("qemu_flash.bin", "qemu_flash.bin", False),
    ArtifactSpec("qemu_efuse.bin", "qemu_efuse.bin", False),
)


def git_output(args: list[str]) -> str:
    try:
        out = subprocess.check_output(["git", *args], cwd=ROOT, text=True)
    except (subprocess.CalledProcessError, OSError):
        return "unknown"
    return out.strip() or "unknown"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def collect_artifacts(out_files_dir: Path) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    missing_required: list[str] = []

    for spec in ARTIFACTS:
        src = BUILD_DIR / spec.src_rel
        if not src.exists():
            if spec.required:
                missing_required.append(str(src))
            continue
        dest = out_files_dir / spec.dest_name
        shutil.copy2(src, dest)
        digest = sha256_file(dest)
        results.append(
            {
                "name": spec.dest_name,
                "source": str(src.relative_to(ROOT)),
                "size_bytes": dest.stat().st_size,
                "sha256": digest,
            }
        )

    if missing_required:
        lines = "\n".join(f"  - {p}" for p in missing_required)
        raise RuntimeError(f"required release artifact(s) not found:\n{lines}")

    return results


def write_sha256sums(files_dir: Path, artifacts: list[dict[str, object]], out_path: Path) -> None:
    lines = []
    for item in artifacts:
        digest = str(item["sha256"])
        name = str(item["name"])
        lines.append(f"{digest}  files/{name}")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def make_tarball(version: str, release_dir: Path) -> Path:
    tar_name = f"xv6-esp-{version}.tar.gz"
    tar_path = release_dir / tar_name
    with tarfile.open(tar_path, "w:gz") as tar:
        for child in sorted(release_dir.iterdir()):
            if child == tar_path:
                continue
            tar.add(child, arcname=child.name)
    return tar_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Create release bundle with checksums and manifest.")
    parser.add_argument("--version", required=True, help="release version (e.g. v1.2.3)")
    parser.add_argument("--commit", default="", help="git commit SHA for this release")
    parser.add_argument(
        "--out-dir",
        default=str(DIST_DIR),
        help="output release root directory (default: dist/releases)",
    )
    args = parser.parse_args()

    version = args.version.strip()
    if not version:
        raise SystemExit("--version must not be empty")

    commit = args.commit.strip() or git_output(["rev-parse", "HEAD"])
    dirty = git_output(["status", "--porcelain"])
    release_root = Path(args.out_dir).resolve() / version
    files_dir = release_root / "files"
    files_dir.mkdir(parents=True, exist_ok=True)

    artifacts = collect_artifacts(files_dir)
    artifacts.sort(key=lambda item: str(item["name"]))

    manifest = {
        "project": "xv6-esp",
        "version": version,
        "commit": commit,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "build_dir": str(BUILD_DIR.relative_to(ROOT)),
        "git_dirty": bool(dirty),
        "host": {
            "platform": os.uname().sysname,
            "release": os.uname().release,
            "machine": os.uname().machine,
        },
        "artifacts": artifacts,
    }
    manifest_path = release_root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")

    sha_path = release_root / "SHA256SUMS"
    write_sha256sums(files_dir, artifacts, sha_path)

    tar_path = make_tarball(version, release_root)

    print(f"release bundle ready: {release_root}")
    print(f"manifest: {manifest_path}")
    print(f"checksums: {sha_path}")
    print(f"tarball: {tar_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
