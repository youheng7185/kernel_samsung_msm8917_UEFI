// SPDX-License-Identifier: GPL-2.0-only
// J250F SDCC2 removable SD card, conservative 400 kHz, one-bit, read-only PIO.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <J250fSdcc2Status.h>
#include "RpmSmd.h"
#include "SmemMap.h"

#define HC     0x07864900U
#define CORE   0x07864000U
#define GCC    0x01800000U
#define TLMM   0x01000000U
#define BLKSZ  512U

#define REG_BLOCK_SIZE 0x04U
#define REG_BLOCK_COUNT 0x06U
#define REG_ARGUMENT 0x08U
#define REG_TRANSFER 0x0cU
#define REG_COMMAND 0x0eU
#define REG_RESPONSE 0x10U
#define REG_BUFFER 0x20U
#define REG_PRESENT 0x24U
#define REG_POWER 0x29U
#define REG_CLOCK 0x2cU
#define REG_TIMEOUT 0x2eU
#define REG_RESET 0x2fU
#define REG_INTERRUPT 0x30U
#define REG_INT_ENABLE 0x34U
#define REG_SIGNAL_ENABLE 0x38U
#define REG_HOST_CONTROL2 0x3eU
#define REG_VENDOR 0x10cU
#define CORE_MODE 0x78U
#define CORE_PWR_STATUS 0xdcU
#define CORE_PWR_MASK 0xe0U
#define CORE_PWR_CLEAR 0xe4U
#define CORE_PWR_CTL 0xe8U

#define INT_CMD 1U
#define INT_TRANSFER 2U
#define INT_READ_READY 0x20U
#define INT_ERROR_MASK 0xffff8000U
#define PRESENT_DATA (1U << 11)
#define RESP_NONE 0U
#define RESP_48 2U
#define RESP_136 1U
#define RESP_BUSY 3U
#define CMD_CRC 8U
#define CMD_INDEX 16U
#define CMD_DATA 32U

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} J250F_SD_VENDOR_PATH;

STATIC EFI_GUID mSdGuid = {0x27863253,0x2d92,0x489b,{0xab,0x78,0xd3,0x37,0x71,0x4d,0xcf,0x21}};
STATIC EFI_HANDLE mHandle;
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo;
STATIC J250F_SD_VENDOR_PATH mPath;
STATIC UINT16 mRca;
STATIC BOOLEAN mReady;
STATIC UINT32 mStage;
STATIC UINT32 mLastInterrupt;
STATIC UINT32 mLastResponse;
STATIC UINT32 mLastPresent;
STATIC UINT32 mCmd55Recovered;
STATIC UINT32 mAcmd41Recovered;
STATIC UINT32 mCmd2Retries;
STATIC UINT32 mCmd9Retries;
STATIC UINT32 mCmd9Recovered;
STATIC UINT32 mCmd3Recovered;
STATIC EFI_GUID mStatusGuid = J250F_SDCC2_STATUS_GUID;

STATIC VOID
ReportStatus (EFI_STATUS Result)
{
  J250F_SDCC2_STATUS Record = {
    mStage, 0, (UINT64)Result, mLastInterrupt, mLastResponse,
    mLastPresent, MmioRead16 (HC + REG_CLOCK),
    MmioRead32 (CORE + CORE_PWR_STATUS),
    MmioRead32 (HC + REG_INT_ENABLE), MmioRead16 (HC + REG_COMMAND),
    mCmd55Recovered, mAcmd41Recovered, mCmd2Retries, mCmd9Retries,
    mCmd9Recovered, mCmd3Recovered,
    MmioRead32 (HC + REG_RESPONSE + 4),
    MmioRead32 (HC + REG_RESPONSE + 8),
    MmioRead32 (HC + REG_RESPONSE + 12)
  };
  (VOID)gRT->SetVariable (L"J250fSdcc2Status", &mStatusGuid,
          EFI_VARIABLE_BOOTSERVICE_ACCESS, sizeof (Record), &Record);
}

STATIC UINT32 Read32 (UINTN Base, UINTN Offset) { return MmioRead32 (Base + Offset); }
STATIC VOID Write32 (UINTN Base, UINTN Offset, UINT32 Value) { MmioWrite32 (Base + Offset, Value); MemoryFence (); }

STATIC EFI_STATUS
Wait32 (UINTN Base, UINTN Offset, UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  UINTN Elapsed;
  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    if ((Read32 (Base, Offset) & Mask) == Expected) return EFI_SUCCESS;
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
VoteRail (UINT32 Id, UINT32 Microvolts, UINT32 Load)
{
  STATIC CONST UINT32 Keys[] = {0x6e657773U, 0x00007675U, 0x0000616dU};
  UINT32 Values[3] = {1, Microvolts, Load};
  return J250fRpmVote (0x616f646cU, Id, Keys, Values, 3);
}

STATIC EFI_STATUS
PowerUp (VOID)
{
  STATIC CONST UINT32 XoKey = 0x62616e45U, XoEnable = 1;
  EFI_STATUS Status = J250fMapRpmSmem ();
  if (EFI_ERROR (Status)) return Status;
  Status = J250fRpmOpen ();
  if (EFI_ERROR (Status)) return Status;
  Status = J250fRpmVote (0x306b6c63U, 0, &XoKey, &XoEnable, 1);
  if (EFI_ERROR (Status)) return Status;
  // J250F DTS: SDCC2 vdd=L11 2.95V, vdd-io=L12 1.8V..2.95V.
  Status = VoteRail (11, 2950000, 50000);
  if (EFI_ERROR (Status)) return Status;
  return VoteRail (12, 2950000, 22000);
}

STATIC EFI_STATUS
SetPins (VOID)
{
  UINT32 Value = Read32 (TLMM, 0x109000);
  // pinctrl-msm8917: SDC2 CLK pull[15:14] drive[8:6], CMD pull[12:11]
  // drive[5:3], DATA pull[10:9] drive[2:0]. Active DTS: 12/8/8 mA.
  Value &= ~((3U << 14) | (7U << 6) | (3U << 11) | (7U << 3) |
             (3U << 9) | 7U);
  Value |= (5U << 6) | (3U << 11) | (3U << 3) |
           (3U << 9) | 3U;
  Write32 (TLMM, 0x109000, Value);
  if (Read32 (TLMM, 0x109000) != Value) return EFI_DEVICE_ERROR;
  // DTS cd-gpios = GPIO67 active low. GPIO function, input, no pull.
  Write32 (TLMM, 67U * 0x1000U, 0);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SetClock400k (VOID)
{
  UINT32 Value;
  // GCC-8952 SDCC2 RCG offsets; XO 19.2 MHz / 12 * 1/4 = 400 kHz.
  Write32 (GCC, 0x43018, Read32 (GCC, 0x43018) & ~1U);
  Write32 (GCC, 0x4300c, 1);
  Write32 (GCC, 0x43010, ~(4U - 1U));
  Write32 (GCC, 0x43014, ~4U);
  Value = Read32 (GCC, 0x43008);
  Value = (Value & ~((31U << 0) | (7U << 8) | (3U << 12))) |
          23U | (2U << 12);
  Write32 (GCC, 0x43008, Value);
  Write32 (GCC, 0x43004, Read32 (GCC, 0x43004) | 1U);
  if (EFI_ERROR (Wait32 (GCC, 0x43004, 1, 0, 5000))) return EFI_TIMEOUT;
  Write32 (GCC, 0x4301c, Read32 (GCC, 0x4301c) | 1U);
  Write32 (GCC, 0x43018, Read32 (GCC, 0x43018) | 1U);
  if (EFI_ERROR (Wait32 (GCC, 0x4301c, 1U << 31, 0, 5000)) ||
      EFI_ERROR (Wait32 (GCC, 0x43018, 1U << 31, 0, 5000))) return EFI_TIMEOUT;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ServicePower (VOID)
{
  UINT32 Pending = Read32 (CORE, CORE_PWR_STATUS);
  UINT32 Ack = 0;
  if (Pending == 0) return EFI_SUCCESS;
  Write32 (CORE, CORE_PWR_CLEAR, Pending);
  if (Pending & 3U) Ack |= 1U;
  if (Pending & 12U) Ack |= 4U;
  Write32 (CORE, CORE_PWR_CTL, Ack);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
WaitPresent (UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  UINTN Elapsed;
  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    ServicePower ();
    if ((Read32 (HC, REG_PRESENT) & Mask) == Expected) return EFI_SUCCESS;
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
ResetCommandLine (VOID)
{
  UINTN Elapsed;
  MmioWrite8 (HC + REG_RESET, 2U); // SDHCI software reset: command circuit
  MemoryFence ();
  for (Elapsed = 0; Elapsed < 100000; Elapsed += 10) {
    ServicePower ();
    if (!(MmioRead8 (HC + REG_RESET) & 2U)) return EFI_SUCCESS;
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Command (UINT8 Index, UINT32 Argument, UINT16 Flags, UINT32 *Response,
         VOID *Data)
{
  EFI_STATUS Status;
  UINT32 Interrupt = 0;
  UINT32 BeforeResponse;
  BOOLEAN RecoveredResponse = FALSE;
  UINTN I;
  UINT32 *Words = Data;
  mStage = 0x100U + Index;
  mLastInterrupt = 0;
  mLastResponse = 0;
  mLastPresent = Read32 (HC, REG_PRESENT);
  Status = WaitPresent (3U, 0, 1000000);
  if (EFI_ERROR (Status)) return Status;
  Write32 (HC, REG_INTERRUPT, 0xffffffffU);
  BeforeResponse = Read32 (HC, REG_RESPONSE);
  if (Data != NULL) {
    MmioWrite16 (HC + REG_BLOCK_SIZE, BLKSZ);
    MmioWrite16 (HC + REG_BLOCK_COUNT, 1);
    MmioWrite16 (HC + REG_TRANSFER, 0x12U); // block count + read
  }
  Write32 (HC, REG_ARGUMENT, Argument);
  MmioWrite16 (HC + REG_COMMAND, (UINT16)((Index << 8) | Flags |
                                 (Data != NULL ? CMD_DATA : 0)));
  MemoryFence ();
  for (I = 0; I < (Index == 41 ? 2000U : 100000U); ++I) {
    ServicePower ();
    Interrupt = Read32 (HC, REG_INTERRUPT);
    if (Interrupt & (INT_CMD | INT_ERROR_MASK)) break;
    MicroSecondDelay (10);
  }
  mLastInterrupt = Interrupt;
  mLastPresent = Read32 (HC, REG_PRESENT);
  mLastResponse = Read32 (HC, REG_RESPONSE);
  // SDCC2 has returned valid CMD55 R1 and ACMD41 R3 responses with IRQ=0.
  // Recover only a fresh response with the expected bits and an idle command
  // line. A stale response must never count as a completed command.
  if (Index == 55 && Interrupt == 0 && Data == NULL &&
      !(mLastPresent & 1U) && mLastResponse != BeforeResponse &&
      (mLastResponse & 0x20U) != 0 &&
      (mLastResponse & 0xfdffa080U) == 0) {
    RecoveredResponse = TRUE;
    ++mCmd55Recovered;
  }
  if (Index == 41 && Interrupt == 0 && Data == NULL &&
      !(mLastPresent & 1U) && mLastResponse != BeforeResponse &&
      (mLastResponse & 0x00ff8000U) != 0) {
    RecoveredResponse = TRUE;
    ++mAcmd41Recovered;
  }
  if (Index == 3 && Interrupt == 0 && Data == NULL &&
      !(mLastPresent & 1U) && mLastResponse != BeforeResponse &&
      (mLastResponse >> 16) != 0 && (mLastResponse >> 16) != 0xffffU &&
      (mLastResponse & 0xe000U) == 0) {
    // R6 carries a nonzero RCA and no COM_CRC_ERROR/ILLEGAL_COMMAND/ERROR.
    RecoveredResponse = TRUE;
    ++mCmd3Recovered;
  }
  if (Index == 9 && Interrupt == 0 && Data == NULL &&
      !(mLastPresent & 1U) && mLastResponse != BeforeResponse &&
      mLastResponse != 0 &&
      ((Read32 (HC, REG_RESPONSE + 12) >> 22) & 3U) == 1U) {
    // Only CSD v2 is supported below; reject stale or implausible R2 data.
    RecoveredResponse = TRUE;
    ++mCmd9Recovered;
  }
  if ((!(Interrupt & INT_CMD) && !RecoveredResponse) ||
      (Interrupt & INT_ERROR_MASK)) {
    Write32 (HC, REG_INTERRUPT, Interrupt);
    if (Interrupt & INT_ERROR_MASK) {
      Status = ResetCommandLine ();
      if (EFI_ERROR (Status)) return Status;
      return (Interrupt & 0x00020000U) ? EFI_CRC_ERROR : EFI_DEVICE_ERROR;
    }
    if (mLastPresent & 1U) (VOID)ResetCommandLine ();
    return EFI_TIMEOUT;
  }
  if (Response != NULL) {
    if ((Flags & 3U) == RESP_136) {
      UINT32 Raw[4];
      for (I = 0; I < 4; ++I) Raw[I] = Read32 (HC, REG_RESPONSE + I * 4);
      Response[0] = (Raw[3] << 8) | (Raw[2] >> 24);
      Response[1] = (Raw[2] << 8) | (Raw[1] >> 24);
      Response[2] = (Raw[1] << 8) | (Raw[0] >> 24);
      Response[3] = Raw[0] << 8;
    } else {
      Response[0] = Read32 (HC, REG_RESPONSE);
    }
  }
  Write32 (HC, REG_INTERRUPT, INT_CMD);
  if (Data == NULL) return EFI_SUCCESS;
  for (I = 0; I < 100000; ++I) {
    Interrupt = Read32 (HC, REG_INTERRUPT);
    if (Interrupt & INT_ERROR_MASK) return EFI_DEVICE_ERROR;
    if ((Interrupt & INT_READ_READY) ||
        (Read32 (HC, REG_PRESENT) & PRESENT_DATA)) break;
    MicroSecondDelay (10);
  }
  if (I == 100000) return EFI_TIMEOUT;
  for (I = 0; I < BLKSZ / 4; ++I) Words[I] = Read32 (HC, REG_BUFFER);
  Write32 (HC, REG_INTERRUPT, INT_READ_READY);
  for (I = 0; I < 100000; ++I) {
    Interrupt = Read32 (HC, REG_INTERRUPT);
    if (Interrupt & (INT_TRANSFER | INT_ERROR_MASK)) break;
    MicroSecondDelay (10);
  }
  Write32 (HC, REG_INTERRUPT, Interrupt & (INT_TRANSFER | INT_ERROR_MASK));
  if (Interrupt & INT_ERROR_MASK) return EFI_DEVICE_ERROR;
  return (Interrupt & INT_TRANSFER) ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC EFI_STATUS
InitHost (VOID)
{
  EFI_STATUS Status;
  UINTN I;
  mStage = 1;
  Status = PowerUp ();
  if (EFI_ERROR (Status)) return Status;
  mStage = 2;
  Status = SetPins ();
  if (EFI_ERROR (Status)) return Status;
  mStage = 3;
  Status = SetClock400k ();
  if (EFI_ERROR (Status)) return Status;
  mStage = 4;
  if (Read32 (TLMM, 67U * 0x1000U + 4U) & 1U) return EFI_NO_MEDIA;
  mStage = 5;
  Write32 (HC, REG_VENDOR, 0xa1cU);
  Write32 (CORE, CORE_MODE, 1U | (1U << 13));
  MmioWrite8 (HC + REG_RESET, 1);
  for (I = 0; I < 100000; ++I) {
    ServicePower ();
    if (!(MmioRead8 (HC + REG_RESET) & 1U)) break;
    MicroSecondDelay (10);
  }
  if (I == 100000) return EFI_TIMEOUT;
  Write32 (CORE, CORE_PWR_MASK, 0xfU);
  Write32 (HC, REG_INT_ENABLE, 0xffffffffU);
  Write32 (HC, REG_SIGNAL_ENABLE, 0);
  MmioWrite8 (HC + REG_TIMEOUT, 0x0e);
  MmioWrite16 (HC + REG_HOST_CONTROL2,
               MmioRead16 (HC + REG_HOST_CONTROL2) & ~0x000fU);
  MmioWrite16 (HC + REG_CLOCK, 1);
  Status = Wait32 (HC, REG_CLOCK, 2U, 2U, 20000);
  if (EFI_ERROR (Status)) return Status;
  MmioWrite16 (HC + REG_CLOCK, 5);
  MmioWrite8 (HC + REG_POWER, 0x0d); // SD supply 3.0 V encoding + bus on
  for (I = 0; I < 100; ++I) { ServicePower (); MicroSecondDelay (100); }
  MicroSecondDelay (5000);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
InitCard (VOID)
{
  EFI_STATUS Status;
  UINT32 Response[4] = {0};
  UINT32 Csd[4];
  UINT64 Sectors;
  UINTN Try;
  Status = Command (0, 0, RESP_NONE, NULL, NULL);
  if (EFI_ERROR (Status)) return Status;
  Status = Command (8, 0x1aa, RESP_48 | CMD_CRC | CMD_INDEX, Response, NULL);
  if (EFI_ERROR (Status)) return Status;
  if ((Response[0] & 0xfffU) != 0x1aaU)
    return EFI_PROTOCOL_ERROR;
  for (Try = 0; Try < 100; ++Try) {
    Status = Command (55, 0, RESP_48 | CMD_CRC | CMD_INDEX, Response, NULL);
    if (EFI_ERROR (Status)) return Status;
    Status = Command (41, 0x40ff8000U, RESP_48, Response, NULL); // ACMD41 R3
    if (EFI_ERROR (Status)) return Status;
    if (Response[0] & (1U << 31)) break;
    MicroSecondDelay (10000);
  }
  if (Try == 100) return EFI_TIMEOUT;
  if (!(Response[0] & (1U << 30))) return EFI_UNSUPPORTED; // SDHC/SDXC
  MicroSecondDelay (1000);
  for (Try = 0; Try < 3; ++Try) {
    Status = Command (2, 0, RESP_136 | CMD_CRC, Response, NULL);
    if (!EFI_ERROR (Status)) break;
    if (Status != EFI_CRC_ERROR) return Status;
    ++mCmd2Retries;
    MicroSecondDelay (1000);
  }
  if (EFI_ERROR (Status)) return Status;
  Status = Command (3, 0, RESP_48 | CMD_CRC | CMD_INDEX, Response, NULL);
  if (EFI_ERROR (Status)) return Status;
  mRca = (UINT16)(Response[0] >> 16);
  if (mRca == 0) return EFI_DEVICE_ERROR;
  for (Try = 0; Try < 3; ++Try) {
    Status = Command (9, (UINT32)mRca << 16,
                      RESP_136 | CMD_CRC, Csd, NULL);
    if (!EFI_ERROR (Status)) break;
    if (Status != EFI_TIMEOUT && Status != EFI_CRC_ERROR) return Status;
    ++mCmd9Retries;
    MicroSecondDelay (1000);
  }
  if (EFI_ERROR (Status)) return Status;
  if ((Csd[0] >> 30) != 1) return EFI_UNSUPPORTED;
  Sectors = (((UINT64)((Csd[1] & 0x3fU) << 16) | (Csd[2] >> 16)) + 1) * 1024;
  if (Sectors == 0 || Sectors > MAX_UINT32) return EFI_UNSUPPORTED;
  Status = Command (7, (UINT32)mRca << 16,
                    RESP_BUSY | CMD_CRC | CMD_INDEX, Response, NULL);
  if (EFI_ERROR (Status)) return Status;
  mMedia.LastBlock = Sectors - 1;
  mReady = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SdReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended)
{ (VOID)Extended; return This == &mBlockIo && mReady ? EFI_SUCCESS : EFI_NO_MEDIA; }

STATIC EFI_STATUS EFIAPI SdRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                                EFI_LBA Lba, UINTN BufferSize, VOID *Buffer)
{
  EFI_STATUS Status;
  UINTN Count, Index;
  UINT8 *Bytes = Buffer;
  UINT32 Sector[BLKSZ / sizeof (UINT32)];
  UINT32 Response[4];
  if (This != &mBlockIo || Buffer == NULL || BufferSize % BLKSZ != 0)
    return EFI_INVALID_PARAMETER;
  if (!mReady) return EFI_NO_MEDIA;
  if (MediaId != mMedia.MediaId) return EFI_MEDIA_CHANGED;
  Count = BufferSize / BLKSZ;
  if (Lba > mMedia.LastBlock || Count > mMedia.LastBlock - Lba + 1)
    return EFI_INVALID_PARAMETER;
  for (Index = 0; Index < Count; ++Index) {
    Status = Command (17, (UINT32)(Lba + Index),
                      RESP_48 | CMD_CRC | CMD_INDEX, Response, Sector);
    if (EFI_ERROR (Status)) return Status;
    if (Response[0] & 0xfdffa080U) return EFI_DEVICE_ERROR;
    CopyMem (Bytes + Index * BLKSZ, Sector, BLKSZ);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SdWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                                 EFI_LBA Lba, UINTN BufferSize, VOID *Buffer)
{ (VOID)This; (VOID)MediaId; (VOID)Lba; (VOID)BufferSize; (VOID)Buffer;
  return EFI_WRITE_PROTECTED; }

STATIC EFI_STATUS EFIAPI SdFlush (EFI_BLOCK_IO_PROTOCOL *This)
{ return This == &mBlockIo && mReady ? EFI_SUCCESS : EFI_NO_MEDIA; }

EFI_STATUS EFIAPI J250fSdcc2Entry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *System)
{
  EFI_STATUS Status;
  UINT8 Sector0[BLKSZ];
  (VOID)Image; (VOID)System;
  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId = 1;
  mMedia.RemovableMedia = TRUE;
  mMedia.MediaPresent = FALSE;
  mMedia.ReadOnly = TRUE;
  mMedia.BlockSize = BLKSZ;
  mMedia.IoAlign = 1;
  mBlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION3;
  mBlockIo.Media = &mMedia;
  mBlockIo.Reset = SdReset;
  mBlockIo.ReadBlocks = SdRead;
  mBlockIo.WriteBlocks = SdWrite;
  mBlockIo.FlushBlocks = SdFlush;

  Status = InitHost ();
  if (!EFI_ERROR (Status)) Status = InitCard ();
  if (!EFI_ERROR (Status)) {
    mMedia.MediaPresent = TRUE;
    Status = SdRead (&mBlockIo, mMedia.MediaId, 0, BLKSZ, Sector0);
    if (!EFI_ERROR (Status) && (Sector0[510] != 0x55 || Sector0[511] != 0xaa))
      Status = EFI_VOLUME_CORRUPTED;
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "J250F SDCC2: card init/read failed: %r\n", Status));
    ReportStatus (Status);
    mReady = FALSE;
    mMedia.MediaPresent = FALSE;
    return Status;
  }
  ZeroMem (&mPath, sizeof (mPath));
  mPath.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  mPath.Vendor.Header.SubType = HW_VENDOR_DP;
  mPath.Vendor.Header.Length[0] = sizeof (VENDOR_DEVICE_PATH);
  CopyMem (&mPath.Vendor.Guid, &mSdGuid, sizeof (EFI_GUID));
  mPath.End.Type = END_DEVICE_PATH_TYPE;
  mPath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mPath.End.Length[0] = sizeof (EFI_DEVICE_PATH_PROTOCOL);
  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle,
             &gEfiBlockIoProtocolGuid, &mBlockIo,
             &gEfiDevicePathProtocolGuid, &mPath, NULL);
  DEBUG ((DEBUG_ERROR, "J250F SDCC2: %Lu sectors, BlockIo=%r\n",
          mMedia.LastBlock + 1, Status));
  mStage = 0x200;
  ReportStatus (Status);
  return Status;
}
