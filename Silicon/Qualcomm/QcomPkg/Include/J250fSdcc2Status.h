// SPDX-License-Identifier: GPL-2.0-only
#ifndef J250F_SDCC2_STATUS_H
#define J250F_SDCC2_STATUS_H

#define J250F_SDCC2_STATUS_GUID \
  {0x9d2e258a,0xc020,0x4180,{0x95,0xe2,0xab,0x45,0x8d,0x55,0x61,0x77}}

typedef struct {
  UINT32 Stage;
  UINT32 Reserved;
  UINT64 Status;
  UINT32 Interrupt;
  UINT32 Response;
  UINT32 Present;
  UINT32 Clock;
  UINT32 Power;
  UINT32 InterruptEnable;
  UINT32 Command;
  UINT32 Cmd55Recovered;
  UINT32 Acmd41Recovered;
  UINT32 Cmd2Retries;
  UINT32 Cmd9Retries;
  UINT32 Cmd9Recovered;
  UINT32 Cmd3Recovered;
  UINT32 Response1;
  UINT32 Response2;
  UINT32 Response3;
} J250F_SDCC2_STATUS;

#endif
