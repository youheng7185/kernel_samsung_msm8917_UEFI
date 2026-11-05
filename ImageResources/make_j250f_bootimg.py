#!/usr/bin/env python3
"""
make_j250f_bootimg.py
Generates a Samsung Galaxy J2 Pro 2018 (SM-J250F / j2y18lte) compliant Android boot image
containing an ARM BootShim + EDK2 UEFI FD + Samsung QCDT device tree table.
"""

import os, sys, struct, hashlib

def build_boot_img(kernel_payload_path, qcdt_path, output_path, ramdisk_path=None, add_seandroid=True):
    page_size = 2048
    def pad(x): return (x + page_size - 1) // page_size * page_size
    def pad_bytes(b):
        p = pad(len(b))
        return b + b'\x00' * (p - len(b))

    with open(kernel_payload_path, 'rb') as f:
        kernel_data = f.read()

    ramdisk_data = b''
    if ramdisk_path and os.path.exists(ramdisk_path):
        with open(ramdisk_path, 'rb') as f:
            ramdisk_data = f.read()

    with open(qcdt_path, 'rb') as f:
        qcdt_data = f.read()

    kernel_size = len(kernel_data)
    ramdisk_size = len(ramdisk_data)
    second_size = 0
    dt_size = len(qcdt_data)

    # Hardware verified Samsung J250F addresses
    kernel_addr  = 0x80008000
    ramdisk_addr = 0x82000000
    second_addr  = 0x00000000
    tags_addr    = 0x81e00000
    os_version   = 0x18000181 # Android 8.0/9.0 patch level
    name         = b''
    cmdline      = b'console=null androidboot.hardware=qcom msm_rtb.filter=0x237 ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci lpm_levels.sleep_disabled=1 buildvariant=userdebug'

    # Compute SHA1 of kernel, ramdisk, second, dt
    sha = hashlib.sha1()
    sha.update(kernel_data)
    sha.update(struct.pack('<I', kernel_size))
    sha.update(ramdisk_data)
    sha.update(struct.pack('<I', ramdisk_size))
    if second_size > 0:
        sha.update(struct.pack('<I', second_size))
    else:
        sha.update(struct.pack('<I', 0))
    sha.update(qcdt_data)
    sha.update(struct.pack('<I', dt_size))
    img_id = sha.digest().ljust(32, b'\x00')

    # Construct v0 boot header with Qualcomm dt_size extension at offset 40
    header = struct.pack(
        '<8s10I16s512s32s',
        b'ANDROID!',
        kernel_size,
        kernel_addr,
        ramdisk_size,
        ramdisk_addr,
        second_size,
        second_addr,
        tags_addr,
        page_size,
        dt_size,
        os_version,
        name.ljust(16, b'\x00'),
        cmdline.ljust(512, b'\x00'),
        img_id
    )
    # Pad header to 1 page (2048 bytes)
    header = header.ljust(page_size, b'\x00')

    # Assemble full boot image
    padded_kernel  = pad_bytes(kernel_data)
    padded_ramdisk = pad_bytes(ramdisk_data) if ramdisk_size > 0 else b''
    padded_qcdt    = pad_bytes(qcdt_data)

    boot_img = header + padded_kernel + padded_ramdisk + padded_qcdt

    if add_seandroid:
        # Append SEANDROIDENFORCE footer for Samsung bootloader compatibility
        boot_img += b'SEANDROIDENFORCE\x00\x00\x00\x00'

    # Check 32 MiB partition constraint
    max_partition_size = 32 * 1024 * 1024
    if len(boot_img) > max_partition_size:
        raise ValueError(f"Boot image size ({len(boot_img)} bytes) exceeds 32 MiB partition limit!")

    with open(output_path, 'wb') as f:
        f.write(boot_img)

    sha256 = hashlib.sha256(boot_img).hexdigest()

    print(f"=======================================================")
    print(f" SUCCESS: Samsung J250F Boot Image Generated")
    print(f"=======================================================")
    print(f" Output File:       {output_path}")
    print(f" Total Image Size:  {len(boot_img)} bytes ({len(boot_img)/(1024*1024):.2f} MiB)")
    print(f" Kernel Payload:    {kernel_size} bytes (padded: {len(padded_kernel)})")
    print(f" QCDT Table Size:   {dt_size} bytes (padded: {len(padded_qcdt)})")
    print(f" Partition Limit:   {max_partition_size} bytes (32.00 MiB)")
    print(f" Free Space in Boot:{max_partition_size - len(boot_img)} bytes")
    print(f" SHA-256 Checksum:  {sha256}")
    print(f"=======================================================")
    return output_path, len(boot_img), sha256

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <kernel_payload> <qcdt_file> <output_boot.img> [ramdisk_file]")
        sys.exit(1)
    k_path = sys.argv[1]
    dt_path = sys.argv[2]
    out_path = sys.argv[3]
    r_path = sys.argv[4] if len(sys.argv) > 4 else None
    build_boot_img(k_path, dt_path, out_path, r_path)
