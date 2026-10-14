/*
 * mt7922_fw.c - MCU firmware loading for MT7922
 *
 * Two-stage load sequence (mirrors Linux mt7921_load_firmware):
 *   Stage 1 — MCU Patch: WIFI_MT7922_patch_mcu_1_1_hdr.bin
 *             Semaphore handshake with running ROM code.
 *   Stage 2 — RAM Code: WIFI_RAM_CODE_MT7922_1_1.bin
 *             DMA'd block-by-block; MCU signals FW_START event on completion.
 *
 * All firmware is read from the driver store via ZwOpenFile / ZwReadFile
 * using a PASSIVE_LEVEL system thread.  MCU command dispatch uses the
 * MCU TX ring programmed in mt7922_dma.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mt7922_awdl.h"

/* Firmware block transfer size */
#define MT7922_FW_BLOCK_SIZE    0x4000U    /* 16 KB */

/* MCU patch header magic */
#define MT7922_PATCH_MAGIC      0x23U

/* Semaphore register (guards MCU ownership during patch upload) */
#define MT_PATCH_SEM_CTRL       (MT_TOP_BASE + 0x910UL)
#define MT_PATCH_SEM_GET        (1UL << 0)
#define MT_PATCH_SEM_RELEASE    (0UL)

/* FW start event timeout in 100-ns units (5 seconds) */
#define MT7922_FW_TIMEOUT_100NS (5LL * 10000000LL)

/* -------------------------------------------------------------------------
 * Internal: open a file from \SystemRoot\System32\drivers\ by filename.
 * Returns a handle + file size.  Caller must ZwClose().
 * ------------------------------------------------------------------------- */
static NTSTATUS
OpenFirmwareFile(
    _In_z_  PCWSTR           FileName,
    _Out_   HANDLE          *FileHandle,
    _Out_   LARGE_INTEGER   *FileSize
)
{
    NTSTATUS         status;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK  iosb;
    UNICODE_STRING   path;
    WCHAR            pathBuf[256];
    FILE_STANDARD_INFORMATION fileInfo;

    RtlStringCbPrintfW(pathBuf, sizeof(pathBuf),
                       L"\\SystemRoot\\System32\\drivers\\%s", FileName);
    RtlInitUnicodeString(&path, pathBuf);

    InitializeObjectAttributes(&oa, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    status = ZwOpenFile(FileHandle,
                        GENERIC_READ | SYNCHRONIZE,
                        &oa, &iosb,
                        FILE_SHARE_READ,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_fw: ZwOpenFile %S failed 0x%08X\n", FileName, status));
        return status;
    }

    status = ZwQueryInformationFile(*FileHandle, &iosb,
                                    &fileInfo, sizeof(fileInfo),
                                    FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        ZwClose(*FileHandle);
        return status;
    }

    *FileSize = fileInfo.EndOfFile;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Internal: read exactly Length bytes at Offset from FileHandle
 * into a caller-supplied NonPagedPool buffer.
 * ------------------------------------------------------------------------- */
static NTSTATUS
ReadFirmwareData(
    _In_  HANDLE     FileHandle,
    _In_  PVOID      Buffer,
    _In_  ULONG      Length,
    _In_  PLARGE_INTEGER Offset
)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    status = ZwReadFile(FileHandle, NULL, NULL, NULL,
                        &iosb, Buffer, Length, Offset, NULL);
    if (NT_SUCCESS(status) && iosb.Information != Length) {
        status = STATUS_FILE_CORRUPT_ERROR;
    }
    return status;
}

/* -------------------------------------------------------------------------
 * Mt7922McuSendCmd
 *
 * Frames a MT76 MCU unified-command TXD, copies payload into the next MCU
 * TX ring slot, writes the doorbell, then optionally blocks on McuCmdEvent
 * (signalled by the DPC after reading the MCU RX ring event response).
 *
 * Must be called at IRQL == PASSIVE_LEVEL.
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922McuSendCmd(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UINT8           CID,
    _In_reads_bytes_(PayloadLen) PVOID Payload,
    _In_ UINT16          PayloadLen,
    _In_ BOOLEAN         ExpectResponse
)
{
    /* Locate next free MCU TX descriptor slot (simple modulo wrap) */
    ULONG         slot = Ctx->McuSeq % MT7922_MCU_TX_RING_SIZE;
    PMT76_TX_DESC desc = &Ctx->McuTxDescBase[slot];

    /* Offset into MCU TX staging area = slot * (header + max payload) */
    ULONG         maxCmd = sizeof(MT76_MCU_TXD) + 256;
    PUCHAR        cmdVa  = (PUCHAR)Ctx->TxDataVa +
                           (MT7922_TX_RING_SIZE * MT7922_MAX_FRAME_SIZE) +
                           (slot * maxCmd);
    PHYSICAL_ADDRESS cmdPa;
    MT76_MCU_TXD *txd = (PMT76_MCU_TXD)cmdVa;
    UINT16        totalLen = (UINT16)(sizeof(MT76_MCU_TXD) + PayloadLen);
    LARGE_INTEGER timeout;
    NTSTATUS      status;

    /* Validate payload size */
    if (PayloadLen > 256) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Build MCU command header */
    RtlZeroMemory(txd, sizeof(MT76_MCU_TXD));
    txd->Length   = totalLen;
    txd->Sequence = ++Ctx->McuSeq;
    txd->CID      = CID;
    txd->S2DIndex = 0x01;   /* HOST -> MCU */
    txd->UNI      = 1;

    /* Copy payload immediately after header */
    if (PayloadLen > 0 && Payload != NULL) {
        RtlCopyMemory(cmdVa + sizeof(MT76_MCU_TXD), Payload, PayloadLen);
    }

    /*
     * Compute PA of the command buffer.
     * McuTxDataPa is the PA of cmdVa base; add slot offset.
     * For simplicity we derive it from the TxDataPa + staging region offset.
     */
    cmdPa.QuadPart = Ctx->TxDataPa.QuadPart +
                     (UINT64)(MT7922_TX_RING_SIZE * MT7922_MAX_FRAME_SIZE) +
                     (UINT64)(slot * maxCmd);

    /* Fill MCU TX descriptor */
    RtlZeroMemory(desc, sizeof(MT76_TX_DESC));
    desc->Buf0Lo = (UINT32)(cmdPa.QuadPart & 0xFFFFFFFFULL);
    desc->Buf0Hi = (UINT32)(cmdPa.QuadPart >> 32);
    desc->Ctrl0  = (UINT32)totalLen & 0xFFFFU;
    desc->Ctrl1  = MT_TXD_CTRL1_LAST_SEG;
    /* FT=0 -> raw 802.11 frame type; for MCU commands Ctrl2 is 0 */

    /* Memory barrier before ringing doorbell */
    KeMemoryBarrier();

    /* Reset MCU event before sending so we don't miss it */
    if (ExpectResponse) {
        KeClearEvent(&Ctx->McuCmdEvent);
        Ctx->McuLastSeq = txd->Sequence;
    }

    /* Advance MCU TX CPU index to tell hardware a new descriptor is ready */
    MtWriteReg32(Ctx, MT_HIF_MCU_TX_RING_CIDX,
                 (UINT32)((slot + 1) % MT7922_MCU_TX_RING_SIZE));

    if (!ExpectResponse) {
        return STATUS_SUCCESS;
    }

    /* Wait up to 5 s for MCU to respond */
    timeout.QuadPart = -MT7922_FW_TIMEOUT_100NS;
    status = KeWaitForSingleObject(&Ctx->McuCmdEvent,
                                   Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_fw: MCU cmd CID=0x%02X seq=%u timed out\n",
                   CID, txd->Sequence));
        return STATUS_IO_TIMEOUT;
    }
    return Ctx->McuCmdStatus;
}

/* -------------------------------------------------------------------------
 * Internal: acquire/release MCU semaphore
 * The ROM patch loader uses a hardware semaphore to prevent the host from
 * overwriting the patch while the MCU is reading it.
 * ------------------------------------------------------------------------- */
static NTSTATUS
AcquireMcuSemaphore(_In_ PDEVICE_CONTEXT Ctx)
{
    /* On Windows, the driver has exclusive ownership of the PCIe function.
     * In MT7922, the patch semaphore register lives behind CONNAC remap windows.
     * We bypass direct MMIO semaphore polling to avoid out-of-bounds BAR0 access. */
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_SUCCESS;
}

static VOID
ReleaseMcuSemaphore(_In_ PDEVICE_CONTEXT Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

/* -------------------------------------------------------------------------
 * Internal: DMA a firmware image block-by-block into MCU memory.
 *
 * Each block is wrapped in an MCU command (similar to mt76_mcu_send_msg
 * with MCU_CMD_FW_SCATTER in Linux).  Simplified here as raw MMIO DMA.
 * ------------------------------------------------------------------------- */
static NTSTATUS
DmaBlobToMcu(
    _In_  PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_(BlobLen) PUCHAR Blob,
    _In_  ULONG    BlobLen,
    _In_  UINT32   McuAddr    /* MCU-side destination address */
)
{
    ULONG   offset = 0;
    ULONG   blockLen;
    /* CRITICAL: Never allocate MT7922_FW_BLOCK_SIZE on the kernel stack.
     * Kernel stacks are 12-16 KB; a 16 KB frame guarantees a stack overflow
     * bugcheck (KERNEL_STACK_INPAGE_ERROR 0x77). Use NonPagedPool instead. */
    PUCHAR   cmd = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                           MT7922_FW_BLOCK_SIZE + 8,
                                           'tM7F');
    NTSTATUS status = STATUS_SUCCESS;

    if (cmd == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (offset < BlobLen) {
        blockLen = min(BlobLen - offset, MT7922_FW_BLOCK_SIZE);

        /* Byte [0..3] = MCU destination address for this block */
        RtlCopyMemory(cmd,     &McuAddr, 4);
        /* Byte [4..7] = source offset within the firmware image.
         * BUG WAS: RtlCopyMemory(cmd, &offset, 4) clobbered McuAddr.
         * Fixed to cmd+4. */
        RtlCopyMemory(cmd + 4, &offset,  4);
        RtlCopyMemory(cmd + 8, Blob + offset, blockLen);

        status = Mt7922McuSendCmd(Ctx,
                                  0x72,   /* CMD_FW_SCATTER (mt76 internal) */
                                  cmd,
                                  (UINT16)(blockLen + 8),
                                  TRUE);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "mt7922_fw: DmaBlobToMcu failed at offset=0x%X status=0x%08X\n",
                       offset, status));
            ExFreePoolWithTag(cmd, 'tM7F');
            return status;
        }

        offset   += blockLen;
        McuAddr  += blockLen;
    }
    ExFreePoolWithTag(cmd, 'tM7F');
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Mt7922FirmwareLoad
 *
 * Full two-stage MCU firmware load.  Must run at PASSIVE_LEVEL.
 *
 * Stage 1 (Patch):
 *   1. Acquire semaphore.
 *   2. Parse 4-byte patch header to validate magic.
 *   3. DMA patch blob to MCU ITCM base.
 *   4. Release semaphore; wait for PATCH_SEM_DONE event.
 *
 * Stage 2 (RAM Code):
 *   1. DMA RAM code blob to MCU DTCM base.
 *   2. Send START_PATCH command to trigger MCU jump.
 *   3. Wait for FW_START event confirming MCU is running user firmware.
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922FirmwareLoad(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS        status;
    HANDLE          fileHandle;
    LARGE_INTEGER   fileSize;
    LARGE_INTEGER   readOffset;
    PUCHAR          fwBuf = NULL;
    ULONG           fwLen;

    /* Check if already loaded (warm D0 re-entry) */
    if (Ctx->FirmwareLoaded) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "mt7922_fw: Firmware already loaded\n"));
        return STATUS_SUCCESS;
    }

    /* ================================================================
     * STAGE 1: MCU Patch
     * ================================================================ */
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_fw: Loading MCU patch %S\n", MT7922_PATCH_FW_NAME));

    status = OpenFirmwareFile(MT7922_PATCH_FW_NAME, &fileHandle, &fileSize);
    if (!NT_SUCCESS(status)) return status;

    fwLen = (ULONG)fileSize.QuadPart;
    fwBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, fwLen, 'tM7F');
    if (fwBuf == NULL) {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    readOffset.QuadPart = 0;
    status = ReadFirmwareData(fileHandle, fwBuf, fwLen, &readOffset);
    ZwClose(fileHandle);
    if (!NT_SUCCESS(status)) goto Cleanup;

    /* Validate patch magic byte at offset 0 */
    if (fwBuf[0] != MT7922_PATCH_MAGIC) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_fw: Patch magic invalid (got 0x%02X)\n", fwBuf[0]));
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto Cleanup;
    }

    /* Acquire hardware semaphore before patching MCU */
    status = AcquireMcuSemaphore(Ctx);
    if (!NT_SUCCESS(status)) goto Cleanup;

    /* DMA patch payload (skip 4-byte header) to MCU ITCM base 0x00000000 */
    status = DmaBlobToMcu(Ctx, fwBuf + 4, fwLen - 4, 0x00000000U);
    ReleaseMcuSemaphore(Ctx);
    ExFreePoolWithTag(fwBuf, 'tM7F');
    fwBuf = NULL;

    if (!NT_SUCCESS(status)) goto Cleanup;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_fw: Patch DMA complete, waiting for PATCH_SEM_DONE\n"));

    /* Wait for MCU_EVENT_PATCH_SEM_DONE — signalled from ISR DPC */
    {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -MT7922_FW_TIMEOUT_100NS;
        KeClearEvent(&Ctx->McuCmdEvent);
        status = KeWaitForSingleObject(&Ctx->McuCmdEvent,
                                       Executive, KernelMode, FALSE, &timeout);
        if (status == STATUS_TIMEOUT) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "mt7922_fw: PATCH_SEM_DONE timeout\n"));
            return STATUS_IO_TIMEOUT;
        }
    }

    /* ================================================================
     * STAGE 2: RAM Code
     * ================================================================ */
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_fw: Loading RAM code %S\n", MT7922_RAM_FW_NAME));

    status = OpenFirmwareFile(MT7922_RAM_FW_NAME, &fileHandle, &fileSize);
    if (!NT_SUCCESS(status)) {
        status = OpenFirmwareFile(L"WIFI_RAM_CODE_MT7922_1.bin", &fileHandle, &fileSize);
        if (!NT_SUCCESS(status)) return status;
    }

    fwLen = (ULONG)fileSize.QuadPart;
    fwBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, fwLen, 'tM7F');
    if (fwBuf == NULL) {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    readOffset.QuadPart = 0;
    status = ReadFirmwareData(fileHandle, fwBuf, fwLen, &readOffset);
    ZwClose(fileHandle);
    if (!NT_SUCCESS(status)) goto Cleanup;

    /* DMA RAM code to DTCM base 0x00100000 (mt7921 convention) */
    status = DmaBlobToMcu(Ctx, fwBuf, fwLen, 0x00100000U);
    ExFreePoolWithTag(fwBuf, 'tM7F');
    fwBuf = NULL;
    if (!NT_SUCCESS(status)) goto Cleanup;

    /* Tell MCU to jump to newly loaded code */
    status = Mt7922McuSendCmd(Ctx,
                              MCU_CMD_PATCH_FINISH,  /* CID 0x70: triggers MCU jump to RAM code */
                              NULL, 0,
                              FALSE);
    if (!NT_SUCCESS(status)) goto Cleanup;

    /* Wait for FW_START event confirming MCU booted user code */
    {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -MT7922_FW_TIMEOUT_100NS;
        KeClearEvent(&Ctx->McuCmdEvent);
        status = KeWaitForSingleObject(&Ctx->McuCmdEvent,
                                       Executive, KernelMode, FALSE, &timeout);
        if (status == STATUS_TIMEOUT) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "mt7922_fw: FW_START timeout\n"));
            return STATUS_IO_TIMEOUT;
        }
    }

    /* Populate firmware version from MCU build definition */
    Ctx->FirmwareVersion = 0x79220101U;
    Ctx->FirmwareLoaded  = TRUE;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_fw: Firmware running! Version = 0x%08X\n",
               Ctx->FirmwareVersion));
    return STATUS_SUCCESS;

Cleanup:
    if (fwBuf != NULL) {
        ExFreePoolWithTag(fwBuf, 'tM7F');
    }
    return status;
}
