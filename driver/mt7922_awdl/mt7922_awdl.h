/*
 * mt7922_awdl.h - KMDF PCIe driver for MediaTek MT7922 / AMD RZ616
 *                 AWDL (Apple Wireless Direct Link) radio control
 *
 * Hardware: MT7922 (PCIe VID:0x14C3 PID:0x7922 | AMD RZ616 PID:0x0616)
 * Target OS: Windows 11 x64, WDK 10.0.26100+
 * Driver Model: Kernel-Mode Driver Framework (KMDF) 1.33+
 *
 * Reference: Linux mt76/mt7921 driver
 *   drivers/net/wireless/mediatek/mt76/mt7921/
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>

VOID WinDropLog(_In_z_ _Printf_format_string_ const char *Format, ...);

/* --------------------------------------------------------------------------
 * PCI Identity
 * -------------------------------------------------------------------------- */
#define MT7922_VENDOR_ID        0x14C3U
#define MT7922_DEVICE_ID        0x7922U
#define RZ616_DEVICE_ID         0x0616U      /* AMD-branded variant */

/* --------------------------------------------------------------------------
 * IOCTL Interface  \\.\AwdlRadioDevice
 * -------------------------------------------------------------------------- */
DEFINE_GUID(GUID_DEVINTERFACE_AWDL_RADIO,
    0xb5a4a6a0, 0x8f3c, 0x4e1d, 0x9e, 0x2b, 0xc7, 0x1f, 0x3a, 0x88, 0x0d, 0x44);

#define AWDL_IOCTL_TYPE         0x9944UL
#define AWDL_IOCTL_BASE         0x800UL

#define IOCTL_AWDL_SET_CHANNEL  \
    CTL_CODE(AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 0, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_AWDL_INJECT_FRAME \
    CTL_CODE(AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_AWDL_READ_FRAMES  \
    CTL_CODE(AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_AWDL_GET_STATUS   \
    CTL_CODE(AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_READ_DATA)

/* --------------------------------------------------------------------------
 * IOCTL Payload Structures (shared kernel / userland)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)

/** IOCTL_AWDL_SET_CHANNEL input */
typedef struct _AWDL_SET_CHANNEL_IN {
    UINT8   Channel;          /* 802.11 channel number (e.g., 44, 149) */
    UINT8   Band;             /* 0 = 2.4 GHz, 1 = 5 GHz */
    UINT8   Width;            /* 0=20 MHz, 1=40 MHz, 2=80 MHz */
    UINT8   Reserved;
} AWDL_SET_CHANNEL_IN, *PAWDL_SET_CHANNEL_IN;

/** IOCTL_AWDL_INJECT_FRAME input - raw 802.11 frame bytes follow header */
typedef struct _AWDL_INJECT_FRAME_IN {
    UINT32  FrameLength;      /* Bytes of frame data that follow */
    UINT8   NoAck;            /* 1 = set NO_ACK policy bit */
    UINT8   Reserved[3];
    UINT8   FrameData[1];     /* Variable-length - allocate FrameLength bytes */
} AWDL_INJECT_FRAME_IN, *PAWDL_INJECT_FRAME_IN;

/** IOCTL_AWDL_READ_FRAMES output - caller provides buffer, driver fills */
typedef struct _AWDL_RX_FRAME_HDR {
    UINT32  TotalLength;      /* Length of this header + FrameData */
    UINT64  Timestamp;        /* QueryPerformanceCounter tick at receive */
    INT8    Rssi;             /* Signal strength dBm */
    UINT8   Channel;
    UINT8   Reserved[2];
    UINT8   FrameData[1];
} AWDL_RX_FRAME_HDR, *PAWDL_RX_FRAME_HDR;

/** IOCTL_AWDL_GET_STATUS output */
typedef struct _AWDL_DRIVER_STATUS {
    UINT32  ChipId;           /* Value read from MT_TOP_CHIP_ID */
    UINT32  FirmwareVersion;  /* Reported by MCU after patch load */
    UINT8   FirmwareLoaded;   /* 1 if MCU patch + RAM code loaded */
    UINT8   RadioEnabled;     /* 1 if promiscuous monitor active */
    UINT8   CurrentChannel;
    UINT8   Reserved;
} AWDL_DRIVER_STATUS, *PAWDL_DRIVER_STATUS;

#pragma pack(pop)

/* --------------------------------------------------------------------------
 * MT7922 Register Offsets  (BAR0 MMIO)
 * Ported from linux/mt76/mt7921/regs.h
 * -------------------------------------------------------------------------- */

/* Chip/Top-level */
#define MT_TOP_BASE             0x88000000UL
#define MT_TOP_CHIP_ID          (MT_TOP_BASE + 0x8F0UL)
#define MT_TOP_HW_REV           (MT_TOP_BASE + 0x8F4UL)
#define MT_TOP_FW_REV           (MT_TOP_BASE + 0x8F8UL)

/* HIF (Host Interface) - PCIe DMA engine */
#define MT_HIF_BASE             0x4000UL
#define MT_HIF_RST              (MT_HIF_BASE + 0x100UL)
#define MT_HIF_TX_RING0_CTRL0   (MT_HIF_BASE + 0x300UL)  /* TX ring 0 base lo */
#define MT_HIF_TX_RING0_CTRL1   (MT_HIF_BASE + 0x304UL)  /* TX ring 0 base hi */
#define MT_HIF_TX_RING0_CTRL2   (MT_HIF_BASE + 0x308UL)  /* TX ring 0 count   */
#define MT_HIF_TX_RING0_CIDX    (MT_HIF_BASE + 0x30CUL)  /* CPU index         */
#define MT_HIF_TX_RING0_DIDX    (MT_HIF_BASE + 0x310UL)  /* DMA index         */
#define MT_HIF_RX_RING0_CTRL0   (MT_HIF_BASE + 0x500UL)
#define MT_HIF_RX_RING0_CTRL1   (MT_HIF_BASE + 0x504UL)
#define MT_HIF_RX_RING0_CTRL2   (MT_HIF_BASE + 0x508UL)
#define MT_HIF_RX_RING0_CIDX    (MT_HIF_BASE + 0x50CUL)
#define MT_HIF_RX_RING0_DIDX    (MT_HIF_BASE + 0x510UL)

/* Doorbell registers */
#define MT_HIF_TX_RING0_DOORBELL  (MT_HIF_BASE + 0x314UL)
#define MT_HIF_RX_RING0_DOORBELL  (MT_HIF_BASE + 0x514UL)

/* MCU TX ring registers (ring 1, offset +0x200 from data TX ring) */
#define MT_HIF_MCU_TX_RING_CTRL0  (MT_HIF_TX_RING0_CTRL0 + 0x200UL)
#define MT_HIF_MCU_TX_RING_CTRL1  (MT_HIF_TX_RING0_CTRL1 + 0x200UL)
#define MT_HIF_MCU_TX_RING_CTRL2  (MT_HIF_TX_RING0_CTRL2 + 0x200UL)
#define MT_HIF_MCU_TX_RING_CIDX   (MT_HIF_TX_RING0_CIDX  + 0x200UL)
#define MT_HIF_MCU_TX_RING_DIDX   (MT_HIF_TX_RING0_DIDX  + 0x200UL)

/* MCU RX ring registers (ring 1, offset +0x200 from data RX ring) */
#define MT_HIF_MCU_RX_RING_CTRL0  (MT_HIF_RX_RING0_CTRL0 + 0x200UL)
#define MT_HIF_MCU_RX_RING_CTRL1  (MT_HIF_RX_RING0_CTRL1 + 0x200UL)
#define MT_HIF_MCU_RX_RING_CTRL2  (MT_HIF_RX_RING0_CTRL2 + 0x200UL)
#define MT_HIF_MCU_RX_RING_CIDX   (MT_HIF_RX_RING0_CIDX  + 0x200UL)
#define MT_HIF_MCU_RX_RING_DIDX   (MT_HIF_RX_RING0_DIDX  + 0x200UL)

/* MCU (Embedded RISC-V controller) */
#define MT_MCU_BASE             0x7C000000UL
#define MT_MCU_PCIE_REMAP_2     (MT_MCU_BASE + 0x000UL)
#define MT_MCU_CMD              (MT_MCU_BASE + 0x050UL)
#define MT_MCU_CMD_STOP_DMA     (1UL << 0)
#define MT_MCU_CMD_RESET_DONE   (1UL << 1)
#define MT_MCU_CMD_RECOVERY_DONE (1UL << 2)
#define MT_MCU_CMD_NORMAL_STATE (1UL << 3)

/* WFDMA (Wireless Frame DMA) */
#define MT_WFDMA0_BASE          0xD4000UL
#define MT_WFDMA0_RST           (MT_WFDMA0_BASE + 0x100UL)
#define MT_WFDMA0_BUSY_ENA      (MT_WFDMA0_BASE + 0x13CUL)
#define MT_WFDMA0_GLO_CFG       (MT_WFDMA0_BASE + 0x208UL)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN  (1UL << 0)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN  (1UL << 2)
#define MT_WFDMA0_INT_STA       (MT_WFDMA0_BASE + 0x200UL)
#define MT_WFDMA0_INT_ENA       (MT_WFDMA0_BASE + 0x204UL)

/* Interrupt bits */
#define MT_INT_RX_DONE_DATA     (1UL << 16)
#define MT_INT_TX_DONE_MCU      (1UL << 27)
#define MT_INT_MCU_CMD          (1UL << 29)

/* MCU CMD channel / event IDs (mt76_connac2) */
#define MCU_UNI_CMD_CH_SWITCH       0x34U
#define MCU_UNI_CMD_SET_FILTER      0x7EU
#define MCU_CMD_PATCH_FINISH        0x70U   /* triggers MCU jump to loaded FW */
#define MCU_EVENT_PATCH_SEM_DONE    0x05U
#define MCU_EVENT_FW_START          0x01U

/* Firmware filenames (placed in %SystemRoot%\System32\drivers\) */
#define MT7922_PATCH_FW_NAME    L"WIFI_MT7922_patch_mcu_1_1_hdr.bin"
#define MT7922_RAM_FW_NAME      L"WIFI_RAM_CODE_MT7922_1.bin"

/* --------------------------------------------------------------------------
 * DMA Descriptor - mt76_connac2_tx_desc (32 bytes, little-endian)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 4)
typedef struct _MT76_TX_DESC {
    UINT32  Buf0Lo;       /* Buffer 0 low 32-bit PA */
    UINT32  Ctrl0;        /* [15:0]=buf0_len, [31:16]=buf1_len */
    UINT32  Ctrl1;        /* [16]=last_seg, [17]=no_ack, [20]=ft */
    UINT32  Ctrl2;        /* Queue/WCID/TID fields */
    UINT32  Buf1Lo;       /* Buffer 1 low 32-bit PA */
    UINT32  Buf0Hi;       /* Buffer 0 high 32-bit PA (IOVA) */
    UINT32  Buf1Hi;
    UINT32  Reserved;
} MT76_TX_DESC, *PMT76_TX_DESC;

typedef struct _MT76_RX_DESC {
    UINT32  Ctrl0;        /* [15:0]=buf_size, [31]=done */
    UINT32  Ctrl1;        /* [15:0]=pkt_len,  [31:30]=l3l4_done */
    UINT32  BufLo;
    UINT32  BufHi;
} MT76_RX_DESC, *PMT76_RX_DESC;
#pragma pack(pop)

/* Descriptor control field bits */
#define MT_TXD_CTRL1_LAST_SEG   (1UL << 16)
#define MT_TXD_CTRL1_NO_ACK     (1UL << 17)
#define MT_TXD_CTRL1_FT_MASK    (1UL << 20)   /* 0 = 802.11 native frame type */
#define MT_RXD_CTRL0_DONE       (1UL << 31)

/* --------------------------------------------------------------------------
 * Ring sizes - keep as power-of-two
 * -------------------------------------------------------------------------- */
#define MT7922_TX_RING_SIZE     256U
#define MT7922_RX_RING_SIZE     256U
#define MT7922_MCU_TX_RING_SIZE 64U
#define MT7922_MCU_RX_RING_SIZE 64U
#define MT7922_MAX_FRAME_SIZE   2048U      /* bytes per RX slot */

/* --------------------------------------------------------------------------
 * MCU Command / Event framing (mt76_connac2_mcu_rxd)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct _MT76_MCU_TXD {
    UINT16  Length;        /* total payload length including this header */
    UINT8   Sequence;
    UINT8   CID;           /* Command ID */
    UINT8   Reserved[4];
    UINT8   ExtCID;
    UINT8   S2DIndex;      /* 0x01 = HOST -> MCU */
    UINT8   Attr;
    UINT8   UNI;           /* 1 = unified command set */
} MT76_MCU_TXD, *PMT76_MCU_TXD;

typedef struct _MT76_MCU_RXD {
    UINT16  Length;
    UINT8   Sequence;
    UINT8   Eid;           /* Event ID */
    UINT8   Status;        /* 0 = success */
    UINT8   ExtEid;
    UINT8   Reserved[2];
} MT76_MCU_RXD, *PMT76_MCU_RXD;
#pragma pack(pop)

/* --------------------------------------------------------------------------
 * RX Frame FIFO shared between ISR and IOCTL_AWDL_READ_FRAMES
 * Lock-free single-producer / single-consumer byte ring, power-of-2 size.
 * -------------------------------------------------------------------------- */
#define RX_FIFO_SIZE   (16UL * 1024UL)   /* 16 KB - fits within KMDF 64 KB context limit */

typedef struct _RX_FIFO {
    UINT8   Buffer[RX_FIFO_SIZE];
    volatile LONG64  WriteOffset;   /* producer index */
    volatile LONG64  ReadOffset;    /* consumer index */
    KSPIN_LOCK       Lock;          /* guards Write side only */
} RX_FIFO, *PRX_FIFO;

/* --------------------------------------------------------------------------
 * Per-Device Context  (stored in WDF object context)
 * -------------------------------------------------------------------------- */
typedef struct _DEVICE_CONTEXT {
    /* PCI bus interface */
    WDFDEVICE           WdfDevice;
    BUS_INTERFACE_STANDARD  BusInterface;

    /* BAR0 MMIO */
    PHYSICAL_ADDRESS    Bar0PhysBase;
    SIZE_T              Bar0Size;
    PUCHAR              Bar0Va;          /* KernelVirtualAddress of BAR0 */

    /* WDF common DMA resources */
    WDFDMAENABLER       DmaEnabler;

    /* TX ring (data path) */
    WDFCOMMONBUFFER     TxRingBuffer;
    PMT76_TX_DESC       TxDescBase;      /* VA */
    PHYSICAL_ADDRESS    TxDescPa;        /* PA/IOVA */
    WDFCOMMONBUFFER     TxDataBuffer;    /* payload staging */
    PUCHAR              TxDataVa;
    PHYSICAL_ADDRESS    TxDataPa;
    ULONG               TxHead;          /* Next descriptor slot to fill */
    ULONG               TxTail;          /* Last descriptor submitted    */

    /* RX ring (data path) */
    WDFCOMMONBUFFER     RxRingBuffer;
    PMT76_RX_DESC       RxDescBase;
    PHYSICAL_ADDRESS    RxDescPa;
    WDFCOMMONBUFFER     RxDataBuffer;    /* payload pool: RX_RING_SIZE * MAX_FRAME_SIZE */
    PUCHAR              RxDataVa;
    PHYSICAL_ADDRESS    RxDataPa;
    ULONG               RxHead;

    /* MCU command ring (smaller, separate queue) */
    WDFCOMMONBUFFER     McuTxRingBuffer;
    PMT76_TX_DESC       McuTxDescBase;
    PHYSICAL_ADDRESS    McuTxDescPa;
    WDFCOMMONBUFFER     McuRxRingBuffer;
    PMT76_RX_DESC       McuRxDescBase;
    PHYSICAL_ADDRESS    McuRxDescPa;
    UINT8               McuSeq;          /* Rolling sequence number */

    /* Firmware state */
    UINT32              ChipId;
    UINT32              FirmwareVersion;
    BOOLEAN             FirmwareLoaded;

    /* Radio state */
    BOOLEAN             RadioEnabled;
    UINT8               CurrentChannel;

    /* RX FIFO shared with IOCTL consumer */
    RX_FIFO             RxFifo;

    /* Interrupt object */
    WDFINTERRUPT        Interrupt;

    /* Synchronization for MCU command serialization */
    KEVENT              McuCmdEvent;
    UINT8               McuLastSeq;
    NTSTATUS            McuCmdStatus;

    /* WDF I/O queue for IOCTL_AWDL_READ_FRAMES (holds pending IRPs) */
    WDFQUEUE            ReadQueue;

    /* ISR -> DPC scratch: interrupt status bits captured in ISR at DIRQL */
    UINT32              PendingIntSta;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* --------------------------------------------------------------------------
 * Inline MMIO helpers (volatile read/write with memory barrier)
 * -------------------------------------------------------------------------- */
__forceinline UINT32
MtReadReg32(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Ctx->Bar0Va + Offset));
}

__forceinline VOID
MtWriteReg32(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset, _In_ UINT32 Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Ctx->Bar0Va + Offset), Value);
}

/* --------------------------------------------------------------------------
 * Forward declarations - implemented across translation units
 * -------------------------------------------------------------------------- */

/* mt7922_init.c */
EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY         EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          EvtDeviceD0Exit;

/* mt7922_dma.c */
NTSTATUS Mt7922DmaInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     Mt7922DmaFree(_In_ PDEVICE_CONTEXT Ctx);
VOID     Mt7922DmaRxReplenish(_In_ PDEVICE_CONTEXT Ctx);

/* mt7922_fw.c */
NTSTATUS Mt7922FirmwareLoad(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS Mt7922McuSendCmd(_In_ PDEVICE_CONTEXT Ctx,
                          _In_ UINT8 CID,
                          _In_reads_bytes_(PayloadLen) PVOID Payload,
                          _In_ UINT16 PayloadLen,
                          _In_ BOOLEAN ExpectResponse);

/* mt7922_ioctl.c */
VOID     EvtIoDeviceControl(_In_ WDFQUEUE   Queue,
                             _In_ WDFREQUEST Request,
                             _In_ size_t     OutputBufferLength,
                             _In_ size_t     InputBufferLength,
                             _In_ ULONG      IoControlCode);

/* mt7922_isr.c */
EVT_WDF_INTERRUPT_ISR  EvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC  EvtInterruptDpc;
VOID Mt7922ProcessRxRing(_In_ PDEVICE_CONTEXT Ctx);

/* mt7922_radio.c */
NTSTATUS Mt7922SetMonitorMode(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Enable);
NTSTATUS Mt7922SetMacAddress(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(6) PUCHAR MacAddr);
NTSTATUS Mt7922EnableRadio(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT8 InitialChannel, _In_reads_(6) PUCHAR AwdlMac);
NTSTATUS Mt7922DisableRadio(_In_ PDEVICE_CONTEXT Ctx);

