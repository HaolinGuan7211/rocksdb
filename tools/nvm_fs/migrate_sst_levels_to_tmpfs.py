#!/usr/bin/env python3
"""
Copy live SST files at selected levels into SimulatedHybridFileSystem tmpfs root.

Why this exists:
  - `SimulatedHybridFileSystem` supports path redirection via
    `--simulate_xp_redirect_to_tmpfs=1 --simulate_xp_tmpfs_root=...`.
  - Redirection only changes the physical backing path, it does not "move" files.
  - This helper copies the current *live* SSTs for specific levels (e.g. L0-L4)
    to tmpfs so that subsequent db_bench runs will actually open the SSTs from
    RAM-backed storage.

Important:
  - This script only COPIES files. It never deletes or moves the original DB.
  - Destination path follows the simulator rule:
        tmpfs_root + absolute_path
    Example:
        /tmp/mydb/db/000123.sst  ->  /dev/shm/tmpfs/tmp/mydb/db/000123.sst
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Set, Tuple


_LIVE_FILE_RE = re.compile(r"^(?P<path>.+?)\s+: level\s+(?P<level>\d+),")


@dataclass(frozen=True)
class LiveFile:
  path: str
  level: int


def _default_rocksdb_root() -> Path:
  # .../rocksdb/tools/nvm_fs/this_script.py -> parents[2] == .../rocksdb
  return Path(__file__).resolve().parents[2]


def _default_ldb_path() -> Path:
  root = _default_rocksdb_root()
  candidates = [
    root / "build_nvm" / "tools" / "ldb",
    root / "build" / "tools" / "ldb",
  ]
  for cand in candidates:
    if cand.is_file() and os.access(cand, os.X_OK):
      return cand
  return candidates[0]


def _parse_levels_csv(csv: str) -> Set[int]:
  out: Set[int] = set()
  for raw in csv.split(","):
    token = raw.strip()
    if not token:
      continue
    if token[0] in ("l", "L"):
      token = token[1:]
    try:
      lvl = int(token)
    except ValueError as exc:
      raise ValueError(f"invalid level token: {raw!r}") from exc
    if lvl < 0:
      raise ValueError(f"invalid level (negative): {raw!r}")
    out.add(lvl)
  if not out:
    raise ValueError("empty --levels")
  return out


def _run_ldb_list_live_files(ldb: Path, db: str) -> str:
  cmd = [str(ldb), f"--db={db}", "list_live_files_metadata", "--sort_by_filename"]
  proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
  if proc.returncode != 0:
    raise RuntimeError(
      "ldb failed (exit=%d)\ncmd: %s\noutput:\n%s"
      % (proc.returncode, " ".join(cmd), proc.stdout)
    )
  return proc.stdout


def _parse_live_files(output: str) -> List[LiveFile]:
  files: List[LiveFile] = []
  for line in output.splitlines():
    m = _LIVE_FILE_RE.match(line.strip())
    if not m:
      continue
    files.append(LiveFile(path=m.group("path"), level=int(m.group("level"))))
  return files


def _to_tmpfs_path(tmpfs_root: str, src_path: str) -> str:
  tmpfs_root = tmpfs_root.rstrip("/") or tmpfs_root
  if not tmpfs_root:
    raise ValueError("empty tmpfs_root")
  if src_path.startswith("/"):
    return tmpfs_root + src_path
  return tmpfs_root + "/" + src_path


def _bytes_to_gib(n: int) -> float:
  return float(n) / (1024.0**3)


def _statvfs_free_bytes(path: str) -> Optional[int]:
  try:
    st = os.statvfs(path)
  except OSError:
    return None
  return int(st.f_bavail) * int(st.f_frsize)


def _copy_one(src: str, dst: str, dry_run: bool) -> None:
  os.makedirs(os.path.dirname(dst), exist_ok=True)
  if dry_run:
    return
  shutil.copy2(src, dst)
  src_sz = os.path.getsize(src)
  dst_sz = os.path.getsize(dst)
  if src_sz != dst_sz:
    raise RuntimeError(f"size mismatch after copy: {src} ({src_sz}) -> {dst} ({dst_sz})")


def main(argv: Sequence[str]) -> int:
  ap = argparse.ArgumentParser()
  ap.add_argument("--db", required=True, help="RocksDB DB directory (path to .../db).")
  ap.add_argument("--levels", required=True, help="CSV levels, e.g. 0,1,2,3,4")
  ap.add_argument(
    "--tmpfs_root",
    default="/dev/shm/tmpfs",
    help="Tmpfs root used by --simulate_xp_tmpfs_root (default: /dev/shm/tmpfs).",
  )
  ap.add_argument(
    "--ldb",
    default=str(_default_ldb_path()),
    help="Path to rocksdb ldb tool (default: build_nvm/tools/ldb if present).",
  )
  ap.add_argument("--dry_run", action="store_true", help="Only print what would be copied.")
  args = ap.parse_args(argv[1:])

  db = os.path.abspath(args.db)
  ldb = Path(args.ldb)
  levels = _parse_levels_csv(args.levels)
  tmpfs_root = args.tmpfs_root

  if not os.path.isdir(db):
    print(f"error: --db is not a directory: {db}", file=sys.stderr)
    return 2
  if not (ldb.is_file() and os.access(ldb, os.X_OK)):
    print(f"error: --ldb not found/executable: {ldb}", file=sys.stderr)
    return 2

  os.makedirs(tmpfs_root, exist_ok=True)

  output = _run_ldb_list_live_files(ldb, db)
  live_files = _parse_live_files(output)
  if not live_files:
    print("error: no live files parsed from ldb output; is this a valid DB?", file=sys.stderr)
    return 2

  selected = [f for f in live_files if f.level in levels and f.path.endswith(".sst")]
  if not selected:
    print(f"error: no live SSTs found for levels={sorted(levels)}", file=sys.stderr)
    return 2

  total_bytes = 0
  missing: List[str] = []
  for f in selected:
    if not os.path.isfile(f.path):
      missing.append(f.path)
      continue
    total_bytes += os.path.getsize(f.path)
  if missing:
    print("error: some live SSTs are missing on disk:", file=sys.stderr)
    for p in missing:
      print(f"  - {p}", file=sys.stderr)
    return 2

  free_bytes = _statvfs_free_bytes(tmpfs_root)
  if free_bytes is not None and total_bytes > free_bytes:
    print(
      "warning: tmpfs_root free space may be insufficient:\n"
      f"  need={_bytes_to_gib(total_bytes):.2f} GiB\n"
      f"  free={_bytes_to_gib(free_bytes):.2f} GiB\n"
      f"  tmpfs_root={tmpfs_root}",
      file=sys.stderr,
    )

  print(f"db={db}")
  print(f"levels={sorted(levels)}")
  print(f"tmpfs_root={tmpfs_root}")
  print(f"ldb={ldb}")
  print(f"live_sst_selected={len(selected)} total={_bytes_to_gib(total_bytes):.2f} GiB")

  for idx, f in enumerate(sorted(selected, key=lambda x: (x.level, x.path))):
    dst = _to_tmpfs_path(tmpfs_root, f.path)
    sz = os.path.getsize(f.path)
    print(f"[{idx+1:4d}/{len(selected):4d}] L{f.level} {_bytes_to_gib(sz):7.3f} GiB  {f.path} -> {dst}")
    _copy_one(f.path, dst, args.dry_run)

  print("done.")
  print("next: run db_bench with:")
  print(
    "  --simulate_xp_redirect_to_tmpfs=1 "
    f"--simulate_xp_tmpfs_root={tmpfs_root} "
    f"--simulate_xp_levels={args.levels}"
  )
  return 0


if __name__ == "__main__":
  raise SystemExit(main(sys.argv))
