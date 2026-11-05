/* J250F ARM64 Android Image header and EDK2 relocation shim. */
.text
.global _start
_start:
  b     _relocate
  .long 0
  .quad 0x80000                 /* text_offset: matches the working ARM64 wrapper */
  .quad UEFI_SIZE + 0x100       /* image_size: shim plus firmware payload */
  .quad 0                     /* flags: little endian, 4 KiB pages */
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0

_relocate:
  /* Preserve x0 (bootloader DTB pointer) for the firmware entry. */
  adr   x4, _payload
  ldr   x5, _uefi_base
  cmp   x4, x5
  b.eq  _entry
  ldr   x6, _uefi_size
  mov   x7, x5

_copy:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x7], #16
  subs  x6, x6, #16
  b.ne  _copy

  /* The payload was copied through the data cache. Publish it to I-cache. */
  mov   x7, x5
  ldr   x6, _uefi_size
_clean:
  dc    cvac, x7
  add   x7, x7, #64
  subs  x6, x6, #64
  b.gt  _clean
  dsb   sy
  ic    iallu
  dsb   sy
  isb

_entry:
  br    x5

.balign 8
_uefi_base:
  .quad UEFI_BASE
_uefi_size:
  .quad UEFI_SIZE

.org 0x100
_payload:
