"""Check that every HIP device image in a binary was compiled for the target its bundle entry names.

    python verify_hip_targets.py BINARY [--expect gfx1100,gfx1101,...]

Scans an executable, object or archive for clang offload bundles. Each AMD entry carries a target label
(hipv4-amdgcn-amd-amdhsa--gfx1102) and an ELF code object whose e_flags record the processor it was
compiled for. A label/code mismatch means a GPU of that family receives another family's machine code.
"""
import argparse
import struct
import sys

BUNDLE_MAGIC = b"__CLANG_OFFLOAD_BUNDLE__"
COMPRESSED_MAGIC = b"CCOB"
EM_AMDGPU = 224
EF_AMDGPU_MACH = 0x0FF
MACH_BY_TARGET = {
    "gfx1100": 0x041,
    "gfx1101": 0x046,
    "gfx1102": 0x047,
    "gfx1200": 0x048,
    "gfx1201": 0x04E,
}
TARGET_BY_MACH = {mach: target for target, mach in MACH_BY_TARGET.items()}


def bundles(data):
    position = 0
    while True:
        start = data.find(BUNDLE_MAGIC, position)
        if start < 0:
            return
        position = start + len(BUNDLE_MAGIC)
        if start + 32 > len(data):
            continue
        count = struct.unpack_from("<Q", data, start + 24)[0]
        if count == 0 or count > 64:
            continue  # the magic string itself, e.g. inside the runtime
        cursor = start + 32
        entries = []
        for _ in range(count):
            offset, size, label_size = struct.unpack_from("<QQQ", data, cursor)
            cursor += 24
            label = data[cursor:cursor + label_size].decode("ascii", "replace")
            cursor += label_size
            entries.append((label, data[start + offset:start + offset + size]))
        yield start, entries


def compiled_mach(image):
    if len(image) < 52 or image[:4] != b"\x7fELF" or image[4] != 2:
        return None
    machine = struct.unpack_from("<H", image, 18)[0]
    if machine != EM_AMDGPU:
        return None
    return struct.unpack_from("<I", image, 48)[0] & EF_AMDGPU_MACH


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--expect", default="", help="comma-separated gfx targets every bundle must contain")
    args = parser.parse_args()
    expected = {target for target in args.expect.split(",") if target}
    data = open(args.binary, "rb").read()

    failures = []
    found = 0
    for start, entries in bundles(data):
        labels = set()
        for label, image in entries:
            if "amdgcn" not in label:
                continue
            target = label.rsplit("--", 1)[-1].split(":", 1)[0]
            labels.add(target)
            mach = compiled_mach(image)
            actual = TARGET_BY_MACH.get(mach, "unknown" if mach is None else hex(mach))
            status = "ok" if mach is not None and mach == MACH_BY_TARGET.get(target) else "MISMATCH"
            print(f"bundle@{start:#x} {target}: code object for {actual} ({len(image)} bytes) {status}")
            if status != "ok":
                failures.append(f"bundle@{start:#x} {target} holds {actual} code")
        if labels:
            found += 1
            if expected and labels != expected:
                failures.append(f"bundle@{start:#x} targets {sorted(labels)} != {sorted(expected)}")
    if found == 0:
        if COMPRESSED_MAGIC in data:
            failures.append("only compressed offload bundles found; this check needs uncompressed bundles")
        else:
            failures.append("no AMD offload bundles found")
    for failure in failures:
        print("FAIL", failure)
    if not failures:
        print(f"PASS {found} HIP bundle(s): every device image matches its target")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
