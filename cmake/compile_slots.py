#!/usr/bin/env python3
"""Runs a compile when one of SLOTS slots is free (a compiler launcher):
compile_slots.py LOCK_DIR SLOTS COMMAND...

The GPU module's CUDA files each compile every GPU generation at once, a few
GB of memory per generation; ninja would start as many as it has jobs. The
slots are lock files, released by the system even when the compile dies."""

import os
import subprocess
import sys
import time

if os.name == "nt":
    import msvcrt

    def try_lock(handle):
        try:
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False
else:
    import fcntl

    def try_lock(handle):
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False


def main():
    lock_dir, slots, command = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    os.makedirs(lock_dir, exist_ok=True)
    while True:
        for slot in range(slots):
            handle = open(os.path.join(lock_dir, f"slot{slot}"), "a+b")
            handle.seek(0)
            if try_lock(handle):
                # held until this process ends
                return subprocess.call(command)
            handle.close()
        time.sleep(0.25)


if __name__ == "__main__":
    sys.exit(main())
