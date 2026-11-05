"""Validate the packaged image against this build's complete firmware payload."""
from pathlib import Path
import struct
import sys

image = Path(sys.argv[1]).read_bytes()
mode = sys.argv[2]
arch = sys.argv[3]
if mode not in ("DEBUG", "RELEASE"):
    raise ValueError("Expected DEBUG or RELEASE")
if arch not in ("ARM", "AARCH64"):
    raise ValueError("Expected ARM or AARCH64")
build = Path(f"Build/J250FPkg-{arch}/{mode}_CLANG38")
shim = Path(f"BootShim/{arch}/BootShim.bin").read_bytes()
fd = (build / "FV/J250F_UEFI.fd").read_bytes()
qcdt = Path("ImageResources/DTBs/J250F.qcdt").read_bytes()
header = struct.unpack_from("<8s10I", image)
assert header[0] == b"ANDROID!"
assert header[1] == len(shim) + len(fd)
assert header[2] == 0x80008000 and header[8] == 2048
assert header[3] == header[5] == 0
assert header[9] == len(qcdt)
if arch == "ARM":
    assert struct.unpack_from("<I", shim, 0x20)[0] == 0xEA000002
    assert struct.unpack_from("<I", shim, 0x2C)[0] == header[1]
else:
    assert len(shim) == 0x100
    assert shim[0x38:0x3c] == b"ARM\x64"
    assert struct.unpack_from("<Q", shim, 8)[0] == 0x80000
    assert struct.unpack_from("<Q", shim, 16)[0] == header[1]
assert image[2048 : 2048 + header[1]] == shim + fd
dt_start = (2048 + header[1] + 2047) // 2048 * 2048
assert image[dt_start : dt_start + len(qcdt)] == qcdt
assert image.endswith(b"SEANDROIDENFORCE" + bytes(4))
assert len(image) <= 32 * 1024 * 1024

# Check the built security module, not just the configured preprocessor flag.
modules = list(build.glob(f"{arch}/MdeModulePkg/Universal/SecurityStubDxe/**/OUTPUT/SecurityStubDxe.efi"))
assert len(modules) == 1
module = modules[0].read_bytes()
pe_offset = struct.unpack_from("<I", module, 0x3c)[0]
assert module[pe_offset:pe_offset + 4] == b"PE\0\0"
assert struct.unpack_from("<H", module, pe_offset + 4)[0] == (0xAA64 if arch == "AARCH64" else 0x01C2)
marker = b"J250F ADB: allowing unsigned RAM EFI image"
assert (marker in module) == (mode == "DEBUG")
print(f"Validated {arch} {mode}: header, entry, complete payload, QCDT, footer and RAM EFI policy")
