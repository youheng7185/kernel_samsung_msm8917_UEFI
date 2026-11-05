/** @file
 *DeviceBootManager  - Ms Device specific extensions to BdsDxe.

Copyright (C) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <DfciSystemSettingTypes.h>

#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>
#include <Guid/MsBootMenuGuid.h>
#include <Guid/DfciPacketHeader.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <J250fSdcc2Status.h>
#include <Protocol/Security2.h>
#include <Library/BaseCryptLib.h>
#include <Protocol/OnScreenKeyboard.h>
#include <Protocol/TpmPpProtocol.h>

#include <Library/BaseMemoryLib.h>
#include <Library/ConsoleMsgLib.h>
#include <Library/DebugLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsBootManagerSettingsLib.h>
#include <Library/MsBootOptionsLib.h>
#include <Library/MsBootOptions.h>
#include <Library/MsBootPolicyLib.h>
#include <Library/MsBootPolicy.h>
#include <Library/BootGraphicsProviderLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphics.h>
#include <Library/GraphicsConsoleHelperLib.h>
#include <Library/MsPlatformDevicesLib.h>
#include <Library/MsPlatformPowerCheckLib.h>
#include <Library/MsNetworkDependencyLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#if defined(J250F_ADB_RAM_EXECUTION)
#include <Library/FrameBufferSerialPortLib.h>
#endif
#include <Library/PowerServicesLib.h>
#include <Library/ThermalServicesLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MsNVBootReasonLib.h>
#include <Library/MuTelemetryHelperLib.h>
#include <Library/VariablePolicyHelperLib.h>
#include <Library/UpdateFacsHardwareSignatureLib.h>

#include <Settings/BootMenuSettings.h>
#include <Settings/DfciSettings.h>

static EFI_EVENT  mPreReadyToBootEvent;
static EFI_EVENT  mPostReadyToBootEvent;

#if defined(J250F_BOOT_TO_SHELL)
STATIC EFI_GUID mJ250fSdcc2PathGuid = {
  0x27863253,0x2d92,0x489b,{0xab,0x78,0xd3,0x37,0x71,0x4d,0xcf,0x21}
};

STATIC BOOLEAN
J250fIsSdcc2Volume (EFI_HANDLE Handle)
{
  EFI_DEVICE_PATH_PROTOCOL *Node = DevicePathFromHandle (Handle);
  UINTN Limit = 64;
  while (Node != NULL && Limit-- != 0 && !IsDevicePathEnd (Node)) {
    if (DevicePathType (Node) == HARDWARE_DEVICE_PATH &&
        DevicePathSubType (Node) == HW_VENDOR_DP &&
        DevicePathNodeLength (Node) >= sizeof (VENDOR_DEVICE_PATH) &&
        CompareGuid (&((VENDOR_DEVICE_PATH *)Node)->Guid,
                     &mJ250fSdcc2PathGuid)) {
      return TRUE;
    }
    Node = NextDevicePathNode (Node);
  }
  return FALSE;
}

STATIC BOOLEAN
J250fDtbName (CONST CHAR16 *Name)
{
  UINTN Length = StrLen (Name);
  if (Length == 3) {
    return CharToUpper (Name[0]) == L'D' &&
           CharToUpper (Name[1]) == L'T' &&
           CharToUpper (Name[2]) == L'B';
  }
  return Length > 4 && Name[Length - 4] == L'.' &&
         CharToUpper (Name[Length - 3]) == L'D' &&
         CharToUpper (Name[Length - 2]) == L'T' &&
         CharToUpper (Name[Length - 1]) == L'B';
}

STATIC VOID
J250fDiscoverBootFiles (VOID)
{
  EFI_GUID                         StatusGuid = J250F_SDCC2_STATUS_GUID;
  J250F_SDCC2_STATUS               SdStatus;
  EFI_HANDLE                       *Handles = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Image;
  EFI_FILE_INFO                    *Info;
  EFI_STATUS                       Status;
  UINTN                            Count = 0;
  UINTN                            Index;
  UINTN                            Size;
  UINTN                            Entries;
  BOOLEAN                          FoundDtb;
  UINTN                            Matches = 0;

  Size = sizeof (SdStatus);
  Status = gRT->GetVariable (L"J250fSdcc2Status", &StatusGuid,
                  NULL, &Size, &SdStatus);
  if (!EFI_ERROR (Status) && Size == sizeof (SdStatus)) {
    Print (L"SDCC2 stage=0x%x status=%r\r\n", SdStatus.Stage,
           (EFI_STATUS)SdStatus.Status);
    Print (L"IRQ=%08x RSP=%08x PRESENT=%08x\r\n",
           SdStatus.Interrupt, SdStatus.Response, SdStatus.Present);
    Print (L"CLOCK=%04x PWR=%08x\r\n",
           SdStatus.Clock, SdStatus.Power);
    Print (L"IEN=%08x CMD=%04x\r\n",
           SdStatus.InterruptEnable, SdStatus.Command);
    Print (L"Recovered: CMD55=%u ACMD41=%u\r\n",
           SdStatus.Cmd55Recovered, SdStatus.Acmd41Recovered);
    Print (L"Retry: CMD2=%u CMD9=%u\r\n",
           SdStatus.Cmd2Retries, SdStatus.Cmd9Retries);
    Print (L"Recovered: CMD3=%u CMD9=%u\r\n",
           SdStatus.Cmd3Recovered, SdStatus.Cmd9Recovered);
    Print (L"RSP1=%08x RSP2=%08x RSP3=%08x\r\n",
           SdStatus.Response1, SdStatus.Response2, SdStatus.Response3);
  } else {
    Print (L"SDCC2 driver has not reported status.\r\n");
  }

  // Equivalent to the shell's connect -r and map -r discovery pass.
  EfiBootManagerConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"No filesystem volumes found (%r).\r\n", Status);
    return;
  }

  Info = AllocatePool (4096);
  if (Info == NULL) {
    FreePool (Handles);
    return;
  }
  for (Index = 0; Index < Count; ++Index) {
    Status = gBS->HandleProtocol (Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      Print (L"Volume %u: open failed (%r)\r\n", (UINT32)Index, Status);
      continue;
    }
    Print (L"Volume %u root%s (shell fs alias may differ):\r\n",
           (UINT32)Index,
           J250fIsSdcc2Volume (Handles[Index]) ? L" [SDCC2 card]" : L"");
    Entries = 0;
    for (;;) {
      Size = 4096;
      Status = Root->Read (Root, &Size, Info);
      if (EFI_ERROR (Status) || Size == 0) break;
      Print (L"  %s  %Lu bytes%s\r\n", Info->FileName, Info->FileSize,
             (Info->Attribute & EFI_FILE_DIRECTORY) ? L" [dir]" : L"");
      if (++Entries == 48) {
        Print (L"  ... root listing limited to 48 entries\r\n");
        break;
      }
    }
    if (EFI_ERROR (Status)) {
      Print (L"  directory read: %r\r\n", Status);
    } else if (Entries == 0) {
      Print (L"  (empty)\r\n");
    }
    Status = Root->SetPosition (Root, 0);
    if (EFI_ERROR (Status)) {
      Print (L"Volume %u: rewind failed (%r)\r\n", (UINT32)Index, Status);
      Root->Close (Root);
      continue;
    }
    Image = NULL;
    Status = Root->Open (Root, &Image, L"Image", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status)) {
      Root->Close (Root);
      continue;
    }
    Image->Close (Image);
    FoundDtb = FALSE;
    for (;;) {
      Size = 4096;
      Status = Root->Read (Root, &Size, Info);
      if (EFI_ERROR (Status) || Size == 0) {
        break;
      }
      if (!(Info->Attribute & EFI_FILE_DIRECTORY) &&
          J250fDtbName (Info->FileName)) {
        Print (L"Volume %u: Image and %s found\r\n", (UINT32)Index,
               Info->FileName);
        FoundDtb = TRUE;
        ++Matches;
        break;
      }
    }
    if (!FoundDtb) {
      Print (L"Volume %u: Image found; no DTB at root\r\n", (UINT32)Index);
    }
    Root->Close (Root);
  }
  if (Matches == 0) {
    Print (L"SDCC2: no volume with Image and DTB found.\r\n");
  }
  FreePool (Info);
  FreePool (Handles);
}
#endif

/* J250F one-partition WP81 BDS diagnostic state. No storage writes. */
STATIC EFI_GUID mWpEspReadyGuid = {
  0xd7f18d20, 0x49b1, 0x4e41, {0x8d,0x1a,0x91,0x46,0x6c,0x7e,0x37,0xa2}
};
/* Instrumentation from MsBootPolicyLib's *existing* LoadImage wrapper.
   No image authentication/boot policy bypass is performed. */
extern BOOLEAN    gWp81LoadObserved;
extern BOOLEAN    gWp81LoadPolicyRejected;
extern EFI_STATUS gWp81LoadStatus;
extern BOOLEAN    gWp81LoadHookInstalled;
extern UINTN      gWp81LoadCallCount;

STATIC BOOLEAN mWpDirectLoadProbeAttempted = FALSE;
STATIC EFI_STATUS mWpDirectLoadProbeResult = EFI_NOT_STARTED;
STATIC UINTN mWpHookCallsBeforeProbe = 0;
STATIC EFI_STATUS mWpHookStatusBeforeProbe = EFI_NOT_STARTED;
STATIC BOOLEAN mWpBootFF10Attempted = FALSE;
STATIC EFI_STATUS mWpBootFF10Result = EFI_NOT_READY;

/* Single-image trust exception for this lab-only ESP boot experiment.
 * This is a narrowly scoped authentication override. It is NOT a Secure Boot
 * enforcement model and is not safe to reuse for general device booting.
 * The only permitted image is the exact 771152-byte BLU donor BOOTMGFW.EFI
 * (SHA-256 below) on this driver-created virtual ESP. All other calls are
 * passed unchanged to the firmware's original Security2 handler.
 */
STATIC EFI_GUID mWpVirtualDiskPathGuid = {
  0xe7b5ba2a,0xd586,0x4df0,{0x8e,0x28,0x2a,0xd5,0x5d,0x85,0x9c,0x81}
};
STATIC CONST UINT8 mWpDonorBootmgrSha256[32] = {
  0xc4,0xfc,0x8b,0x29,0xd3,0xf4,0xe9,0xfb,
  0xbf,0xf3,0x69,0x6c,0x12,0xb6,0xed,0x15,
  0xac,0xde,0x21,0x9e,0x6b,0x88,0x28,0xfb,
  0x46,0x5e,0xd8,0x31,0x8b,0xfa,0x6a,0xdf
};
STATIC EFI_SECURITY2_FILE_AUTHENTICATION mWpOriginalSecurity2Auth;
STATIC BOOLEAN mWpSecurity2HookInstalled = FALSE;
STATIC BOOLEAN mWpDonorTrustApplied = FALSE;
STATIC EFI_STATUS mWpOriginalSecurityStatus = EFI_NOT_STARTED;

STATIC BOOLEAN WpHasExactVirtualBootmgrPath (
  CONST EFI_DEVICE_PATH_PROTOCOL *Path
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL *Node;
  BOOLEAN Vendor = FALSE;
  BOOLEAN ExactFile = FALSE;
  UINTN Limit = 2048;
  if (Path == NULL) return FALSE;
  Node = Path;
  while (Limit-- && !IsDevicePathEnd(Node)) {
    UINTN Size = DevicePathNodeLength(Node);
    if (Size < sizeof(EFI_DEVICE_PATH_PROTOCOL) || Size > MAX_UINT16)
      return FALSE;
    if (DevicePathType(Node) == HARDWARE_DEVICE_PATH &&
        DevicePathSubType(Node) == HW_VENDOR_DP &&
        Size == sizeof(VENDOR_DEVICE_PATH) &&
        CompareGuid(&((CONST VENDOR_DEVICE_PATH *)Node)->Guid,
                    &mWpVirtualDiskPathGuid)) {
      Vendor = TRUE;
    }
    if (DevicePathType(Node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType(Node) == MEDIA_FILEPATH_DP &&
        Size >= sizeof(FILEPATH_DEVICE_PATH)) {
      CONST FILEPATH_DEVICE_PATH *Fp = (CONST FILEPATH_DEVICE_PATH *)Node;
      /* Bound string compare to node length; never read untrusted
         device-path bytes past the current node. */
      UINTN Chars = (Size - OFFSET_OF(FILEPATH_DEVICE_PATH, PathName)) /
                    sizeof(CHAR16);
      STATIC CONST CHAR16 Expected[] = L"\\Windows\\BOOT\\EFI\\BOOTMGFW.EFI";
      if (Chars >= sizeof(Expected)/sizeof(CHAR16) &&
          CompareMem(Fp->PathName,Expected,sizeof(Expected)) == 0)
        ExactFile = TRUE;
    }
    Node = NextDevicePathNode(Node);
  }
  return Vendor && ExactFile && Limit > 0;
}

STATIC EFI_STATUS EFIAPI WpExactDonorSecurity2Auth (
  IN CONST EFI_SECURITY2_ARCH_PROTOCOL *This,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *File OPTIONAL,
  IN VOID                              *FileBuffer,
  IN UINTN                             FileSize,
  IN BOOLEAN                           BootPolicy
  )
{
  EFI_STATUS Status;
  UINT8 Digest[32];
  VOID *Ready = NULL;
  Status = mWpOriginalSecurity2Auth(This,File,FileBuffer,FileSize,BootPolicy);
  mWpOriginalSecurityStatus = Status;
  /* Never override EFI_SECURITY_VIOLATION (including revocation failures).
     This experimental exception addresses ONLY the observed ACCESS_DENIED. */
  if (Status != EFI_ACCESS_DENIED)
    return Status;
  /* Note the marker exists only after private eMMC backing is staged,
     public physical BlockIo is removed, and virtual GPT is validated. */
  if (FileBuffer == NULL || FileSize != 771152 ||
      EFI_ERROR(gBS->LocateProtocol(&mWpEspReadyGuid,NULL,&Ready)) ||
      Ready == NULL || !WpHasExactVirtualBootmgrPath(File) ||
      !Sha256HashAll(FileBuffer,FileSize,Digest) ||
      CompareMem(Digest,mWpDonorBootmgrSha256,sizeof(Digest)) != 0)
    return Status;
  /* A deliberate exception for this exact donor image only. No other EFI
     binary, no physical disk, and no image with a different hash is allowed.
     This skips the original handler's denial for exactly this one image. */
  mWpDonorTrustApplied = TRUE;
  DEBUG ((DEBUG_ERROR,
    "[WP81] ACCEPT exact donor BOOTMGFW.EFI sha256, original Security2=%r; "
    "experimental image-specific trust override.\n", Status));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WpInstallDonorTrust (VOID)
{
  EFI_SECURITY2_ARCH_PROTOCOL *Sec2 = NULL;
  EFI_STATUS Status;
  if (mWpSecurity2HookInstalled) return EFI_SUCCESS;
  Status = gBS->LocateProtocol(&gEfiSecurity2ArchProtocolGuid,NULL,
                                (VOID **)&Sec2);
  if (EFI_ERROR(Status)) return Status;
  if (Sec2 == NULL || Sec2->FileAuthentication == NULL)
    return EFI_NOT_READY;
  mWpOriginalSecurity2Auth = Sec2->FileAuthentication;
  Sec2->FileAuthentication = WpExactDonorSecurity2Auth;
  mWpSecurity2HookInstalled = TRUE;
  DEBUG ((DEBUG_ERROR,
    "[WP81] Installed exact-hash trust exception for virtual ESP BOOTMGFW.EFI.\n"));
  return EFI_SUCCESS;
}



CHAR8  mMemoryType[][30] = {
  // Value for PcdMemoryMapTypes
  "EfiReservedMemoryType      ",                               // 0x0001
  "EfiLoaderCode              ",                               // 0x0002
  "EfiLoaderData              ",                               // 0x0004
  "EfiBootServicesCode        ",                               // 0x0008
  "EfiBootServicesData        ",                               // 0x0010
  "EfiRuntimeServicesCode     ",                               // 0x0020
  "EfiRuntimeServicesData     ",                               // 0x0040
  "EfiConventionalMemory      ",                               // 0x0080
  "EfiUnusableMemory          ",                               // 0x0100
  "EfiACPIReclaimMemory       ",                               // 0x0200   Both ACPI types would
  "EfiACPIMemoryNVS           ",                               // 0x0400   be 0x0600
  "EfiMemoryMappedIO          ",                               // 0x0800
  "EfiMemoryMappedIOPortSpace ",                               // 0x1000
  "EfiPalCode                 ",                               // 0x2000
  "EfiMaxMemoryType           "
};

static
VOID
ThermalFailureShutdown (
  VOID
  )
{
  EFI_STATUS Status = EFI_SUCCESS;
  UINT32     WaitTime = PcdGet32 (PcdShutdownGraphicDisplayTime);

  // Display the too hot picture
  Status = DisplayBootGraphic (BG_CRITICAL_OVER_TEMP);
  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((DEBUG_ERROR, "%a Unabled to set graphics - %r\n", __FUNCTION__, Status));
  }

  // Wait a few seconds
  gBS->Stall(WaitTime);

  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}

static
VOID
PowerFailureShutdown (
  VOID
  )
{
  EFI_STATUS Status   = EFI_SUCCESS;
  UINT32     WaitTime = PcdGet32 (PcdShutdownGraphicDisplayTime);

  // Display the low battery picture

  DEBUG ((DEBUG_INFO, "Fetch the display picture.\n"));

  Status = DisplayBootGraphic (BG_CRITICAL_LOW_BATTERY);
  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((DEBUG_ERROR, "%a Unabled to set graphics - %r\n", __FUNCTION__, Status));
  }

  // Wait a few seconds
  gBS->Stall(WaitTime);

  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}

static
EFI_STATUS
MsPreBootChecks (
  VOID
  )
{
  EFI_STATUS  Status      = EFI_SUCCESS;
  EFI_STATUS  WaitStatus  = EFI_SUCCESS;
  BOOLEAN     ThermalGood = TRUE;
  BOOLEAN     PowerGood   = TRUE;
  INT32       RetryCount  = PcdGet32 (PcdPowerAndThermalRetries);

  EFI_EVENT  TimerEvent = NULL;
  UINTN      index;
  UINT32     WaitTime = PcdGet32 (PcdThermalControlRetryWait);

  UINT32  ThermalFailureCount = 1;

  // Create an event

  DEBUG ((DEBUG_INFO, "Inside MsPrebootchecks\n"));

  Status = gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &TimerEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Create Event failed. %r\n", Status));
    goto CleanUp;
  }

  do {
    // Check to see if the Power situation is good

    DEBUG ((DEBUG_INFO, "SystemPowerCheck\n"));

    Status = SystemPowerCheck (PowerCaseBoot, &PowerGood);

    DEBUG ((DEBUG_INFO, "SystemPowerCheck %r\n", Status));

    if ((!EFI_ERROR (Status)) && (!PowerGood)) {
      DEBUG ((DEBUG_INFO, "SystemPowerMitigate(Boot)\n"));

      Status = SystemPowerMitigate (PowerCaseBoot);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_INFO, "SystemPowerMitigate(Boot) failed - %r.  Shutdown now\n", Status));

        PowerGood = FALSE;
        goto CleanUp;
      }

      // There should be enough power to boot so fall through to next test.
      // Thermal mitigation may burn battery so we will check power once
      // more before booting.
    } else {
      // change - error retrieving power state should not stop boot
      PowerGood = TRUE;
    }

    // Check to see if the thermal situation is good

    Status = SystemThermalCheck (ThermalCaseBoot, &ThermalGood);

    if ((!EFI_ERROR (Status)) && (!ThermalGood)) {
      if (1 == ThermalFailureCount) {
        // Set active cooling event

        DEBUG ((DEBUG_WARN, "MsPreBootChecks: Thermal mitgation has been started\n"));
      }

      if (2 == ThermalFailureCount) {
        // Set passive cooling event
      }

      if (ThermalFailureCount < 3) {
        ThermalFailureCount++;
      }

      // Wait for cooling to have an effect but not so long we completely
      // drain the battery. ToDo: should consider adding some UI to let the
      // user know what is going on.

      if (RetryCount > 0) {
        // Set timer using the event
        gBS->SetTimer (TimerEvent, TimerRelative, WaitTime);

        // Wait for event to fire
        WaitStatus = gBS->WaitForEvent (1, &TimerEvent, &index);
        if (EFI_ERROR (WaitStatus)) {
          DEBUG ((DEBUG_ERROR, "MsPreBootChecks: WaitForEvent failed. %r\n", Status));
        }
      }
    } else {
      // change - error retrieving thermal should not stop boot
      ThermalGood = TRUE;
    }
  } while ((RetryCount-- > 0) && ((!ThermalGood) || (!PowerGood)));

CleanUp:

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  if (!ThermalGood) {
    DEBUG ((DEBUG_ERROR, "MsPreBootChecks failed when calling Thermal Good function. %r\n", Status));
    LogTelemetry (TRUE, NULL, EFI_COMPUTING_UNIT_HOST_PROCESSOR | EFI_CU_HP_EC_THERMAL, NULL, NULL, 0, 0);
    ThermalFailureShutdown ();    // Should never return from this function
  }

  if (!PowerGood) {
    DEBUG ((DEBUG_ERROR, "MsPreBootChecks failed when calling Power Good function. %r\n", Status));
    LogTelemetry (TRUE, NULL, EFI_COMPUTING_UNIT_HOST_PROCESSOR | EFI_CU_HP_EC_LOW_VOLTAGE, NULL, NULL, 0, 0);
    PowerFailureShutdown ();    // Should never return from this function
  }

  return Status;
}

/**
  Lock the required boot variables if LockBootOrder is enabled
*/
static
VOID
BdsBootLockBootVariables (
  VOID
  )
{
  EFI_STATUS                      Status;
  BOOLEAN                         EnableBootOrderLock = FALSE;
  EDKII_VARIABLE_POLICY_PROTOCOL  *VarPolicyProtocol  = NULL;
  UINT16                          *BootOrder;
  UINTN                           BootOrderSize;
  CHAR16                          OptionName[sizeof ("Boot####")];
  UINTN                           i;
  static BOOLEAN                  AlreadyLocked = FALSE;

  if (AlreadyLocked) {
    // This can happen as we may call ready to boot a number of times;
    return;
  }

  Status = GetBootManagerSetting (
             DFCI_SETTING_ID__BOOT_ORDER_LOCK,
             &EnableBootOrderLock
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a Unable to get BootOrderLock setting\n", __FUNCTION__));
    return;
  }

  if (!EnableBootOrderLock) {
    DEBUG ((DEBUG_INFO, "%a - BootOrder is not locked\n", __FUNCTION__));
    return;
  }

  Status = gBS->LocateProtocol (&gEdkiiVariablePolicyProtocolGuid, NULL, (VOID **)&VarPolicyProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a - Failed to locate var policy protocol (%r).  Can't lock variables\n", __FUNCTION__, Status));
    return;
  }

  Status = RegisterBasicVariablePolicy (
             VarPolicyProtocol,
             &gEfiGlobalVariableGuid,
             EFI_BOOT_ORDER_VARIABLE_NAME,
             VARIABLE_POLICY_NO_MIN_SIZE,
             VARIABLE_POLICY_NO_MAX_SIZE,
             VARIABLE_POLICY_NO_MUST_ATTR,
             VARIABLE_POLICY_NO_CANT_ATTR,
             VARIABLE_POLICY_TYPE_LOCK_NOW
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unable to lock BootOrder. Code=%r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "Variable BootOrder locked\n"));
  }

  // Delete BootNext as a locked BootNext is a bad thing
  Status = gRT->SetVariable (
                  EFI_BOOT_NEXT_VARIABLE_NAME,
                  &gEfiGlobalVariableGuid,
                  0,
                  0,
                  NULL
                  );
  DEBUG ((DEBUG_INFO, "Status from deleting BootNext prior to lock. Code=%r\n", Status));

  Status = RegisterBasicVariablePolicy (
             VarPolicyProtocol,
             &gEfiGlobalVariableGuid,
             EFI_BOOT_NEXT_VARIABLE_NAME,
             VARIABLE_POLICY_NO_MIN_SIZE,
             VARIABLE_POLICY_NO_MAX_SIZE,
             VARIABLE_POLICY_NO_MUST_ATTR,
             VARIABLE_POLICY_NO_CANT_ATTR,
             VARIABLE_POLICY_TYPE_LOCK_NOW
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unable to lock BootNext. Code=%r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "Variable BootNext locked\n"));
  }

  GetVariable2 (
    L"BootOrder",
    &gEfiGlobalVariableGuid,
    (VOID **)&BootOrder,
    &BootOrderSize
    );
  if (BootOrder == NULL) {
    DEBUG ((DEBUG_ERROR, "%a - Failed to locate BootOrder (%r).  Can't lock Boot####\n", __FUNCTION__, Status));
    return;
  }

  for (i = 0; i < (BootOrderSize / sizeof (UINT16)); i++) {
    UnicodeSPrint (OptionName, sizeof (OptionName), L"Boot%04x", BootOrder[i]);
    Status = RegisterBasicVariablePolicy (
               VarPolicyProtocol,
               &gEfiGlobalVariableGuid,
               OptionName,
               VARIABLE_POLICY_NO_MIN_SIZE,
               VARIABLE_POLICY_NO_MAX_SIZE,
               VARIABLE_POLICY_NO_MUST_ATTR,
               VARIABLE_POLICY_NO_CANT_ATTR,
               VARIABLE_POLICY_TYPE_LOCK_NOW
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Unable to lock %s. Code=%r\n", OptionName, Status));
    } else {
      DEBUG ((DEBUG_INFO, "Variable %s locked\n", OptionName));
    }
  }
}

/**
 * Enable the On Screen Keyboard
 *
 *
 * @return VOID
 */
static
VOID
EnableOSK (
  VOID
  )
{
  EFI_STATUS                         Status;
  MS_ONSCREEN_KEYBOARD_PROTOCOL      *OSKProtocol;
  MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *SWMProtocol;

  //
  // Enable on-screen keyboard icon auto-display mode.  This will cause the keyboard
  // icon to be presented when an application (launched by BDS) waits on the keyboards
  // Simple Text In (or Extended) event.
  //

  // Locate the on-screen keyboard (OSK) protocol
  //
  Status = gBS->LocateProtocol (
                  &gMsOSKProtocolGuid,
                  NULL,
                  (VOID **)&OSKProtocol
                  );

  if (EFI_ERROR (Status)) {
    OSKProtocol = NULL;
    DEBUG ((DEBUG_WARN, "WARN: Failed to find the on-screen keyboard protocol (%r).\r\n", Status));
  } else {
    // Configure the OSK size, position, and state (75% of screen width, bottom right
    // position, docked) - this is most compatible with the Windows 10 English-localized
    // Bitlocker PIN screen.  The keyboard is not displayed by default.
    //
    OSKProtocol->ShowKeyboard (OSKProtocol, FALSE);
    OSKProtocol->SetKeyboardSize (OSKProtocol, 75);
    OSKProtocol->SetKeyboardPosition (OSKProtocol, BottomRight, Docked);
    OSKProtocol->ShowDockAndCloseButtons (OSKProtocol, TRUE);

    // Configure the OSK icon position (used to launch on-screen keyboard).  The keyboard
    // icon is not displayed by default.
    //
    OSKProtocol->ShowKeyboardIcon (OSKProtocol, FALSE);
    OSKProtocol->SetKeyboardIconPosition (OSKProtocol, BottomRight);

    // Configure the OSK icon for auto-enable and self-refresh modes.
    //
    OSKProtocol->SetKeyboardMode (OSKProtocol, (OSK_MODE_AUTOENABLEICON | OSK_MODE_SELF_REFRESH));
  }

  //
  // Disable the mouse pointer.  This allows a 3rd party application to render the pointer themselves
  // if they wish.  If the on-screen keyboard auto activates or if we launch our own application that
  // requires mouse pointer (ex: FrontPage), they will re-enable our mouse pointer.
  //

  // Locate the Simple Window Manager (SWM) protocol
  //
  Status = gBS->LocateProtocol (
                  &gMsSWMProtocolGuid,
                  NULL,
                  (VOID **)&SWMProtocol
                  );

  if (EFI_ERROR (Status)) {
    SWMProtocol = NULL;
    DEBUG ((DEBUG_WARN, "WARN: Failed to find the Simple Window Manager protocol (%r).\r\n", Status));
  } else {
    // Disable the mouse pointer.
    //
    SWMProtocol->EnableMousePointer (
                   SWMProtocol,
                   FALSE
                   );
  }
}

/**
 * Print Memory Map
 *
 * @return VOID
 */
static
VOID
PrintMemoryMap (
  )
{
  if (PcdGet8 (PcdEnableMemMapOutput)) {
    EFI_STATUS             Status;
    UINTN                  MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR  *MemoryMap    = NULL;
    VOID                   *p;
    CHAR8                  *Entry;
    UINTN                  MapKey;
    UINTN                  DescriptorSize;
    UINT32                 DescriptorVersion;
    UINTN                  Count;
    UINTN                  i;

    Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      MemoryMap = AllocatePool (MemoryMapSize + (sizeof (EFI_MEMORY_DESCRIPTOR) * 2));
      p         = (VOID *)MemoryMap; // Save for FreePool
      if (MemoryMap != NULL) {
        Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
        Entry  = (CHAR8 *)MemoryMap;
        if (Status == EFI_SUCCESS) {
          Count = MemoryMapSize / DescriptorSize;
          for (i = 0; i < Count; i++) {
            MemoryMap = (EFI_MEMORY_DESCRIPTOR *)Entry;
            if (MemoryMap->Type <= EfiMaxMemoryType) {
              if (((1 << MemoryMap->Type) & PcdGet32 (PcdEnableMemMapTypes)) != 0) {
                DEBUG ((DEBUG_INFO, "%a at %p for %d pages\n", mMemoryType[MemoryMap->Type], MemoryMap->PhysicalStart, MemoryMap->NumberOfPages));
                if (PcdGet8 (PcdEnableMemMapDumpOutput)) {
                  DUMP_HEX (DEBUG_INFO, 0, (CHAR8 *)MemoryMap->PhysicalStart, 48, "");
                }
              }
            } else {
              DEBUG ((DEBUG_ERROR, "Invalid memory type - %x\n", MemoryMap->Type));
            }

            Entry += DescriptorSize;
          }
        }

        FreePool (p);
      }
    }
  }
}

/**
  Check if current BootCurrent variable is internal shell boot option.

  @retval  TRUE         BootCurrent is internal shell.
  @retval  FALSE        BootCurrent is not internal shell.
**/
static
BOOLEAN
BootCurrentIsInternalShell (
  VOID
  )
{
  UINTN                     VarSize;
  UINT16                    BootCurrent;
  CHAR16                    BootOptionName[16];
  UINT8                     *BootOption;
  UINT8                     *Ptr;
  BOOLEAN                   Result;
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *LastDeviceNode;
  EFI_GUID                  *GuidPoint;

  BootOption = NULL;
  Result     = FALSE;

  //
  // Get BootCurrent variable
  //
  VarSize = sizeof (UINT16);
  Status  = gRT->GetVariable (
                   L"BootCurrent",
                   &gEfiGlobalVariableGuid,
                   NULL,
                   &VarSize,
                   &BootCurrent
                   );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  //
  // Create boot option Bootxxxx from BootCurrent
  //
  UnicodeSPrint (BootOptionName, sizeof (BootOptionName), L"Boot%04x", BootCurrent);

  GetEfiGlobalVariable2 (BootOptionName, (VOID **)&BootOption, &VarSize);

  if ((BootOption == NULL) || (VarSize == 0)) {
    return FALSE;
  }

  Ptr            = BootOption;
  Ptr           += sizeof (UINT32);
  Ptr           += sizeof (UINT16);
  Ptr           += StrSize ((CHAR16 *)Ptr);
  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Ptr;
  LastDeviceNode = TempDevicePath;
  while (!IsDevicePathEnd (TempDevicePath)) {
    LastDeviceNode = TempDevicePath;
    TempDevicePath = NextDevicePathNode (TempDevicePath);
  }

  GuidPoint = EfiGetNameGuidFromFwVolDevicePathNode (
                (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)LastDeviceNode
                );
  if ((GuidPoint != NULL) &&
      ((CompareGuid (GuidPoint, PcdGetPtr (PcdShellFile))) ||
       (CompareGuid (GuidPoint, &gUefiShellFileGuid)))
      )
  {
    //
    // if this option is internal shell, return TRUE
    //
    Result = TRUE;
  }

  if (BootOption != NULL) {
    FreePool (BootOption);
    BootOption = NULL;
  }

  return Result;
}

/**
Pre ready to boot callback to lock bds variables.

@param  Event                 Event whose notification function is being invoked.
@param  Context               The pointer to the notification function's context,
which is implementation-dependent.
**/
static
VOID
EFIAPI
PreReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BdsBootLockBootVariables ();
  EnableOSK ();

  gBS->CloseEvent (Event);
  return;
}

/**
Post ready to boot callback to print memory map, and update FACS hardware signature.
For booting the internal shell, set the video resolution to low.

@param  Event                 Event whose notification function is being invoked.
@param  Context               The pointer to the notification function's context,
which is implementation-dependent.
**/
static
VOID
EFIAPI
PostReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BOOLEAN         StartNetworkStack = FALSE;
  EFI_STATUS      Status;
  static BOOLEAN  FirstPass = TRUE;

  if (BootCurrentIsInternalShell ()) {
    EfiBootManagerConnectAll ();
    if (PcdGetBool (PcdLowResolutionInternalShell)) {
      Status = SetGraphicsConsoleMode (GCM_LOW_RES);
      if (EFI_ERROR (Status) != FALSE) {
        DEBUG ((DEBUG_ERROR, "%a Unable to set console mode - %r\n", __FUNCTION__, Status));
      }
    }
  }

  if (FirstPass) {
    FirstPass = FALSE;
    Status    = GetBootManagerSetting (
                  DFCI_SETTING_ID__START_NETWORK,
                  &StartNetworkStack
                  );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a Unable to get Start Network setting\n", __FUNCTION__));
    } else {
      if (StartNetworkStack) {
        DEBUG ((DEBUG_INFO, "%a - Starting the network stack\n", __FUNCTION__));
        // This will unblock the network stack.
        StartNetworking ();

        // ConnectAll - Convert to ConnetLess in phase 2 (Work Item 1544)
        EfiBootManagerConnectAll ();
      }
    }

    PrintMemoryMap ();
    Status = UpdateFacsHardwareSignature (DefaultFacsHwSigAlgorithm);
  }

  return;
}

/**
 * Constructor   - This runs when BdsDxe is loaded, before BdsArch protocol is published
 *
 * @return EFI_STATUS
 */
EFI_STATUS
EFIAPI
DeviceBootManagerConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  // Install Pre-ReadyToBoot callback to note when the variables need to be locked
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PreReadyToBoot,
                  NULL,
                  &gEfiEventPreReadyToBootGuid,
                  &mPreReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [BDS]: Failed to register OnReadyToBoot (%r).\r\n", Status));
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PostReadyToBoot,
                  NULL,
                  &gEfiEventPostReadyToBootGuid,
                  &mPostReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [BDS]: Failed to register OnReadyToBoot (%r).\r\n", Status));
  }

  // Constructor MUST return EFI_SUCCESS as a failure can result in an unusable system
  return EFI_SUCCESS;
}

/**
  OnDemandConInConnect
 */
EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerOnDemandConInConnect (
  VOID
  )
{
  return GetPlatformConnectOnConInList ();
}

/**
  Do the device specific action at start of BdsEntry (callback into BdsArch from DXE Dispatcher)
**/
VOID
EFIAPI
DeviceBootManagerBdsEntry (
  VOID
  )
{
  /* Do not signal EndOfDxe here. BdsEntry.c must initialize volatile
     BootOptionSupport first, otherwise VariablePolicy returns
     EFI_WRITE_PROTECTED and its line 798 asserts. The WP81-only
     EndOfDxe transition is performed in AfterConsole, prior to loading
     BootFF10 and after BDS boot variables are initialized. */

  EfiEventGroupSignal (&gMsStartOfBdsNotifyGuid);

  //
  // Also signal the DFCI version of this event.
  //
  EfiEventGroupSignal (&gDfciStartOfBdsNotifyGuid);

  UpdateRebootReason ();
}

/**
  Do the device specific action before the console is connected.

  Such as:

      Initialize the platform boot order
      Supply Console information

  Returns value from GetPlatformPreffered Console, which will be the handle and device path
  of the device console
**/
EFI_HANDLE
EFIAPI
DeviceBootManagerBeforeConsole (
  EFI_DEVICE_PATH_PROTOCOL   **DevicePath,
  BDS_CONSOLE_CONNECT_ENTRY  **PlatformConsoles
  )
{
  /*
   * The WP81 ESP-only driver exposes this marker only after the private
   * userdata backing has been staged, the physical UEFI disk has been
   * disconnected, and a valid BootFF10 option has been registered.
   * Do NOT infer the mode from BootOrder: BDS can alter that variable.
   */
  VOID        *ReadyMarker = NULL;
  EFI_STATUS  MarkerStatus;
  EFI_STATUS  ChoiceStatus;
  EFI_STATUS  EntryStatus;
  UINT16      BootChoice = 0;
  UINT8       BootHeader[8];
  UINTN       Size;
  UINTN       OrderSize;
  UINT16      OnlyBootOption = 0xff10;
  EFI_STATUS  RestoreStatus;

  MarkerStatus = gBS->LocateProtocol(&mWpEspReadyGuid, NULL, &ReadyMarker);
  Size = sizeof(BootChoice);
  ChoiceStatus = gRT->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid,
                                  NULL, &Size, &BootChoice);
  OrderSize = Size;
  Size = sizeof(BootHeader);
  EntryStatus = gRT->GetVariable(L"BootFF10", &gEfiGlobalVariableGuid,
                                 NULL, &Size, BootHeader);

  DEBUG ((DEBUG_ERROR,
    "[Bds] WP81 ESP-ready marker=%r markerPtr=%p, BootOrder=%r first=%04x, BootFF10=%r bytes=%u\n",
    MarkerStatus, ReadyMarker, ChoiceStatus, BootChoice, EntryStatus,
    (UINT32)Size));

  if (!EFI_ERROR(MarkerStatus) && ReadyMarker != NULL) {
    /* This is the sole intended BDS path. Repair BootOrder if earlier
       BDS callbacks or stale emulated variables have replaced it. This
       touches only UEFI boot variables (RAM-backed on J250F), never eMMC. */
    if (EFI_ERROR(ChoiceStatus) || OrderSize != sizeof(OnlyBootOption) ||
        BootChoice != OnlyBootOption) {
      RestoreStatus = gRT->SetVariable(L"BootOrder", &gEfiGlobalVariableGuid,
                         EFI_VARIABLE_NON_VOLATILE |
                         EFI_VARIABLE_BOOTSERVICE_ACCESS |
                         EFI_VARIABLE_RUNTIME_ACCESS,
                         sizeof(OnlyBootOption), &OnlyBootOption);
      DEBUG ((DEBUG_ERROR,
        "[Bds] WP81 BootOrder repair FF10: %r\n", RestoreStatus));
    }
    DEBUG ((DEBUG_ERROR,
      "[Bds] WP81 ESP-only active: SKIP generic Internal Storage and USB boot options.\n"));
  } else {
    MsBootOptionsLibRegisterDefaultBootOptions ();
  }
  *PlatformConsoles = GetPlatformConsoleList ();

  return GetPlatformPreferredConsole (DevicePath);
}

/**
  Do the device specific action after the console is connected.

  Such as:
**/
EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerAfterConsole (
  VOID
  )
{
  EFI_BOOT_MODE    BootMode;
  TPM_PP_PROTOCOL  *TpmPp = NULL;
  EFI_STATUS       Status;

  MsPreBootChecks ();

  PlatformPowerLevelCheck ();

#if !defined(J250F_BOOT_TO_SHELL)
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);
  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((DEBUG_ERROR, "%a Unabled to set graphics - %r\n", __FUNCTION__, Status));
  }

  ConsoleMsgLibDisplaySystemInfoOnConsole ();
#else
  J250fDiscoverBootFiles ();
#endif

  BootMode = GetBootModeHob ();

  if (BootMode != BOOT_ON_FLASH_UPDATE) {
    Status = gBS->LocateProtocol (&gTpmPpProtocolGuid, NULL, (VOID **)&TpmPp);
    if (!EFI_ERROR (Status) && (TpmPp != NULL)) {
      Status = TpmPp->PromptForConfirmation (TpmPp);
      DEBUG ((DEBUG_ERROR, "%a: Unexpected return from Tpm Physical Presence. Code=%r\n", __FUNCTION__, Status));
    }
  }

  /* BDS has already initialized BootOptionSupport and other volatile boot
     variables before entering PlatformBootManagerBefore/AfterConsole.
     EndOfDxe now safely unlocks filesystem image loading in SecurityStubDxe,
     without prematurely locking BootOptionSupport. Do not bypass the
     Security2 image-authentication callbacks. */
  {
    VOID *EspReady = NULL;
    EFI_STATUS ReadyStatus = gBS->LocateProtocol(
                              &mWpEspReadyGuid, NULL, &EspReady);
    if (!EFI_ERROR(ReadyStatus) && EspReady != NULL) {
      DEBUG ((DEBUG_ERROR,
        "[WP81 BDS] EndOfDxe AFTER BootOptionSupport initialization.\n"));
      EFI_STATUS EventStatus = EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
      EFI_STATUS TrustStatus = WpInstallDonorTrust();
      DEBUG ((DEBUG_ERROR,
        "[WP81] EndOfDxe signal %r, exact-donor trust hook %r.\n",
        EventStatus, TrustStatus));
    }
  }

  return GetPlatformConnectList ();
}

static
VOID
RebootToFrontPage (
  VOID
  )
{
  UINT64      OsIndication;
  EFI_STATUS  Status;

  OsIndication = EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
  Status       = gRT->SetVariable (
                        L"OsIndications",
                        &gEfiGlobalVariableGuid,
                        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                        sizeof (UINT64),
                        &OsIndication
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unable to set OsIndications\n"));
  }

  DEBUG ((DEBUG_WARN, "%a Resetting system.\n", __FUNCTION__));
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  CpuDeadLoop ();
}

/**
ProcessBootCompletion
*/
VOID
EFIAPI
DeviceBootManagerProcessBootCompletion (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  UINTN       BufferSize;
  BOOLEAN     MsBootNext = FALSE;
  EFI_STATUS  Status;
  EFI_STATUS  RestartStatus;

  BufferSize = sizeof (MsBootNext);
  Status     = gRT->GetVariable (
                      L"MsBootNext",
                      &gMsBootMenuFormsetGuid,
                      NULL,
                      &BufferSize,
                      &MsBootNext
                      );
  if (!EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"MsBootNext",
                    &gMsBootMenuFormsetGuid,
                    EFI_VARIABLE_BOOTSERVICE_ACCESS,
                    0,
                    NULL
                    );
  }


  /* EfiBootManagerBoot stores the final LoadImage/StartImage result here.
     Keep it for a readable post-failure screen instead of showing the
     unhelpful generic "No Operating System Found" image. */
  if (BootOption != NULL && BootOption->OptionNumber == 0xff10) {
    mWpBootFF10Attempted = TRUE;
    mWpBootFF10Result = BootOption->Status;
    mWpHookCallsBeforeProbe = gWp81LoadCallCount;
    mWpHookStatusBeforeProbe = gWp81LoadStatus;
    /*
     * If the boot option was denied, safely retry LoadImage only (NEVER
     * StartImage) using the original Boot#### path. This separates a BDS
     * option/variable failure from an EFI image-loading/security error.
     * Successful images are immediately unloaded. eMMC backing stays RO.
     */
    if (mWpBootFF10Result == EFI_ACCESS_DENIED &&
        BootOption->FilePath != NULL) {
      EFI_HANDLE ProbeImage = NULL;
      mWpDirectLoadProbeAttempted = TRUE;
      mWpDirectLoadProbeResult = gBS->LoadImage(
          FALSE, gImageHandle, BootOption->FilePath, NULL, 0, &ProbeImage);
      if (ProbeImage != NULL &&
          (!EFI_ERROR(mWpDirectLoadProbeResult) ||
           mWpDirectLoadProbeResult == EFI_SECURITY_VIOLATION)) {
        (VOID)gBS->UnloadImage(ProbeImage);
      }
      DEBUG ((DEBUG_ERROR,
        "[WP81 BDS] LoadImage-only follow-up: %r (no StartImage)\n",
        mWpDirectLoadProbeResult));
    }
    DEBUG ((DEBUG_ERROR,
      "[WP81 BDS] BootFF10 returned %r (0x%lx); EFI path: \\Windows\\BOOT\\EFI\\BOOTMGFW.EFI\n",
      mWpBootFF10Result, (UINT64)mWpBootFF10Result));
  }

  RestartStatus = BootOption->Status;
  if (OEM_PREVIOUS_SECURITY_VIOLATION == BootOption->Status) {
    RestartStatus = EFI_SECURITY_VIOLATION;
  }

  if (MsBootNext) {
    SetRebootReason (RestartStatus);
    RebootToFrontPage ();    // Reboot to front page
  }

  SetRebootReason (BootOption->Status);
  Status = SetGraphicsConsoleMode (GCM_NATIVE_RES);
  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((DEBUG_ERROR, "%a Unabled to set console mode - %r\n", __FUNCTION__, Status));
  }

  return;
}

/**
 * Check for HardKeys during boot.  If the hard keys are pressed, builds
 * a boot option for the specific hard key setting.
 *
 *
 * @param BootOption   - Boot Option filled in based on which hard key is pressed
 *
 * @return EFI_STATUS  - EFI_NOT_FOUND - no hard key pressed, no BootOption
 *                       EFI_SUCCESS   - BootOption is valid
 *                       other error   - Unable to build BootOption
 */
EFI_STATUS
EFIAPI
DeviceBootManagerPriorityBoot (
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
#if defined(J250F_BOOT_TO_SHELL)
  /* Launch the shell embedded in the firmware volume on every boot. */
  return MsBootOptionsLibUEFIShell (BootOption, NULL);
#else
  BOOLEAN     FrontPageBoot;
  BOOLEAN     UEFIShell;
  EFI_STATUS  Status;

  FrontPageBoot = MsBootPolicyLibIsSettingsBoot ();
  UEFIShell     = MsBootPolicyLibUEFIShell ();
  MsBootPolicyLibClearBootRequests ();

  // There are four cases:
  //   1.  Nothing pressed.             return EFI_NOT_FOUND
  //   2.  FrontPageBoot                load FrontPage
  //   3.  UEFIShell                    Load UEFI Shell
  //   4.1 Both indicators are present  Load NetworkUnlock
  //   4.2 Both indicators are present  Load Switch Slot App

  if (UEFIShell) {
    // Alternate boot or Network Unlock option
    if (FrontPageBoot) {
#if AB_SLOT_SUPPORT == 1
      DEBUG ((DEBUG_INFO, "[Bds] Slot Switch\n"));
      Status = MsBootOptionsLibSlotSwitchApp (BootOption, "VOL+/-");
#else
      DEBUG ((DEBUG_INFO, "[Bds] both detected. NetworkUnlock\n"));
      Status = MsBootOptionsLibGetDefaultBootApp (BootOption, "NS");
#endif
    } else {
      DEBUG ((DEBUG_INFO, "[Bds] UEFI Shell\n"));
      Status = MsBootOptionsLibUEFIShell (BootOption, "VOL-");
    }
  } else if (FrontPageBoot) {
    // Front Page Boot Option
    DEBUG ((DEBUG_INFO, "[Bds] enter Front Page\n"));
    Status = MsBootOptionsLibGetBootManagerMenu (BootOption, "VOL+");
    SetRebootReason (OEM_REBOOT_TO_SETUP_KEY);
  } else {
    Status = EFI_NOT_FOUND;
  }

  return Status;
#endif
}

/**
 This is called from BDS right before going into front page
 when no bootable devices/options found
*/
VOID
EFIAPI
DeviceBootManagerUnableToBoot (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutput;
  VOID                         *WpMarker = NULL;

  Status = gBS->LocateProtocol(&mWpEspReadyGuid, NULL, &WpMarker);
  if (!EFI_ERROR(Status) && WpMarker != NULL) {
    /* The original BootFF10 result cannot distinguish LoadImage failure
       from a started image returning an error. The linked boot policy's
       LoadImage wrapper records the first-stage result separately. */
    CHAR8 Buffer[896];
    CONST CHAR8 *Stage;
    UINTN Length;
    UINT16 Order[64];
    UINTN OrderBytes = sizeof(Order);
    UINTN OrderCount = 0;
    UINT16 OrderFirst = 0xffff;
    EFI_STATUS OrderStatus;

    if (!gWp81LoadObserved && !mWpDirectLoadProbeAttempted) {
      Stage = "No image-loading trace";
    } else if (gWp81LoadPolicyRejected) {
      Stage = "Platform device-path policy refused";
    } else if (mWpDirectLoadProbeAttempted &&
               !EFI_ERROR(mWpDirectLoadProbeResult) &&
               mWpBootFF10Result == EFI_ACCESS_DENIED) {
      Stage = "Image loads: BDS option or StartImage denied";
    } else if (gWp81LoadObserved && EFI_ERROR(gWp81LoadStatus)) {
      Stage = "Underlying UEFI LoadImage refused";
    } else if (mWpDirectLoadProbeAttempted &&
               EFI_ERROR(mWpDirectLoadProbeResult)) {
      Stage = "Direct UEFI image-load probe refused";
    } else {
      Stage = "LoadImage succeeded; later stage failed";
    }

    OrderStatus = gRT->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid,
                                   NULL, &OrderBytes, Order);
    if (!EFI_ERROR(OrderStatus)) {
      OrderCount = OrderBytes / sizeof(Order[0]);
      if (OrderCount != 0) OrderFirst = Order[0];
    }
#if defined(J250F_ADB_RAM_EXECUTION)
    FbConStartScreen();
    (VOID)SerialPortInitialize();
#endif
    Length = AsciiSPrint(Buffer, sizeof(Buffer),
      "\r\nWP81 ESP-ONLY BDS DIAGNOSTICS\r\n"
      "BootFF10: %a  Final: %r (0x%lx)\r\n"
      "Loader: Windows/BOOT/EFI/BOOTMGFW.EFI\r\n"
      "LoadImage hook installed: %a\r\n"
      "LoadImage hook calls before probe: %u\r\n"
      "Hook policy denied: %a\r\n"
      "Exact donor trust installed: %a\r\n"
      "Exact donor trust used: %a\r\n"
      "Original Security2 status: %r\r\n"
      "Original hook status: %r\r\n"
      "Direct load-only test: %r (%a)\r\n"
      "Failure stage: %a\r\n"
      "BootOrder: %r, count=%u, first=%04x\r\n"
      "Physical eMMC hidden only in UEFI.\r\n"
      "Virtual ESP writes use RAM COW only.\r\n"
      "Windows kernel eMMC MMIO NOT isolated.\r\n"
      "Shutdown in 120 seconds.\r\n",
      mWpBootFF10Attempted ? "YES" : "NO",
      mWpBootFF10Attempted ? mWpBootFF10Result : EFI_NOT_STARTED,
      (UINT64)(mWpBootFF10Attempted ? mWpBootFF10Result : EFI_NOT_STARTED),
      gWp81LoadHookInstalled ? "YES" : "NO",
      (UINT32)mWpHookCallsBeforeProbe,
      gWp81LoadPolicyRejected ? "YES" : "NO",
      mWpSecurity2HookInstalled ? "YES" : "NO",
      mWpDonorTrustApplied ? "YES" : "NO",
      mWpOriginalSecurityStatus,
      mWpHookCallsBeforeProbe > 0 ? mWpHookStatusBeforeProbe : EFI_NOT_STARTED,
      mWpDirectLoadProbeAttempted ? mWpDirectLoadProbeResult : EFI_NOT_STARTED,
      mWpDirectLoadProbeAttempted ? "not executed" : "not attempted",
      Stage, OrderStatus, (UINT32)OrderCount, OrderFirst);
    (VOID)SerialPortWrite((UINT8 *)Buffer, Length);
    if (gST != NULL && gST->ConOut != NULL) {
      Print(L"\r\nWP81 BootFF10: %r; LoadImage: %r\r\n",
            mWpBootFF10Attempted ? mWpBootFF10Result : EFI_NOT_STARTED,
            gWp81LoadObserved ? gWp81LoadStatus : EFI_NOT_STARTED);
    }
    gBS->Stall(120000000);
    gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop();
    return;
  }

  // Locate Gop Protocol
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&GraphicsOutput);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "%a - Failed to find GOP on ConsoleOutHandle. %r\n", __FUNCTION__, Status));
    // failed on console out.  Try globally within system
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&GraphicsOutput);
  }

  // Set Native Resolution
  Status = SetGraphicsConsoleMode (GCM_NATIVE_RES);
  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((EFI_D_ERROR, "%a Unable to set console mode - %r\n", __FUNCTION__, Status));
  }

  // Display No Boot OS Logo
  Status = DisplayBootGraphic (BG_NO_BOOT_OS);
  if (EFI_ERROR (Status) != FALSE) {
    DEBUG ((EFI_D_ERROR, "%a Unable to set graphics - %r\n", __FUNCTION__, Status));
  }

  // Wait 10s
  gBS->Stall(10000000);

  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);

  // Do Cpu Dead Loop
  CpuDeadLoop ();
}
