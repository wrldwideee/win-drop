/*
 * mt7922_ioctl.c - IOCTL dispatch for \\.\AwdlRadioDevice
 *
 * Implements:
 *   IOCTL_AWDL_SET_CHANNEL   -> MCU_UNI_CMD_CH_SWITCH
 *   IOCTL_AWDL_INJECT_FRAME  -> TX ring submit with NO_ACK + FT=0
 *   IOCTL_AWDL_READ_FRAMES   -> park IRP in read queue; DPC completes it
 *   IOCTL_AWDL_GET_STATUS    -> fill AWDL_DRIVER_STATUS from context
 *
 * Security: every userland buffer boundary is validated before any DMA map
 * or ring descriptor write to prevent BSOD / DMA privilege escalation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mt7922_awdl.h"

/* -------------------------------------------------------------------------
 * Internal: submit one 802.11 frame to the TX data ring
 *
 * Copies FrameData into the pre-allocated TX payload pool (one slot per
 * descriptor index) and arms the descriptor, then rings the doorbell.
 *
 * Called at IRQL <= DISPATCH_LEVEL (inside EvtIoDeviceControl which
 * runs at PASSIVE_LEVEL for sequential queue dispatch).
 * ------------------------------------------------------------------------- */
static NTSTATUS
TxSubmitFrame(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_(FrameLen) PUCHAR FrameData,
    _In_ UINT32  FrameLen,
    _In_ BOOLEAN NoAck
)
{
    ULONG          slot;
    PMT76_TX_DESC  desc;
    PUCHAR         stagingVa;
    PHYSICAL_ADDRESS stagingPa;
    UINT32         ctrl1;

    /* Check ring is not full (head catches up to tail means full) */
    ULONG nextHead = (Ctx->TxHead + 1) % MT7922_TX_RING_SIZE;
    if (nextHead == Ctx->TxTail) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "mt7922_ioctl: TX ring full\n"));
        return STATUS_DEVICE_BUSY;
    }

    slot     = Ctx->TxHead;
    desc     = &Ctx->TxDescBase[slot];
    stagingVa = Ctx->TxDataVa + (SIZE_T)slot * MT7922_MAX_FRAME_SIZE;
    stagingPa.QuadPart = Ctx->TxDataPa.QuadPart +
                         (UINT64)slot * MT7922_MAX_FRAME_SIZE;

    /* Copy frame into DMA-coherent staging area */
    RtlCopyMemory(stagingVa, FrameData, FrameLen);

    /* Build TX descriptor
     *   Ctrl0[15:0]  = buf0 length
     *   Ctrl1[16]    = LAST_SEG (single-buffer frame)
     *   Ctrl1[17]    = NO_ACK  (AWDL Action frames are broadcast)
     *   Ctrl1[20]    = FT=0    (802.11 native frame, not 802.3)
     */
    ctrl1 = MT_TXD_CTRL1_LAST_SEG;
    if (NoAck) {
        ctrl1 |= MT_TXD_CTRL1_NO_ACK;
    }
    /* Clear FT bit (0 = 802.11): MT_TXD_CTRL1_FT_MASK stays 0 */

    RtlZeroMemory(desc, sizeof(MT76_TX_DESC));
    desc->Buf0Lo = (UINT32)(stagingPa.QuadPart & 0xFFFFFFFFULL);
    desc->Buf0Hi = (UINT32)(stagingPa.QuadPart >> 32);
    desc->Ctrl0  = (UINT32)(FrameLen & 0xFFFFU);
    desc->Ctrl1  = ctrl1;

    /* Ensure descriptor is written before doorbell */
    KeMemoryBarrier();

    /* Advance head; write new CPU index to ring doorbell */
    Ctx->TxHead = nextHead;
    MtWriteReg32(Ctx, MT_HIF_TX_RING0_CIDX, (UINT32)Ctx->TxHead);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL,
               "mt7922_ioctl: TX submitted slot=%lu len=%u noack=%d\n",
               slot, FrameLen, (int)NoAck));
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * EvtIoDeviceControl — main IOCTL dispatcher
 * ------------------------------------------------------------------------- */
VOID
EvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    NTSTATUS         status   = STATUS_INVALID_DEVICE_REQUEST;
    WDFDEVICE        device   = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT  ctx      = GetDeviceContext(device);
    ULONG_PTR        info     = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    /* ------------------------------------------------------------------
     * IOCTL_AWDL_SET_CHANNEL
     * Input:  AWDL_SET_CHANNEL_IN
     * Issues MCU_UNI_CMD_CH_SWITCH with the requested 5 GHz social channel.
     * ------------------------------------------------------------------ */
    case IOCTL_AWDL_SET_CHANNEL: {
        PAWDL_SET_CHANNEL_IN in;
        size_t               inLen;

        status = WdfRequestRetrieveInputBuffer(Request,
                                               sizeof(AWDL_SET_CHANNEL_IN),
                                               (PVOID*)&in, &inLen);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "mt7922_ioctl: SET_CHANNEL bad input 0x%08X\n", status));
            break;
        }

        /* Validate channel: AWDL uses 5 GHz social channels 44 and 149 */
        if (in->Band != 1 ||
            (in->Channel != 44 && in->Channel != 149)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "mt7922_ioctl: SET_CHANNEL rejected ch=%u band=%u\n",
                       in->Channel, in->Band));
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        /*
         * MCU_UNI_CMD_CH_SWITCH payload layout (simplified connac2):
         *  [0]  = channel number
         *  [1]  = band (1 = 5 GHz)
         *  [2]  = bandwidth (0=20 MHz)
         *  [3]  = switch reason (0 = AWDL/normal)
         */
        {
            UINT8 chPayload[4];
            chPayload[0] = in->Channel;
            chPayload[1] = in->Band;
            chPayload[2] = in->Width;
            chPayload[3] = 0;   /* reason: AWDL */

            status = Mt7922McuSendCmd(ctx,
                                      MCU_UNI_CMD_CH_SWITCH,
                                      chPayload, sizeof(chPayload),
                                      TRUE);
        }

        if (NT_SUCCESS(status)) {
            ctx->CurrentChannel = in->Channel;
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "mt7922_ioctl: Channel switched to %u\n", in->Channel));
        }
        break;
    }

    /* ------------------------------------------------------------------
     * IOCTL_AWDL_INJECT_FRAME
     * Input:  AWDL_INJECT_FRAME_IN + variable frame bytes
     * Validates buffer bounds, copies frame into TX ring, rings doorbell.
     * ------------------------------------------------------------------ */
    case IOCTL_AWDL_INJECT_FRAME: {
        PAWDL_INJECT_FRAME_IN in;
        size_t                inLen;
        UINT32                frameLen;
        size_t                minRequired;

        status = WdfRequestRetrieveInputBuffer(Request,
                                               sizeof(AWDL_INJECT_FRAME_IN),
                                               (PVOID*)&in, &inLen);
        if (!NT_SUCCESS(status)) break;

        frameLen    = in->FrameLength;
        minRequired = FIELD_OFFSET(AWDL_INJECT_FRAME_IN, FrameData) + frameLen;

        /* ---- SECURITY: validate all userland-controlled lengths ---- */
        if (frameLen == 0 || frameLen > MT7922_MAX_FRAME_SIZE) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "mt7922_ioctl: INJECT_FRAME out-of-range len=%u\n",
                       frameLen));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (inLen < minRequired) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "mt7922_ioctl: INJECT_FRAME buffer underrun inLen=%zu need=%zu\n",
                       inLen, minRequired));
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = TxSubmitFrame(ctx,
                               in->FrameData, frameLen,
                               (BOOLEAN)(in->NoAck != 0));
        break;
    }

    /* ------------------------------------------------------------------
     * IOCTL_AWDL_READ_FRAMES
     * Output: one or more AWDL_RX_FRAME_HDR packets packed into buffer.
     * If the RX FIFO has data, drain it immediately.
     * If not, forward the IRP to the manual read queue for deferred
     * completion by the RX DPC.
     * ------------------------------------------------------------------ */
    case IOCTL_AWDL_READ_FRAMES: {
        PVOID   outBuf;
        size_t  outLen;
        LONG64  avail;

        status = WdfRequestRetrieveOutputBuffer(Request,
                                                sizeof(AWDL_RX_FRAME_HDR),
                                                &outBuf, &outLen);
        if (!NT_SUCCESS(status)) break;

        /* Atomically read the producer write offset */
        avail = InterlockedAdd64(
                    &ctx->RxFifo.WriteOffset,
                    0)
                - ctx->RxFifo.ReadOffset;

        if (avail > 0) {
            /* Data available — drain up to outLen bytes right now */
            SIZE_T toCopy = (SIZE_T)min((LONG64)outLen, avail);
            LONG64 readOff = ctx->RxFifo.ReadOffset;
            PUCHAR dst     = (PUCHAR)outBuf;
            SIZE_T i;

            for (i = 0; i < toCopy; i++) {
                dst[i] = ctx->RxFifo.Buffer[readOff % RX_FIFO_SIZE];
                readOff++;
            }
            /* Publish updated read offset (consumer side — no lock needed) */
            InterlockedExchange64(&ctx->RxFifo.ReadOffset, readOff);

            info   = toCopy;
            status = STATUS_SUCCESS;
        } else {
            /*
             * No data yet — forward IRP to manual queue.
             * The DPC will call WdfRequestCompleteWithInformation
             * once RX data arrives.
             */
            status = WdfRequestForwardToIoQueue(Request, ctx->ReadQueue);
            if (NT_SUCCESS(status)) {
                /* IRP is now owned by the queue; return without completing */
                return;
            }
            /* Forward failed — complete with error */
        }
        break;
    }

    /* ------------------------------------------------------------------
     * IOCTL_AWDL_GET_STATUS
     * Output: AWDL_DRIVER_STATUS
     * ------------------------------------------------------------------ */
    case IOCTL_AWDL_GET_STATUS: {
        PAWDL_DRIVER_STATUS out;
        size_t              outLen;

        status = WdfRequestRetrieveOutputBuffer(Request,
                                                sizeof(AWDL_DRIVER_STATUS),
                                                (PVOID*)&out, &outLen);
        if (!NT_SUCCESS(status)) break;

        out->ChipId          = ctx->ChipId;
        out->FirmwareVersion = ctx->FirmwareVersion;
        out->FirmwareLoaded  = ctx->FirmwareLoaded ? 1 : 0;
        out->RadioEnabled    = ctx->RadioEnabled    ? 1 : 0;
        out->CurrentChannel  = ctx->CurrentChannel;
        out->Reserved        = 0;

        info   = sizeof(AWDL_DRIVER_STATUS);
        status = STATUS_SUCCESS;
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}
