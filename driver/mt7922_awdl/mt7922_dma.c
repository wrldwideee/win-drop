/*
 * mt7922_dma.c - WdfCommonBuffer DMA ring allocation and RX replenishment
 *
 * Allocates four DMA rings:
 *   - TX data ring  (MT7922_TX_RING_SIZE  descriptors)
 *   - RX data ring  (MT7922_RX_RING_SIZE  descriptors)
 *   - MCU TX ring   (MT7922_MCU_TX_RING_SIZE descriptors)
 *   - MCU RX ring   (MT7922_MCU_RX_RING_SIZE descriptors)
 *
 * Plus matching payload staging buffers for TX and RX.
 * Programs the HIF ring-base / count registers and kicks the CPU-index
 * doorbell so the DMA engine knows rings are ready.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mt7922_awdl.h"

/* -------------------------------------------------------------------------
 * Helper: allocate one WdfCommonBuffer and zero it.
 * ------------------------------------------------------------------------- */
static NTSTATUS
AllocCommonBuffer(
    _In_  PDEVICE_CONTEXT   Ctx,
    _In_  SIZE_T             Length,
    _Out_ WDFCOMMONBUFFER   *OutBuf,
    _Out_ PVOID             *OutVa,
    _Out_ PHYSICAL_ADDRESS  *OutPa
)
{
    NTSTATUS status;

    status = WdfCommonBufferCreate(Ctx->DmaEnabler,
                                   Length,
                                   WDF_NO_OBJECT_ATTRIBUTES,
                                   OutBuf);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_dma: WdfCommonBufferCreate len=%zu failed 0x%08X\n",
                   Length, status));
        return status;
    }

    *OutVa = WdfCommonBufferGetAlignedVirtualAddress(*OutBuf);
    *OutPa = WdfCommonBufferGetAlignedLogicalAddress(*OutBuf);
    RtlZeroMemory(*OutVa, Length);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Helper: program one HIF ring register set.
 *   BaseReg  = CTRL0 (base address lo-32)
 *   BaseRegH = CTRL1 (base address hi-32 for 64-bit IOVA)
 *   CntReg   = CTRL2 (descriptor count)
 *   CidxReg  = CPU index register
 * ------------------------------------------------------------------------- */
static VOID
ProgramRingRegs(
    _In_ PDEVICE_CONTEXT    Ctx,
    _In_ ULONG              BaseLoReg,
    _In_ ULONG              BaseHiReg,
    _In_ ULONG              CntReg,
    _In_ ULONG              CidxReg,
    _In_ PHYSICAL_ADDRESS   Pa,
    _In_ UINT32             Count
)
{
    MtWriteReg32(Ctx, BaseLoReg, (UINT32)(Pa.QuadPart & 0xFFFFFFFFULL));
    MtWriteReg32(Ctx, BaseHiReg, (UINT32)(Pa.QuadPart >> 32));
    MtWriteReg32(Ctx, CntReg,    Count);
    MtWriteReg32(Ctx, CidxReg,   0);   /* CPU starts at index 0 */
}

/* -------------------------------------------------------------------------
 * Mt7922DmaInit
 *
 * Creates the WDFDMAENABLER (64-bit, packet-based) then allocates all four
 * descriptor rings and their associated data payload pools.
 * Finally programs the HIF ring-base registers so the hardware knows where
 * to DMA.
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922DmaInit(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS                 status;
    WDF_DMA_ENABLER_CONFIG   dmaConfig;
    SIZE_T                   txDescSize, rxDescSize;
    SIZE_T                   mcuTxDescSize, mcuRxDescSize;
    SIZE_T                   txDataSize, rxDataSize;

    /* Create DMA enabler — 64-bit addressing, max single transfer = 64 KB */
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig,
                                 WdfDmaProfileScatterGather64,
                                 MT7922_MAX_FRAME_SIZE * MT7922_TX_RING_SIZE);
    dmaConfig.WdmDmaVersionOverride = 0;   /* Use system default (avoids STATUS_INVALID_PARAMETER) */

    WinDropLog("[WIN-DROP] Calling WdfDmaEnablerCreate\n");
    status = WdfDmaEnablerCreate(Ctx->WdfDevice,
                                  &dmaConfig,
                                  WDF_NO_OBJECT_ATTRIBUTES,
                                  &Ctx->DmaEnabler);
    WinDropLog("[WIN-DROP] WdfDmaEnablerCreate status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_dma: WdfDmaEnablerCreate failed 0x%08X\n", status));
        return status;
    }

    /* ---- TX data descriptor ring ---- */
    txDescSize = sizeof(MT76_TX_DESC) * MT7922_TX_RING_SIZE;
    status = AllocCommonBuffer(Ctx, txDescSize,
                               &Ctx->TxRingBuffer,
                               (PVOID*)&Ctx->TxDescBase,
                               &Ctx->TxDescPa);
    if (!NT_SUCCESS(status)) return status;

    /* ---- TX payload staging pool (Data TX ring + MCU TX ring) ---- */
    txDataSize = (SIZE_T)MT7922_MAX_FRAME_SIZE * (MT7922_TX_RING_SIZE + MT7922_MCU_TX_RING_SIZE);
    status = AllocCommonBuffer(Ctx, txDataSize,
                               &Ctx->TxDataBuffer,
                               (PVOID*)&Ctx->TxDataVa,
                               &Ctx->TxDataPa);
    if (!NT_SUCCESS(status)) return status;

    /* ---- RX data descriptor ring ---- */
    rxDescSize = sizeof(MT76_RX_DESC) * MT7922_RX_RING_SIZE;
    status = AllocCommonBuffer(Ctx, rxDescSize,
                               &Ctx->RxRingBuffer,
                               (PVOID*)&Ctx->RxDescBase,
                               &Ctx->RxDescPa);
    if (!NT_SUCCESS(status)) return status;

    /* ---- RX payload pool (Data RX ring + MCU RX ring) ---- */
    rxDataSize = (SIZE_T)MT7922_MAX_FRAME_SIZE * (MT7922_RX_RING_SIZE + MT7922_MCU_RX_RING_SIZE);
    status = AllocCommonBuffer(Ctx, rxDataSize,
                               &Ctx->RxDataBuffer,
                               (PVOID*)&Ctx->RxDataVa,
                               &Ctx->RxDataPa);
    if (!NT_SUCCESS(status)) return status;

    /* ---- MCU TX ring ---- */
    mcuTxDescSize = sizeof(MT76_TX_DESC) * MT7922_MCU_TX_RING_SIZE;
    status = AllocCommonBuffer(Ctx, mcuTxDescSize,
                               &Ctx->McuTxRingBuffer,
                               (PVOID*)&Ctx->McuTxDescBase,
                               &Ctx->McuTxDescPa);
    if (!NT_SUCCESS(status)) return status;

    /* ---- MCU RX ring ---- */
    mcuRxDescSize = sizeof(MT76_RX_DESC) * MT7922_MCU_RX_RING_SIZE;
    status = AllocCommonBuffer(Ctx, mcuRxDescSize,
                               &Ctx->McuRxRingBuffer,
                               (PVOID*)&Ctx->McuRxDescBase,
                               &Ctx->McuRxDescPa);
    if (!NT_SUCCESS(status)) return status;

    /* Initialize ring head/tail indices */
    Ctx->TxHead = 0;
    Ctx->TxTail = 0;
    Ctx->RxHead = 0;
    Ctx->McuSeq = 0;

    /* Initialize the RX FIFO spin-lock */
    KeInitializeSpinLock(&Ctx->RxFifo.Lock);
    Ctx->RxFifo.WriteOffset = 0;
    Ctx->RxFifo.ReadOffset  = 0;

    /* -----------------------------------------------------------------------
     * Program HIF ring-base registers
     * Data TX ring -> HIF_TX_RING0
     * Data RX ring -> HIF_RX_RING0
     * MCU rings are accessed via WFDMA offsets (see mt7922_fw.c)
     * ----------------------------------------------------------------------- */
    ProgramRingRegs(Ctx,
                    MT_HIF_TX_RING0_CTRL0,
                    MT_HIF_TX_RING0_CTRL1,
                    MT_HIF_TX_RING0_CTRL2,
                    MT_HIF_TX_RING0_CIDX,
                    Ctx->TxDescPa,
                    MT7922_TX_RING_SIZE);

    ProgramRingRegs(Ctx,
                    MT_HIF_RX_RING0_CTRL0,
                    MT_HIF_RX_RING0_CTRL1,
                    MT_HIF_RX_RING0_CTRL2,
                    MT_HIF_RX_RING0_CIDX,
                    Ctx->RxDescPa,
                    MT7922_RX_RING_SIZE);

    ProgramRingRegs(Ctx,
                    MT_HIF_MCU_TX_RING_CTRL0,
                    MT_HIF_MCU_TX_RING_CTRL1,
                    MT_HIF_MCU_TX_RING_CTRL2,
                    MT_HIF_MCU_TX_RING_CIDX,
                    Ctx->McuTxDescPa,
                    MT7922_MCU_TX_RING_SIZE);

    ProgramRingRegs(Ctx,
                    MT_HIF_MCU_RX_RING_CTRL0,
                    MT_HIF_MCU_RX_RING_CTRL1,
                    MT_HIF_MCU_RX_RING_CTRL2,
                    MT_HIF_MCU_RX_RING_CIDX,
                    Ctx->McuRxDescPa,
                    MT7922_MCU_RX_RING_SIZE);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_dma: Rings initialised. "
               "TX_DESC_PA=0x%llX RX_DESC_PA=0x%llX\n",
               Ctx->TxDescPa.QuadPart, Ctx->RxDescPa.QuadPart));

    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Mt7922DmaFree
 * Returns all WdfCommonBuffers. KMDF automatically unmaps them.
 * Call BEFORE unmapping BAR0.
 * ------------------------------------------------------------------------- */
VOID
Mt7922DmaFree(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->TxRingBuffer != NULL) {
        WdfObjectDelete(Ctx->TxRingBuffer);
        Ctx->TxRingBuffer = NULL;
        Ctx->TxDescBase   = NULL;
    }
    if (Ctx->TxDataBuffer != NULL) {
        WdfObjectDelete(Ctx->TxDataBuffer);
        Ctx->TxDataBuffer = NULL;
        Ctx->TxDataVa     = NULL;
    }
    if (Ctx->RxRingBuffer != NULL) {
        WdfObjectDelete(Ctx->RxRingBuffer);
        Ctx->RxRingBuffer = NULL;
        Ctx->RxDescBase   = NULL;
    }
    if (Ctx->RxDataBuffer != NULL) {
        WdfObjectDelete(Ctx->RxDataBuffer);
        Ctx->RxDataBuffer = NULL;
        Ctx->RxDataVa     = NULL;
    }
    if (Ctx->McuTxRingBuffer != NULL) {
        WdfObjectDelete(Ctx->McuTxRingBuffer);
        Ctx->McuTxRingBuffer = NULL;
        Ctx->McuTxDescBase   = NULL;
    }
    if (Ctx->McuRxRingBuffer != NULL) {
        WdfObjectDelete(Ctx->McuRxRingBuffer);
        Ctx->McuRxRingBuffer = NULL;
        Ctx->McuRxDescBase   = NULL;
    }
    if (Ctx->DmaEnabler != NULL) {
        WdfObjectDelete(Ctx->DmaEnabler);
        Ctx->DmaEnabler = NULL;
    }
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_dma: DMA resources freed\n"));
}

/* -------------------------------------------------------------------------
 * Mt7922DmaRxReplenish
 *
 * Initialises every RX descriptor to point into the pre-allocated RX data
 * pool and writes the CPU index doorbell so the hardware can start writing
 * incoming frames.
 *
 * Called once from D0Entry; can be called again to recover from RX stall.
 * Must run at IRQL <= DISPATCH_LEVEL.
 * ------------------------------------------------------------------------- */
VOID
Mt7922DmaRxReplenish(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    for (i = 0; i < MT7922_RX_RING_SIZE; i++) {
        PMT76_RX_DESC desc = &Ctx->RxDescBase[i];
        PHYSICAL_ADDRESS bufPa;

        /* Each slot is MAX_FRAME_SIZE bytes into the RX data pool */
        bufPa.QuadPart = Ctx->RxDataPa.QuadPart +
                         (UINT64)i * MT7922_MAX_FRAME_SIZE;

        desc->BufLo  = (UINT32)(bufPa.QuadPart & 0xFFFFFFFFULL);
        desc->BufHi  = (UINT32)(bufPa.QuadPart >> 32);
        /* Ctrl0[15:0] = buffer size; Ctrl0[31] = DONE cleared by hardware */
        desc->Ctrl0  = MT7922_MAX_FRAME_SIZE;
        desc->Ctrl1  = 0;
    }

    /* Notify hardware: all RX_RING_SIZE slots ready */
    MtWriteReg32(Ctx, MT_HIF_RX_RING0_CIDX, MT7922_RX_RING_SIZE - 1);
    Ctx->RxHead = 0;

    for (i = 0; i < MT7922_MCU_RX_RING_SIZE; i++) {
        PMT76_RX_DESC mcuDesc = &Ctx->McuRxDescBase[i];
        PHYSICAL_ADDRESS bufPa;

        bufPa.QuadPart = Ctx->RxDataPa.QuadPart +
                         (UINT64)(MT7922_RX_RING_SIZE + i) * MT7922_MAX_FRAME_SIZE;

        mcuDesc->BufLo = (UINT32)(bufPa.QuadPart & 0xFFFFFFFFULL);
        mcuDesc->BufHi = (UINT32)(bufPa.QuadPart >> 32);
        mcuDesc->Ctrl0 = MT7922_MAX_FRAME_SIZE;
        mcuDesc->Ctrl1 = 0;
    }
    MtWriteReg32(Ctx, MT_HIF_MCU_RX_RING_CIDX, MT7922_MCU_RX_RING_SIZE - 1);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_dma: RX ring replenished (%u slots)\n",
               MT7922_RX_RING_SIZE));
}
