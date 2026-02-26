#!/usr/bin/env python3
from __future__ import annotations

import os
import select
import shlex
import signal
import subprocess
import time
import tty
from pathlib import Path


class PtySerial:
    def __init__(self, fd: int) -> None:
        self._fd = fd
        self._timeout: float | None = None
        self._closed = False

    def settimeout(self, timeout: float | None) -> None:
        self._timeout = timeout

    def sendall(self, data: bytes) -> None:
        if self._closed:
            raise OSError("serial endpoint is closed")
        view = memoryview(data)
        sent = 0
        deadline: float | None = None
        if self._timeout is not None:
            deadline = time.time() + self._timeout
        while sent < len(data):
            wait_s: float | None = None
            if deadline is not None:
                wait_s = max(0.0, deadline - time.time())
            _, writable, _ = select.select([], [self._fd], [], wait_s)
            if not writable:
                raise TimeoutError()
            n = os.write(self._fd, view[sent:])
            if n <= 0:
                raise OSError("failed to write to qemu tty")
            sent += n

    def recv(self, size: int) -> bytes:
        if self._closed:
            return b""
        ready, _, _ = select.select([self._fd], [], [], self._timeout)
        if not ready:
            raise TimeoutError()
        try:
            return os.read(self._fd, size)
        except OSError:
            return b""

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            os.close(self._fd)
        except OSError:
            pass


def launch_idf_qemu(root: Path, idf_export: str, flash: Path, efuse: Path) -> tuple[subprocess.Popen, PtySerial]:
    master_fd, slave_fd = os.openpty()
    tty.setraw(slave_fd)
    cmd = (
        f"{idf_export} && "
        "idf.py qemu "
        f"--flash-file {shlex.quote(str(flash))} "
        f"--efuse-file {shlex.quote(str(efuse))}"
    )
    proc = subprocess.Popen(  # noqa: S603
        ["bash", "-lc", cmd],
        cwd=root,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        preexec_fn=os.setsid,
        close_fds=True,
    )
    os.close(slave_fd)
    serial = PtySerial(master_fd)
    return proc, serial


def stop_idf_qemu(proc: subprocess.Popen, serial: PtySerial | None = None) -> None:
    if serial is not None:
        serial.close()

    if proc.poll() is not None:
        return

    pgid = None
    try:
        pgid = os.getpgid(proc.pid)
    except ProcessLookupError:
        return

    if pgid is not None:
        try:
            os.killpg(pgid, signal.SIGTERM)
        except ProcessLookupError:
            return

    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass

    if pgid is not None:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            return

    proc.wait(timeout=5)
