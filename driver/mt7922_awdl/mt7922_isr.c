/*
 * mt7922_isr.c - Interrupt Service Routine and DPC for MT7922
 *
 * ISR (EvtInterruptIsr):
 *   Runs at DIRQL. Reads and clears WFDMA interrupt status register.
 *   Returns TRUE if our device raised the interrupt; queues the DPC.
 *
 * DPC (EvtInterruptDpc):
 *   Runs at DISPATCH_LEVEL. Processes RX ring and MCU event ring.
 *   Forwards raw 802.11 frames to the RX FIFO.
 *   Signals McuCmdEvent for pending MCU command waiters.
 *   Completes any parked IOCTL_AWDL_READ_FRAMES IRPs from the read queue.
 *
 * Mt7922ProcessRxRing:
 *   Called from DPC. Drains all completed RX descriptors, packs frames
 *   into the RX FIFO with an AWDL_RX_FRAME_HDR prefix, and replenishes
 *   the slot so hardware can reuse the buffer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mt7922_awdl.h"

/* -------------------------------------------------------------------------
 * EvtInterruptIsr
 * IRQL == DIRQL.  Must be very fast — only read/clear interrupt status.
 * ------------------------------------------------------------------------- */
BOOLEAN
EvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG        MessageID
)
{
    WDFDEVICE       device = WdfInterruptGetDevice(Interrupt);
    PDEVICE_CONTEXT ctx    = GetDeviceContext(device);
    UINT32          intSta;

    UNREFERENCED_PARAMETER(MessageID);

    /* Read interrupt status */
    intSta = MtReadReg32(ctx, MT_WFDMA0_INT_STA);
    if (intSta == 0 || intSta == 0xFFFFFFFF) {
        /* Not our interrupt (bus error or no pending int) */
        return FALSE;
    }

    /* Acknowledge / clear the interrupt status bits we care about */
    MtWriteReg32(ctx, MT_WFDMA0_INT_STA,
                 intSta & (MT_INT_RX_DONE_DATA |
                           MT_INT_TX_DONE_MCU  |
                           MT_INT_MCU_CMD));

    /* Mask all interrupts until DPC re-enables them to avoid re-entrance */
    MtWriteReg32(ctx, MT_WFDMA0_INT_ENA, 0);

    /*
     * Stash interrupt status for the DPC.  We reuse the low 32 bits of
     * PendingIntSta (a UINT32 we add to the device context for this purpose)
     * rather than aliasing McuCmdStatus, which has its own contract.
     * NOTE: WdfInterruptSetPolicy must NOT be called here at DIRQL — it is
     * a PASSIVE_LEVEL-only API and belongs in EvtDeviceAdd.
     */
    ctx->PendingIntSta = intSta;

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

/* -------------------------------------------------------------------------
 * EvtInterruptDpc
 * IRQL == DISPATCH_LEVEL.
 * Processes all pending RX data, MCU events, and TX completions.
 * Re-enables interrupts when done.
 * ------------------------------------------------------------------------- */
VOID
EvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT    Device
)
{
    PDEVICE_CONTEXT ctx    = GetDeviceContext((WDFDEVICE)Device);
    UINT32          intSta = ctx->PendingIntSta;  /* set by ISR at DIRQL */

    UNREFERENCED_PARAMETER(Interrupt);

    /* ---- RX data frames ---- */
    if (intSta & MT_INT_RX_DONE_DATA) {
        Mt7922ProcessRxRing(ctx);
    }

    /* ---- MCU command/event ring ---- */
    if (intSta & (MT_INT_TX_DONE_MCU | MT_INT_MCU_CMD)) {
        /* Read the MCU RX ring to find the response event */
        ULONG           didx = MtReadReg32(ctx, MT_HIF_MCU_RX_RING_DIDX);
        ULONG           cidx = MtReadReg32(ctx, MT_HIF_MCU_RX_RING_CIDX);
        ULONG           slot;

        /* Walk all completed MCU RX descriptors */
        while (cidx != didx) {
            slot = cidx % MT7922_MCU_RX_RING_SIZE;
            PMT76_RX_DESC  rxd  = &ctx->McuRxDescBase[slot];
            UINT32         ctrl0 = rxd->Ctrl0;

            if (ctrl0 & MT_RXD_CTRL0_DONE) {
                ULONG pktLen = rxd->Ctrl1 & 0xFFFFU;

                if (pktLen >= sizeof(MT76_MCU_RXD)) {
                    /* VA of this MCU RX payload in the RX data pool */
                    PUCHAR       payloadVa = ctx->RxDataVa +
                                            (SIZE_T)(MT7922_RX_RING_SIZE * MT7922_MAX_FRAME_SIZE) +
                                            (SIZE_T)(slot * MT7922_MAX_FRAME_SIZE);
                    PMT76_MCU_RXD evt = (PMT76_MCU_RXD)payloadVa;

                    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL,
                               "mt7922_isr: MCU event eid=0x%02X seq=%u status=0x%02X\n",
                               evt->Eid, evt->Sequence, evt->Status));

                    if (evt->Eid == MCU_EVENT_FW_START ||
                        evt->Eid == MCU_EVENT_PATCH_SEM_DONE ||
                        evt->Sequence == ctx->McuLastSeq)
                    {
                        ctx->McuCmdStatus = (evt->Status == 0) ?
                                            STATUS_SUCCESS :
                                            STATUS_UNSUCCESSFUL;
                        KeSetEvent(&ctx->McuCmdEvent, IO_NETWORK_INCREMENT, FALSE);
                    }
                }

                /* Clear DONE bit and return descriptor to hardware */
                rxd->Ctrl0 = MT7922_MAX_FRAME_SIZE;  /* refill buf_size */
                rxd->Ctrl1 = 0;
            }

            cidx = (cidx + 1) % MT7922_MCU_RX_RING_SIZE;
        }

        /* Advance MCU RX CPU index */
        MtWriteReg32(ctx, MT_HIF_MCU_RX_RING_CIDX,
                     (UINT32)((cidx > 0 ? cidx : MT7922_MCU_RX_RING_SIZE) - 1));
    }

    /* Re-enable all interrupts now that DPC is done */
    MtWriteReg32(ctx, MT_WFDMA0_INT_ENA,
                 MT_INT_RX_DONE_DATA |
                 MT_INT_TX_DONE_MCU  |
                 MT_INT_MCU_CMD);
}

/* -------------------------------------------------------------------------
 * Mt7922ProcessRxRing
 *
 * Drains completed RX descriptors, writes each raw 802.11 frame prefixed
 * with an AWDL_RX_FRAME_HDR into the lock-free RX FIFO, then replenishes
 * the descriptor so hardware can reuse the buffer.
 *
 * Also completes any IRP parked in the read queue if FIFO has data.
 * Called from EvtInterruptDpc at DISPATCH_LEVEL.
 * ------------------------------------------------------------------------- */
VOID
Mt7922ProcessRxRing(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG didx = MtReadReg32(Ctx, MT_HIF_RX_RING0_DIDX);
    ULONG cidx = Ctx->RxHead;

    while (cidx != didx) {
        ULONG         slot = cidx % MT7922_RX_RING_SIZE;
        PMT76_RX_DESC rxd  = &Ctx->RxDescBase[slot];
        UINT32        ctrl0 = rxd->Ctrl0;

        if (!(ctrl0 & MT_RXD_CTRL0_DONE)) {
            /* Hardware hasn't written the done bit yet; stop */
            break;
        }

        {
            ULONG   pktLen  = rxd->Ctrl1 & 0xFFFFU;
            PUCHAR  frameSrc = Ctx->RxDataVa + (SIZE_T)slot * MT7922_MAX_FRAME_SIZE;
            KIRQL   oldIrql;

            if (pktLen > 0 && pktLen <= MT7922_MAX_FRAME_SIZE) {
                ULONG       hdrSize  = FIELD_OFFSET(AWDL_RX_FRAME_HDR, FrameData);
                ULONG       total    = hdrSize + pktLen;
                AWDL_RX_FRAME_HDR hdr;
                LONG64      writeOff;
                ULONG       i;
                LARGE_INTEGER qpc;

                KeQueryPerformanceCounter(&qpc);
                hdr.TotalLength = total;
                hdr.Timestamp   = (UINT64)qpc.QuadPart;
                hdr.Rssi        = -70;   /* TODO: parse from vendor RX status */
                hdr.Channel     = Ctx->CurrentChannel;
                hdr.Reserved[0] = 0;
                hdr.Reserved[1] = 0;

                /* Write into FIFO under spin-lock (producer side) */
                KeAcquireSpinLock(&Ctx->RxFifo.Lock, &oldIrql);

                writeOff = Ctx->RxFifo.WriteOffset;

                /* Overflow check: drop frame if FIFO is full */
                if ((writeOff - Ctx->RxFifo.ReadOffset) +
                    (LONG64)total <= (LONG64)RX_FIFO_SIZE)
                {
                    /* Write header bytes */
                    PUCHAR hdrBytes = (PUCHAR)&hdr;
                    for (i = 0; i < hdrSize; i++) {
                        Ctx->RxFifo.Buffer[writeOff % RX_FIFO_SIZE] = hdrBytes[i];
                        writeOff++;
                    }
                    /* Write frame bytes */
                    for (i = 0; i < pktLen; i++) {
                        Ctx->RxFifo.Buffer[writeOff % RX_FIFO_SIZE] = frameSrc[i];
                        writeOff++;
                    }
                    InterlockedExchange64(&Ctx->RxFifo.WriteOffset, writeOff);
                } else {
                    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                               "mt7922_isr: RX FIFO overflow, dropping %u byte frame\n",
                               pktLen));
                }

                KeReleaseSpinLock(&Ctx->RxFifo.Lock, oldIrql);
            }

            /* Replenish this descriptor slot */
            {
                PHYSICAL_ADDRESS slotPa;
                slotPa.QuadPart = Ctx->RxDataPa.QuadPart +
                                  (UINT64)slot * MT7922_MAX_FRAME_SIZE;
                rxd->BufLo  = (UINT32)(slotPa.QuadPart & 0xFFFFFFFFULL);
                rxd->BufHi  = (UINT32)(slotPa.QuadPart >> 32);
                rxd->Ctrl0  = MT7922_MAX_FRAME_SIZE;   /* clear DONE, reset size */
                rxd->Ctrl1  = 0;
            }
        }

        cidx = (cidx + 1) % MT7922_RX_RING_SIZE;
    }

    Ctx->RxHead = cidx;

    /* Notify hardware of replenished slots */
    if (cidx != (ULONG)MtReadReg32(Ctx, MT_HIF_RX_RING0_CIDX)) {
        MtWriteReg32(Ctx, MT_HIF_RX_RING0_CIDX,
                     (UINT32)((cidx > 0 ? cidx : MT7922_RX_RING_SIZE) - 1));
    }

    /* -----------------------------------------------------------------------
     * Complete any parked IOCTL_AWDL_READ_FRAMES IRP from the read queue
     * if the FIFO now has data.
     * ----------------------------------------------------------------------- */
    {
        LONG64  avail = Ctx->RxFifo.WriteOffset - Ctx->RxFifo.ReadOffset;

        if (avail > 0) {
            WDFREQUEST  pendingRequest;
            NTSTATUS    peekStatus;

            peekStatus = WdfIoQueueRetrieveNextRequest(Ctx->ReadQueue,
                                                       &pendingRequest);
            if (NT_SUCCESS(peekStatus)) {
                PVOID   outBuf;
                size_t  outLen;
                ULONG_PTR bytesCopied = 0;

                if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(pendingRequest,
                                                              1,
                                                              &outBuf,
                                                              &outLen)))
                {
                    SIZE_T  toCopy = (SIZE_T)min((LONG64)outLen, avail);
                    LONG64  readOff = Ctx->RxFifo.ReadOffset;
                    PUCHAR  dst     = (PUCHAR)outBuf;
                    SIZE_T  i;

                    for (i = 0; i < toCopy; i++) {
                        dst[i] = Ctx->RxFifo.Buffer[readOff % RX_FIFO_SIZE];
                        readOff++;
                    }
                    InterlockedExchange64(&Ctx->RxFifo.ReadOffset, readOff);
                    bytesCopied = toCopy;
                }

                WdfRequestCompleteWithInformation(pendingRequest,
                                                  STATUS_SUCCESS,
                                                  bytesCopied);
            }
        }
    }
}
