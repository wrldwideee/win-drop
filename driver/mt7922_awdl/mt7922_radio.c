/*
 * mt7922_radio.c - Monitor mode and channel filter control for MT7922
 *
 * Step 2 additions:
 *   - Mt7922SetMonitorMode: sends MCU_UNI_CMD_SET_FILTER to enable
 *     promiscuous reception of all 802.11 management and data frames
 *     without 802.3 encapsulation translation.
 *   - Mt7922SetMacAddress: programs the hardware MAC for the AWDL interface.
 *   - Mt7922EnableRadio / Mt7922DisableRadio: RF on/off helpers called from
 *     IOCTL_AWDL_SET_CHANNEL and D0Exit respectively.
 *
 * MT76 connac2 filter flags ported from:
 *   linux/mt76/mt7921/mcu.c  mt7921_mcu_set_rx_hdr_trans()
 *   linux/mt76/mt76_connac_mcu.c  mt76_connac_mcu_set_filter()
 *
 * SPDX-License-Identifier: MIT
 */

#include "mt7922_awdl.h"

/* -------------------------------------------------------------------------
 * MT76 RX filter flags (MCU_UNI_CMD_SET_FILTER payload)
 *
 * Ported from mt76_connac_mcu.h RX_FILTER_* defines.
 * -------------------------------------------------------------------------*/
#define RX_FILTER_BSSID_EN          (1U <<  0)  /* accept frames for our BSSID */
#define RX_FILTER_BCN_EN            (1U <<  1)  /* accept Beacon frames */
#define RX_FILTER_DROP_UNDER_BA     (1U <<  2)
#define RX_FILTER_CTRL_RSV          (1U <<  3)
#define RX_FILTER_MCAST_TO_UC       (1U <<  4)
#define RX_FILTER_MONITOR           (1U <<  5)  /* promiscuous / monitor mode */
#define RX_FILTER_PROBE_REQ         (1U <<  6)
#define RX_FILTER_MGMT_EN           (1U <<  7)
#define RX_FILTER_NO_80211_HDR      (1U <<  8)  /* 0 = keep 802.11 hdr (AWDL needs this) */
#define RX_FILTER_HW_80211_HDR_HDR  (1U <<  9)

/* For AWDL monitor: accept all frames, preserve 802.11 header, no BSSID filter */
#define AWDL_MONITOR_FILTER_FLAGS   (RX_FILTER_MONITOR  | \
                                     RX_FILTER_BCN_EN   | \
                                     RX_FILTER_MGMT_EN  | \
                                     RX_FILTER_PROBE_REQ)

/*
 * MCU SET_FILTER payload layout (connac2 unified command):
 *   [0..1]  = tag  (0x0000 for RX filter)
 *   [2..3]  = length of this TLV
 *   [4..7]  = filter flags (UINT32 LE)
 *   [8..11] = reserved
 */
#pragma pack(push, 1)
typedef struct _MCU_SET_FILTER_PAYLOAD {
    UINT16  Tag;
    UINT16  Len;
    UINT32  FilterFlags;
    UINT32  Reserved;
} MCU_SET_FILTER_PAYLOAD, *PMCU_SET_FILTER_PAYLOAD;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Mt7922SetMonitorMode
 *
 * Sends MCU_UNI_CMD_SET_FILTER with the AWDL monitor flag set.
 * After this call succeeds the RX ring will deliver raw 802.11 frames
 * (including the full MAC header) for all frames the radio hears on the
 * current channel.
 *
 * Must be called at PASSIVE_LEVEL after firmware has loaded.
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922SetMonitorMode(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ BOOLEAN         Enable
)
{
    MCU_SET_FILTER_PAYLOAD payload = {0};
    NTSTATUS               status;

    if (!Ctx->FirmwareLoaded) {
        return STATUS_DEVICE_NOT_READY;
    }

    payload.Tag         = 0x0000;
    payload.Len         = sizeof(MCU_SET_FILTER_PAYLOAD);
    payload.FilterFlags = Enable ? AWDL_MONITOR_FILTER_FLAGS : 0;
    payload.Reserved    = 0;

    status = Mt7922McuSendCmd(Ctx,
                              MCU_UNI_CMD_SET_FILTER,
                              &payload,
                              sizeof(payload),
                              TRUE);
    if (NT_SUCCESS(status)) {
        Ctx->RadioEnabled = Enable;
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "mt7922_radio: Monitor mode %s (flags=0x%08X)\n",
                   Enable ? "ENABLED" : "DISABLED",
                   payload.FilterFlags));
    } else {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_radio: SET_FILTER failed 0x%08X\n", status));
    }
    return status;
}

/* -------------------------------------------------------------------------
 * Mt7922SetMacAddress
 *
 * Programs the hardware MAC address register for the AWDL virtual interface.
 * AWDL uses an OUI-prefixed random MAC: Apple OUI 00:17:F2 with random
 * last 3 octets, local-admin bit set.
 *
 * Payload format (connac2 SET_MAC_ADDR):
 *   [0]    = band index (0 = 5 GHz social band)
 *   [1..6] = MAC address bytes
 *   [7]    = reserved
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922SetMacAddress(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_(6) PUCHAR MacAddr
)
{
#pragma pack(push, 1)
    struct {
        UINT8  Band;
        UINT8  Mac[6];
        UINT8  Reserved;
    } payload = {0};
#pragma pack(pop)

    payload.Band = 1;   /* 5 GHz */
    RtlCopyMemory(payload.Mac, MacAddr, 6);

    return Mt7922McuSendCmd(Ctx,
                            0x2A,   /* MCU_UNI_CMD_SET_MAC_ADDR (connac2) */
                            &payload,
                            sizeof(payload),
                            TRUE);
}

/* -------------------------------------------------------------------------
 * Mt7922EnableRadio
 *
 * Full activation sequence for the AWDL radio:
 *   1. Switch to the initial social channel (CH 6 in AWDL master election,
 *      or CH 44 as the default 5 GHz social channel).
 *   2. Enable monitor mode (promiscuous RX).
 *   3. Set the AWDL MAC address.
 *
 * Called from EvtDeviceD0Entry after firmware loads, or from the AWDL daemon
 * via IOCTL_AWDL_SET_CHANNEL (which calls Mt7922SetMonitorMode implicitly).
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922EnableRadio(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UINT8           InitialChannel,
    _In_reads_(6) PUCHAR AwdlMac
)
{
    NTSTATUS status;

    /* Step 1: switch to initial channel */
    {
        UINT8 chPayload[4];
        chPayload[0] = InitialChannel;
        chPayload[1] = 1;   /* 5 GHz */
        chPayload[2] = 0;   /* 20 MHz */
        chPayload[3] = 0;

        status = Mt7922McuSendCmd(Ctx,
                                  MCU_UNI_CMD_CH_SWITCH,
                                  chPayload, sizeof(chPayload),
                                  TRUE);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "mt7922_radio: Initial CH switch failed 0x%08X\n", status));
            return status;
        }
        Ctx->CurrentChannel = InitialChannel;
    }

    /* Step 2: program AWDL MAC */
    status = Mt7922SetMacAddress(Ctx, AwdlMac);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_radio: SetMacAddress failed 0x%08X\n", status));
        return status;
    }

    /* Step 3: enable promiscuous monitor mode */
    status = Mt7922SetMonitorMode(Ctx, TRUE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_radio: SetMonitorMode failed 0x%08X\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_radio: Radio enabled on CH%u, MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               InitialChannel,
               AwdlMac[0], AwdlMac[1], AwdlMac[2],
               AwdlMac[3], AwdlMac[4], AwdlMac[5]));
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Mt7922DisableRadio
 *
 * Disables promiscuous RX filter (reverts to normal infra STA filter).
 * Safe to call at PASSIVE_LEVEL before D0Exit.
 * ------------------------------------------------------------------------- */
NTSTATUS
Mt7922DisableRadio(_In_ PDEVICE_CONTEXT Ctx)
{
    return Mt7922SetMonitorMode(Ctx, FALSE);
}
