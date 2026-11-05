## @file
#
#  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2016, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) 2026, Antigravity low-level ARM firmware engineering.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = J250F
  PLATFORM_GUID                  = 8917250f-c68a-46a6-9c8b-4cee7fc0ff0b
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/J250FPkg-$(ARCH)
  SUPPORTED_ARCHITECTURES        = ARM|AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = J250FPkg/J250F.fdf
  DISPLAY_USES_RGBA              = 1
  USE_DISPLAYDXE                 = 0
  AB_SLOT_SUPPORT                = 0
  USE_UART                       = 0
  # Mu's compositor consumes/hides the real GOP and assumes a linear 32-bit
  # framebuffer. This platform still falls back to PixelBltOnly, so keep its
  # real GOP available for GraphicsConsole rather than publishing a duplicate.
  USE_MS_GOP_OVERRIDE             = 0

[BuildOptions.common]
  GCC:*_*_*_CC_FLAGS = -DJ250F_PLATFORM=1
!if $(TARGET) == DEBUG && $(USE_UART) == 0
  GCC:DEBUG_*_*_CC_FLAGS = -DJ250F_PERSISTENT_TEXT_LOG=1
!endif
  *_*_*_CC_FLAGS = -DDISPLAY_USES_RGBA=$(DISPLAY_USES_RGBA)
  # ADB RAM applications are explicitly permitted in this development build.
  # RELEASE and image loads with a device path retain normal verification.
  DEBUG_*_*_CC_FLAGS = -DJ250F_ADB_RAM_EXECUTION=1

[LibraryClasses.common]
  J250fGopSupportLib|J250fPkg/J250fGopDxe/J250fGopSupportLib.inf
  J250fSdccPlatformLib|GPLDrivers/Library/J250fSdccPlatformLib/J250fSdccPlatformLib.inf
  PlatformMemoryMapLib|J250FPkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  KeypadDeviceImplLib|J250FPkg/Library/KeypadDeviceImplLib/KeypadDeviceImplLib.inf

[PcdsFixedAtBuild.common]
  # Platform-specific memory definitions
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000        # Starting DRAM address
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x60000000        # 1.5 GB Total Physical RAM

  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"Antigravity & Renegade"

  # Safe firmware execution coordinates in High DRAM (Zone 4)
  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x99300000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x99310000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00080000      # 512K stack

  # SMBIOS configuration (Samsung Galaxy J2 Pro 2018 SM-J250F)
  gQcomPkgTokenSpaceGuid.PcdSmbiosSystemVendor|"Samsung Electronics Co., Ltd"
  gQcomPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Galaxy J2 Pro (2018)"
  gQcomPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"SM-J250F"
  gQcomPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"j2y18lte"
  gQcomPkgTokenSpaceGuid.PcdSmbiosBoardModel|"j2y18lte"

  # Simple FrameBuffer (Samsung 540x960 qHD display panel, 24bpp RGB888 native unpadded, base 0xA800035A)
  gQcomPkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x00000000A800035A
  gQcomPkgTokenSpaceGuid.PcdMipiFrameBufferWidth|540
  gQcomPkgTokenSpaceGuid.PcdMipiFrameBufferHeight|960
  gQcomPkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|24
  gQcomPkgTokenSpaceGuid.PcdMipiFrameBufferPixelsPerScanLine|540

  # Suppress excessive INFO debug spam so startup banner remains visible
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002

[Components.common]
  J250fPkg/AdbDebug/Hello/J250fAdbHello.inf
  J250fPkg/PlatformIntegration/J250fSdcc2Dxe.inf
  J250fPkg/J250fGopDxe/J250fGopDxe.inf
  # Build only, not included in the phone firmware image.
  J250fPkg/J250fGopDxe/J250fGopTest.inf
  # Keypad Drivers
  QcomPkg/Drivers/KeypadDxe/KeypadDxe.inf
  QcomPkg/Drivers/GenericKeypadDeviceDxe/GenericKeypadDeviceDxe.inf

[PcdsDynamicDefault.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|540
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|960
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|540
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|960
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|67
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|50
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|67
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|50

!include MSM8917Pkg/MSM8917Pkg.dsc.inc

# Keep SEC on the explicitly attached queue; only DXE can discover HOBs.
# Runtime drivers deliberately excluded: this buffer dies at ExitBootServices.
!if $(TARGET) == DEBUG && $(USE_UART) == 0
[LibraryClasses.common.DXE_CORE, LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  SerialPortLib|QcomPkg/Library/FrameBufferSerialPortLib/FrameBufferSerialPortLogDxe.inf
!endif
