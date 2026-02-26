#!/usr/bin/env python3
import os
import sys

from qemu_ci import cli


def main() -> int:
    argv = ["--suite", "regressions"]
    if os.environ.get("XV6_SKIP_BUILD") == "1":
        argv.append("--skip-build")
    return cli(argv)


if __name__ == "__main__":
    sys.exit(main())
