"""Generate a bounded-memory UTF-8 log fixture. Default 100 MiB; never load it all."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("path", type=Path)
parser.add_argument("--mib", type=int, default=100)
args = parser.parse_args()
if args.mib < 1 or args.mib > 4096:
    parser.error("--mib must be between 1 and 4096")
line = ("2026-10-28 12:34:56.789 [INFO] PortWave 中文 UTF-8 benchmark 0123456789\r\n" * 1000).encode()
remaining = args.mib * 1024 * 1024
args.path.parent.mkdir(parents=True, exist_ok=True)
with args.path.open("xb") as output:
    while remaining:
        chunk = line[:remaining]
        output.write(chunk)
        remaining -= len(chunk)
print(f"Created {args.path}: {args.mib} MiB")
