/*
 * inject_test.c - Userland CLI tool for mt7922_awdl.sys validation
 *
 * Usage:
 *   inject_test status
 *   inject_test channel <ch>          (44 or 149)
 *   inject_test inject <hex_frame>    (raw 802.11 frame bytes as hex string)
 *   inject_test read   [timeout_ms]   (read and dump RX frames)
 *   inject_test awdltest              (inject a minimal AWDL Action Frame)
 *
 * Compile (MSVC):
 *   cl /W4 /O2 inject_test.c /link /out:inject_test.exe
 *
 * SPDX-License-Identifier: MIT
 */

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")

/* ---- IOCTL codes (must match mt7922_awdl.h) ---- */
#define AWDL_IOCTL_TYPE 0x9944UL
#define AWDL_IOCTL_BASE 0x800UL

#define CTL(fn, acc)                                                           \
  CTL_CODE(AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + (fn), METHOD_BUFFERED, (acc))

#define IOCTL_AWDL_SET_CHANNEL CTL(0, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_AWDL_INJECT_FRAME CTL(1, FILE_WRITE_DATA)
#define IOCTL_AWDL_READ_FRAMES CTL(2, FILE_READ_DATA)
#define IOCTL_AWDL_GET_STATUS CTL(3, FILE_READ_DATA)

/* Device path */
#define DEVICE_PATH L"\\\\.\\AwdlRadioDevice"

/* ---- Payload structures (must match mt7922_awdl.h) ---- */
#pragma pack(push, 1)
typedef struct {
  uint8_t Channel;
  uint8_t Band;
  uint8_t Width;
  uint8_t Reserved;
} SET_CHANNEL_IN;
typedef struct {
  uint32_t FrameLength;
  uint8_t NoAck;
  uint8_t Reserved[3];
  uint8_t FrameData[1];
} INJECT_FRAME_IN;
typedef struct {
  uint32_t TotalLength;
  uint64_t Timestamp;
  int8_t Rssi;
  uint8_t Channel;
  uint8_t Reserved[2];
  uint8_t FrameData[1];
} RX_FRAME_HDR;
typedef struct {
  uint32_t ChipId;
  uint32_t FirmwareVersion;
  uint8_t FirmwareLoaded;
  uint8_t RadioEnabled;
  uint8_t CurrentChannel;
  uint8_t Reserved;
} DRIVER_STATUS;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Open the AWDL device
 * ------------------------------------------------------------------------- */
static HANDLE OpenDevice(void) {
  HANDLE h = CreateFileW(DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "[!] Cannot open %ls: error %lu\n", DEVICE_PATH,
            GetLastError());
  }
  return h;
}

/* -------------------------------------------------------------------------
 * cmd: status
 * ------------------------------------------------------------------------- */
static int CmdStatus(HANDLE h) {
  DRIVER_STATUS st = {0};
  DWORD bytes = 0;

  if (!DeviceIoControl(h, IOCTL_AWDL_GET_STATUS, NULL, 0, &st, sizeof(st),
                       &bytes, NULL)) {
    fprintf(stderr, "[!] GET_STATUS ioctl failed: %lu\n", GetLastError());
    return 1;
  }

  printf("=== MT7922 AWDL Driver Status ===\n");
  printf("  ChipId          : 0x%08X %s\n", st.ChipId,
         (st.ChipId >> 16) == 0x7922 ? "(MT7922 OK)" : "(UNKNOWN!)");
  printf("  Firmware Version: 0x%08X\n", st.FirmwareVersion);
  printf("  Firmware Loaded : %s\n", st.FirmwareLoaded ? "YES" : "NO");
  printf("  Radio Enabled   : %s\n", st.RadioEnabled ? "YES" : "NO");
  printf("  Current Channel : %u\n", st.CurrentChannel);
  return 0;
}

/* -------------------------------------------------------------------------
 * cmd: channel <num>
 * ------------------------------------------------------------------------- */
static int CmdChannel(HANDLE h, int ch) {
  SET_CHANNEL_IN in = {0};
  DWORD bytes = 0;

  if (ch != 44 && ch != 149) {
    fprintf(stderr, "[!] AWDL social channels are 44 and 149 only.\n");
    return 1;
  }

  in.Channel = (uint8_t)ch;
  in.Band = 1;  /* 5 GHz */
  in.Width = 0; /* 20 MHz */

  if (!DeviceIoControl(h, IOCTL_AWDL_SET_CHANNEL, &in, sizeof(in), NULL, 0,
                       &bytes, NULL)) {
    fprintf(stderr, "[!] SET_CHANNEL ioctl failed: %lu\n", GetLastError());
    return 1;
  }
  printf("[+] Channel switched to %d (5 GHz)\n", ch);
  return 0;
}

/* -------------------------------------------------------------------------
 * Internal: hex string -> byte buffer
 * ------------------------------------------------------------------------- */
static int HexToBuf(const char *hex, uint8_t *buf, size_t bufLen,
                    size_t *outLen) {
  size_t hexLen = strlen(hex);
  size_t i;

  if (hexLen % 2 != 0) {
    fprintf(stderr, "[!] Hex string must have even number of characters\n");
    return 0;
  }
  *outLen = hexLen / 2;
  if (*outLen > bufLen) {
    fprintf(stderr, "[!] Frame too long (%zu bytes, max %zu)\n", *outLen,
            bufLen);
    return 0;
  }
  for (i = 0; i < *outLen; i++) {
    char byte[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
    buf[i] = (uint8_t)strtoul(byte, NULL, 16);
  }
  return 1;
}

/* -------------------------------------------------------------------------
 * cmd: inject <hex_frame>
 * ------------------------------------------------------------------------- */
static int CmdInject(HANDLE h, const char *hexFrame) {
  static uint8_t frameBuf[2048];
  size_t frameLen = 0;
  uint8_t *inBuf;
  size_t inBufSize;
  INJECT_FRAME_IN *in;
  DWORD bytes = 0;

  if (!HexToBuf(hexFrame, frameBuf, sizeof(frameBuf), &frameLen))
    return 1;

  inBufSize = offsetof(INJECT_FRAME_IN, FrameData) + frameLen;
  // Silence MSVC "can't take offsetof non-POD" — use FIELD_OFFSET semantics:
  inBufSize = sizeof(uint32_t) + sizeof(uint8_t) + 3 + frameLen;
  inBuf = (uint8_t *)calloc(1, inBufSize);
  if (!inBuf) {
    fprintf(stderr, "[!] OOM\n");
    return 1;
  }

  in = (INJECT_FRAME_IN *)inBuf;
  in->FrameLength = (uint32_t)frameLen;
  in->NoAck = 1;
  memcpy(in->FrameData, frameBuf, frameLen);

  if (!DeviceIoControl(h, IOCTL_AWDL_INJECT_FRAME, inBuf, (DWORD)inBufSize,
                       NULL, 0, &bytes, NULL)) {
    fprintf(stderr, "[!] INJECT_FRAME ioctl failed: %lu\n", GetLastError());
    free(inBuf);
    return 1;
  }
  printf("[+] Injected %zu bytes (NO_ACK=1)\n", frameLen);
  free(inBuf);
  return 0;
}

/* -------------------------------------------------------------------------
 * cmd: awdltest
 *
 * Builds and injects a minimal AWDL Action Frame (broadcast):
 *   802.11 hdr: FC=0x00D0 (Action, no DS), Dur=0, DA=ff:ff:ff:ff:ff:ff,
 *               SA=00:11:22:33:44:55, BSSID=ff:ff:ff:ff:ff:ff, SeqCtrl=0
 *   Body: Category=0x7F (Vendor-Specific), OUI=00:17:F2 (Apple), Type=0x08
 *         AWDL Version TLV (type=0x15, len=2, value=0x0100)
 * ------------------------------------------------------------------------- */
static int CmdAwdlTest(HANDLE h) {
  static const uint8_t awdlFrame[] = {
      /* 802.11 MAC header (24 bytes) */
      0xD0,
      0x00, /* Frame Control: Management / Action */
      0x00,
      0x00, /* Duration */
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF, /* DA: broadcast */
      0x00,
      0x11,
      0x22,
      0x33,
      0x44,
      0x55, /* SA: our synthetic MAC */
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF, /* BSSID: broadcast */
      0x00,
      0x00, /* Sequence Control */

      /* Action body */
      0x7F, /* Category: Vendor Specific */
      0x00,
      0x17,
      0xF2, /* Apple OUI */
      0x08, /* AWDL Action Type */
      0x00, /* AWDL Action code (PSF = 0) */

      /* AWDL Version TLV: type=0x15 len=2 value=0x0100 */
      0x15,
      0x02,
      0x00,
      0x01,
      0x00,
  };

  size_t frameLen = sizeof(awdlFrame);
  size_t inBufSize = sizeof(uint32_t) + sizeof(uint8_t) + 3 + frameLen;
  uint8_t *inBuf = (uint8_t *)calloc(1, inBufSize);
  INJECT_FRAME_IN *in;
  DWORD bytes = 0;
  size_t i;

  if (!inBuf) {
    fprintf(stderr, "[!] OOM\n");
    return 1;
  }

  in = (INJECT_FRAME_IN *)inBuf;
  in->FrameLength = (uint32_t)frameLen;
  in->NoAck = 1;
  memcpy(in->FrameData, awdlFrame, frameLen);

  printf("[*] Injecting AWDL PSF Action Frame (%zu bytes)...\n", frameLen);
  printf("    Frame: ");
  for (i = 0; i < frameLen; i++)
    printf("%02X ", awdlFrame[i]);
  printf("\n");

  if (!DeviceIoControl(h, IOCTL_AWDL_INJECT_FRAME, inBuf, (DWORD)inBufSize,
                       NULL, 0, &bytes, NULL)) {
    fprintf(stderr, "[!] INJECT_FRAME ioctl failed: %lu\n", GetLastError());
    free(inBuf);
    return 1;
  }
  printf("[+] AWDL test frame injected successfully\n");
  free(inBuf);
  return 0;
}

/* -------------------------------------------------------------------------
 * cmd: read [timeout_ms]
 * Poll for incoming raw 802.11 frames and pretty-print them.
 * ------------------------------------------------------------------------- */
static int CmdRead(HANDLE h, DWORD timeoutMs) {
  static uint8_t rxBuf[64 * 1024];
  DWORD elapsed = 0;
  DWORD poll = 100; /* Poll interval ms */

  printf("[*] Listening for RX frames (timeout=%lu ms)...\n", timeoutMs);

  while (elapsed < timeoutMs) {
    DWORD bytes = 0;

    if (DeviceIoControl(h, IOCTL_AWDL_READ_FRAMES, NULL, 0, rxBuf,
                        sizeof(rxBuf), &bytes, NULL) &&
        bytes > 0) {

      uint8_t *ptr = rxBuf;
      uint8_t *end = rxBuf + bytes;

      while (ptr + sizeof(RX_FRAME_HDR) - 1 < end) {
        RX_FRAME_HDR *hdr = (RX_FRAME_HDR *)ptr;
        uint32_t total = hdr->TotalLength;
        uint32_t hdrSize = (uint32_t)(sizeof(RX_FRAME_HDR) - 1);
        uint32_t frameLen;
        DWORD i;

        if (total < hdrSize || (uint32_t)(end - ptr) < total)
          break;

        frameLen = total - hdrSize;
        printf("--- RX Frame ---\n");
        printf("  Timestamp : %I64u\n", (unsigned long long)hdr->Timestamp);
        printf("  RSSI      : %d dBm\n", hdr->Rssi);
        printf("  Channel   : %u\n", hdr->Channel);
        printf("  Length    : %u bytes\n", frameLen);
        printf("  Data      : ");
        for (i = 0; i < frameLen && i < 64; i++)
          printf("%02X ", hdr->FrameData[i]);
        if (frameLen > 64)
          printf("...");
        printf("\n");

        ptr += total;
      }
    }

    Sleep(poll);
    elapsed += poll;
  }

  printf("[*] Read loop ended.\n");
  return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
  HANDLE h;
  int ret = 0;

  if (argc < 2) {
    printf("inject_test — MT7922 AWDL Driver Validation CLI\n\n");
    printf("Usage:\n");
    printf("  inject_test status\n");
    printf("  inject_test channel <44|149>\n");
    printf("  inject_test inject <hexbytes>   e.g. D00000...\n");
    printf("  inject_test read [timeout_ms]   default 5000\n");
    printf("  inject_test awdltest            send minimal AWDL PSF frame\n");
    return 0;
  }

  h = OpenDevice();
  if (h == INVALID_HANDLE_VALUE)
    return 1;

  if (strcmp(argv[1], "status") == 0) {
    ret = CmdStatus(h);

  } else if (strcmp(argv[1], "channel") == 0 && argc >= 3) {
    ret = CmdChannel(h, atoi(argv[2]));

  } else if (strcmp(argv[1], "inject") == 0 && argc >= 3) {
    ret = CmdInject(h, argv[2]);

  } else if (strcmp(argv[1], "read") == 0) {
    DWORD timeout = argc >= 3 ? (DWORD)atoi(argv[2]) : 5000;
    ret = CmdRead(h, timeout);

  } else if (strcmp(argv[1], "awdltest") == 0) {
    ret = CmdAwdlTest(h);

  } else {
    fprintf(stderr, "[!] Unknown command: %s\n", argv[1]);
    ret = 1;
  }

  CloseHandle(h);
  return ret;
}
