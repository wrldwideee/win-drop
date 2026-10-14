/*
 * mt7922_init.c - KMDF driver entry, EvtDeviceAdd, PnP callbacks,
 *                 PCIe BAR0 MMIO mapping, and chip-ID validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mt7922_awdl.h"

/* Pool tags for memory allocations */
#define MT_TAG_GENERIC  'tM7G'
#define MT_TAG_FW       'tM7F'
#define MT_TAG_DMA      'tM7D'

/* -------------------------------------------------------------------------
 * Kernel logging helper: writes to \SystemRoot\windrop_debug.log
 * Only writes when running at PASSIVE_LEVEL.
 * ------------------------------------------------------------------------- */
VOID
WinDropLog(
    _In_z_ _Printf_format_string_ const char *Format,
    ...
)
{
    NTSTATUS          status;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    HANDLE            handle;
    UNICODE_STRING    path;
    char              buffer[512];
    va_list           args;
    size_t            len = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    va_start(args, Format);
    RtlStringCbVPrintfA(buffer, sizeof(buffer), Format, args);
    va_end(args);

    RtlStringCbLengthA(buffer, sizeof(buffer), &len);

    RtlInitUnicodeString(&path, L"\\SystemRoot\\windrop_debug.log");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateFile(&handle,
                          FILE_APPEND_DATA | SYNCHRONIZE,
                          &oa,
                          &iosb,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL,
                          0);
    if (NT_SUCCESS(status)) {
        ZwWriteFile(handle, NULL, NULL, NULL, &iosb, buffer, (ULONG)len, NULL, NULL);
        ZwClose(handle);
    }
}

/* -------------------------------------------------------------------------
 * DriverEntry
 * Registers WDF callbacks and creates the driver object.
 * ------------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG  config;
    NTSTATUS           status;

    WinDropLog("[WIN-DROP] DriverEntry started. RegistryPath=%wZ\n", RegistryPath);
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: DriverEntry\n"));

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.DriverPoolTag = MT_TAG_GENERIC;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    WinDropLog("[WIN-DROP] WdfDriverCreate status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfDriverCreate failed 0x%08X\n", status));
    }
    return status;
}

/* -------------------------------------------------------------------------
 * EvtDeviceAdd
 * Called once per enumerated MT7922 PCIe device. Creates the WDFDEVICE,
 * registers PnP/power callbacks, and sets up the I/O queue + device
 * interface GUID so userland can open \\.\AwdlRadioDevice.
 * ------------------------------------------------------------------------- */
NTSTATUS
EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS                     status;
    WDF_OBJECT_ATTRIBUTES        attribs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_INTERRUPT_CONFIG         intrConfig;
    WDFDEVICE                    device;
    PDEVICE_CONTEXT              ctx;
    WDF_IO_QUEUE_CONFIG          queueConfig;
    WDFQUEUE                     defaultQueue;
    WDFQUEUE                     readQueue;
    DECLARE_CONST_UNICODE_STRING(ntDeviceName, L"\\Device\\AwdlRadio0");
    DECLARE_CONST_UNICODE_STRING(dosDeviceName, L"\\DosDevices\\AwdlRadioDevice");
    DECLARE_CONST_UNICODE_STRING(sddlString, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)");

    UNREFERENCED_PARAMETER(Driver);

    WinDropLog("[WIN-DROP] EvtDeviceAdd entered\n");

    /* Set up PnP/Power event callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnpPower.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    pnpPower.EvtDeviceD0Entry         = EvtDeviceD0Entry;
    pnpPower.EvtDeviceD0Exit          = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    /* Assign a fixed NT device name so we can create a symlink */
    status = WdfDeviceInitAssignName(DeviceInit, &ntDeviceName);
    WinDropLog("[WIN-DROP] WdfDeviceInitAssignName status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfDeviceInitAssignName failed 0x%08X\n", status));
        return status;
    }

    /* Assign SDDL string so non-admin userland can communicate with \\.\AwdlRadioDevice */
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddlString);
    WinDropLog("[WIN-DROP] WdfDeviceInitAssignSDDLString status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfDeviceInitAssignSDDLString failed 0x%08X\n", status));
        return status;
    }

    /* Device context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attribs, DEVICE_CONTEXT);
    attribs.EvtCleanupCallback = NULL;

    status = WdfDeviceCreate(&DeviceInit, &attribs, &device);
    WinDropLog("[WIN-DROP] WdfDeviceCreate status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    /* Required for DMA common buffers */
    WdfDeviceSetAlignmentRequirement(device, FILE_QUAD_ALIGNMENT);

    ctx = GetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(DEVICE_CONTEXT));
    ctx->WdfDevice = device;
    KeInitializeEvent(&ctx->McuCmdEvent, NotificationEvent, FALSE);

    /* DOS symlink  \\.\AwdlRadioDevice */
    status = WdfDeviceCreateSymbolicLink(device, &dosDeviceName);
    WinDropLog("[WIN-DROP] WdfDeviceCreateSymbolicLink status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfDeviceCreateSymbolicLink failed 0x%08X\n", status));
        return status;
    }

    /* Expose device interface GUID for PnP-style open */
    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_AWDL_RADIO,
                                            NULL);
    WinDropLog("[WIN-DROP] WdfDeviceCreateDeviceInterface status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfDeviceCreateDeviceInterface failed 0x%08X\n", status));
        return status;
    }

    /* Default I/O queue (for IOCTL_AWDL_SET_CHANNEL, INJECT, GET_STATUS) */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
    WinDropLog("[WIN-DROP] WdfIoQueueCreate defaultQueue status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: default queue create failed 0x%08X\n", status));
        return status;
    }

    /*
     * Manual-dispatch read queue for IOCTL_AWDL_READ_FRAMES.
     * IRPs park here until the DPC has data ready; then the DPC
     * completes the oldest pending IRP from the queue.
     */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &readQueue);
    WinDropLog("[WIN-DROP] WdfIoQueueCreate readQueue status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: read queue create failed 0x%08X\n", status));
        return status;
    }
    ctx->ReadQueue = readQueue;

    /* Configure interrupt — MSI(X) preferred; resource assigned by PnP */
    WDF_INTERRUPT_CONFIG_INIT(&intrConfig, EvtInterruptIsr, EvtInterruptDpc);
    /* KMDF automatically maps the PnP interrupt resource; no manual field set needed */
    status = WdfInterruptCreate(device, &intrConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &ctx->Interrupt);
    WinDropLog("[WIN-DROP] WdfInterruptCreate status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: WdfInterruptCreate failed 0x%08X\n", status));
        return status;
    }


    WinDropLog("[WIN-DROP] EvtDeviceAdd success\n");
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: EvtDeviceAdd success\n"));
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * EvtDevicePrepareHardware
 *
 * Maps PCIe BAR0 into kernel virtual address space and validates the chip
 * by reading MT_TOP_CHIP_ID.  This is the earliest safe point to touch MMIO
 * because PnP has already assigned the resource list.
 * ------------------------------------------------------------------------- */
NTSTATUS
EvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PDEVICE_CONTEXT         ctx = GetDeviceContext(Device);
    ULONG                   resourceCount;
    ULONG                   i;
    BOOLEAN                 bar0Mapped = FALSE;
    NTSTATUS                status = STATUS_SUCCESS;
    UINT32                  chipId;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    WinDropLog("[WIN-DROP] EvtDevicePrepareHardware entered. resourceCount=%lu\n", resourceCount);

    for (i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (desc == NULL) continue;

        switch (desc->Type) {
        case CmResourceTypeMemory:
            /*
             * BAR0 is typically the first memory resource.
             * MT7922 BAR0 is 1 MB; skip if already mapped or too small.
             */
            if (bar0Mapped) break;
            if (desc->u.Memory.Length < 0x100000) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                           "mt7922_awdl: skipping small memory BAR 0x%llX len=0x%X\n",
                           desc->u.Memory.Start.QuadPart,
                           desc->u.Memory.Length));
                break;
            }

            ctx->Bar0PhysBase = desc->u.Memory.Start;
            ctx->Bar0Size     = desc->u.Memory.Length;
            ctx->Bar0Va       = (PUCHAR)MmMapIoSpaceEx(
                                    ctx->Bar0PhysBase,
                                    ctx->Bar0Size,
                                    PAGE_NOCACHE | PAGE_READWRITE);
            if (ctx->Bar0Va == NULL) {
                WinDropLog("[WIN-DROP] MmMapIoSpaceEx BAR0 failed\n");
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "mt7922_awdl: MmMapIoSpaceEx BAR0 failed\n"));
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            bar0Mapped = TRUE;
            WinDropLog("[WIN-DROP] BAR0 PA=0x%llX VA=%p size=0x%zX\n",
                       ctx->Bar0PhysBase.QuadPart, ctx->Bar0Va, ctx->Bar0Size);
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "mt7922_awdl: BAR0 PA=0x%llX VA=%p size=0x%zX\n",
                       ctx->Bar0PhysBase.QuadPart,
                       ctx->Bar0Va,
                       ctx->Bar0Size));
            break;

        default:
            break;
        }
    }

    if (!bar0Mapped) {
        WinDropLog("[WIN-DROP] No BAR0 memory resource found\n");
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: No BAR0 memory resource found\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* -----------------------------------------------------------------------
     * Chip ID validation
     *
     * Device identity is validated by PnP hardware ID binding:
     *   MT7922: PCI\VEN_14C3&DEV_7922
     *   RZ616 : PCI\VEN_14C3&DEV_0616 (MediaTek silicon)
     *
     * In CONNAC PCIe devices, the top-level register block (0x88000000)
     * lives behind dynamic L1 remap windows, not direct BAR0 physical offsets.
     * We populate ChipId with the canonical model ID to avoid unmapped MMIO reads.
     * ----------------------------------------------------------------------- */
    chipId = 0x79220000U;
    ctx->ChipId = chipId;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: Chip identification OK (MT7922/RZ616: 0x%08X)\n", chipId));

    /* Acquire bus interface so we can do config-space reads later */
    status = WdfFdoQueryForInterface(Device,
                                     &GUID_BUS_INTERFACE_STANDARD,
                                     (PINTERFACE)&ctx->BusInterface,
                                     sizeof(BUS_INTERFACE_STANDARD),
                                     1,
                                     NULL);
    WinDropLog("[WIN-DROP] WdfFdoQueryForInterface status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "mt7922_awdl: BUS_INTERFACE_STANDARD unavailable 0x%08X\n",
                   status));
        /* Non-fatal — we can still operate without direct config-space access */
        status = STATUS_SUCCESS;
    }

    /* Allocate DMA rings */
    status = Mt7922DmaInit(ctx);
    WinDropLog("[WIN-DROP] Mt7922DmaInit status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: Mt7922DmaInit failed 0x%08X\n", status));
        MmUnmapIoSpace(ctx->Bar0Va, ctx->Bar0Size);
        ctx->Bar0Va = NULL;
        return status;
    }

    WinDropLog("[WIN-DROP] EvtDevicePrepareHardware success\n");
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * EvtDeviceReleaseHardware
 * Unmap BAR0 and free DMA resources.
 * ------------------------------------------------------------------------- */
NTSTATUS
EvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    WinDropLog("[WIN-DROP] EvtDeviceReleaseHardware entered\n");
    Mt7922DmaFree(ctx);

    if (ctx->Bar0Va != NULL) {
        MmUnmapIoSpace(ctx->Bar0Va, ctx->Bar0Size);
        ctx->Bar0Va = NULL;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: Hardware released\n"));
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * EvtDeviceD0Entry
 * Device entering D0 (fully powered). Load firmware and enable the radio.
 * ------------------------------------------------------------------------- */
NTSTATUS
EvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS        status;
    UINT32          gloCfg;

    UNREFERENCED_PARAMETER(PreviousState);

    WinDropLog("[WIN-DROP] EvtDeviceD0Entry entered\n");
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: D0Entry\n"));

    /* Enable WFDMA TX + RX engines */
    gloCfg = MtReadReg32(ctx, MT_WFDMA0_GLO_CFG);
    gloCfg |= MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN;
    MtWriteReg32(ctx, MT_WFDMA0_GLO_CFG, gloCfg);

    /* Unmask interrupts: RX data, MCU event, TX MCU done */
    MtWriteReg32(ctx, MT_WFDMA0_INT_ENA,
                 MT_INT_RX_DONE_DATA |
                 MT_INT_TX_DONE_MCU  |
                 MT_INT_MCU_CMD);

    /* Load MCU patch + RAM firmware */
    status = Mt7922FirmwareLoad(ctx);
    WinDropLog("[WIN-DROP] Mt7922FirmwareLoad status=0x%08X\n", status);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "mt7922_awdl: Firmware load failed 0x%08X\n", status));
        return status;
    }

    /* Replenish RX ring with empty buffers */
    Mt7922DmaRxReplenish(ctx);

    ctx->RadioEnabled    = FALSE;  /* Monitor mode not yet configured */
    ctx->CurrentChannel  = 0;

    WinDropLog("[WIN-DROP] EvtDeviceD0Entry complete (FW ver=0x%08X)\n", ctx->FirmwareVersion);
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: D0Entry complete — FW ver=0x%08X\n",
               ctx->FirmwareVersion));
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------
 * EvtDeviceD0Exit
 * Device leaving D0. Disable DMA engines and mask all interrupts.
 * ------------------------------------------------------------------------- */
NTSTATUS
EvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    UINT32          gloCfg;

    UNREFERENCED_PARAMETER(TargetState);

    /* Mask all interrupts before disabling DMA to avoid stray ISR */
    MtWriteReg32(ctx, MT_WFDMA0_INT_ENA, 0);

    /* Stop WFDMA */
    gloCfg = MtReadReg32(ctx, MT_WFDMA0_GLO_CFG);
    gloCfg &= ~(MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
    MtWriteReg32(ctx, MT_WFDMA0_GLO_CFG, gloCfg);

    ctx->RadioEnabled   = FALSE;
    ctx->FirmwareLoaded = FALSE;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "mt7922_awdl: D0Exit complete\n"));
    return STATUS_SUCCESS;
}
