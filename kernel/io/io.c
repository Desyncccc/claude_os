/*
 * RadiaNT -- io/io.c
 * I/O manager: driver objects, device objects and IRP dispatch, plus the
 * simple boot-start drivers (Null, Serial, Video).
 */
#include "io.h"
#include "mm.h"
#include "hal.h"
#include "rtl.h"

POBJECT_TYPE IoDriverObjectType, IoDeviceObjectType, IoFileObjectType;

static NTSTATUS IopInvalidDeviceRequest(PDEVICE_OBJECT Device, PIRP Irp)
{
    (void)Device;
    return IoCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

NTSTATUS IoCreateDriver(PCSTR Name, PCSTR Description, PDRIVER_INITIALIZE Entry)
{
    char path[64];
    RtlStringPrintf(path, sizeof(path), "\\Driver\\%s", Name);
    PVOID body;
    NTSTATUS st = ObCreateObject(IoDriverObjectType, path, sizeof(DRIVER_OBJECT), &body);
    if (!NT_SUCCESS(st)) return st;
    PDRIVER_OBJECT drv = body;
    RtlStringCopy(drv->Name, sizeof(drv->Name), Name);
    RtlStringCopy(drv->Description, sizeof(drv->Description), Description);
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = IopInvalidDeviceRequest;
    st = Entry(drv);
    DbgPrint("IO: driver \\Driver\\%s loaded (%u device%s), status 0x%08X\n", Name,
             drv->DeviceCount, drv->DeviceCount == 1 ? "" : "s", (ULONG)st);
    return st;
}

NTSTATUS IoCreateDevice(PDRIVER_OBJECT Driver, PCSTR Name, ULONG Type, SIZE_T ExtensionSize,
                        PDEVICE_OBJECT *Device)
{
    char path[64];
    RtlStringPrintf(path, sizeof(path), "\\Device\\%s", Name);
    PVOID body;
    NTSTATUS st = ObCreateObject(IoDeviceObjectType, path, sizeof(DEVICE_OBJECT) + ExtensionSize, &body);
    if (!NT_SUCCESS(st)) return st;
    PDEVICE_OBJECT dev = body;
    dev->DriverObject = Driver;
    dev->DeviceType = Type;
    dev->DeviceExtension = ExtensionSize ? (PVOID)(dev + 1) : NULL;
    RtlStringCopy(dev->Name, sizeof(dev->Name), Name);
    dev->NextDevice = Driver->DeviceObject;
    Driver->DeviceObject = dev;
    Driver->DeviceCount++;
    if (Device) *Device = dev;
    return STATUS_SUCCESS;
}

NTSTATUS IoCallDriver(PDEVICE_OBJECT Device, PIRP Irp)
{
    Irp->DeviceObject = Device;
    Device->IrpCount++;
    if (Irp->MajorFunction > IRP_MJ_MAXIMUM_FUNCTION)
        return IoCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    return Device->DriverObject->MajorFunction[Irp->MajorFunction](Device, Irp);
}

NTSTATUS IoSynchronousRequest(PDEVICE_OBJECT Device, UCHAR Major, ULONG Ioctl,
                              PVOID Buffer, ULONG Length, ULONG_PTR *Information)
{
    IRP irp;
    memset(&irp, 0, sizeof(irp));
    irp.MajorFunction = Major;
    irp.IoControlCode = Ioctl;
    irp.SystemBuffer = Buffer;
    irp.Length = Length;
    NTSTATUS st = IoCallDriver(Device, &irp);
    if (Information) *Information = irp.IoStatus.Information;
    return st;
}

/* ---- \Driver\Null ------------------------------------------------------------------ */
static NTSTATUS NullDispatch(PDEVICE_OBJECT Device, PIRP Irp)
{
    (void)Device;
    return IoCompleteRequest(Irp, STATUS_SUCCESS, Irp->MajorFunction == IRP_MJ_WRITE ? Irp->Length : 0);
}

NTSTATUS NullDriverEntry(PDRIVER_OBJECT Driver)
{
    Driver->MajorFunction[IRP_MJ_CREATE] = Driver->MajorFunction[IRP_MJ_CLOSE] = NullDispatch;
    Driver->MajorFunction[IRP_MJ_READ] = Driver->MajorFunction[IRP_MJ_WRITE] = NullDispatch;
    NTSTATUS st = IoCreateDevice(Driver, "Null", FILE_DEVICE_NULL, 0, NULL);
    ObCreateSymbolicLink("\\GLOBAL??\\NUL", "\\Device\\Null");
    return st;
}

/* ---- \Driver\Serial ------------------------------------------------------------------ */
static NTSTATUS SerialWrite(PDEVICE_OBJECT Device, PIRP Irp)
{
    (void)Device;
    HalSerialWrite(Irp->SystemBuffer, Irp->Length);
    return IoCompleteRequest(Irp, STATUS_SUCCESS, Irp->Length);
}

NTSTATUS SerialDriverEntry(PDRIVER_OBJECT Driver)
{
    Driver->MajorFunction[IRP_MJ_WRITE] = SerialWrite;
    NTSTATUS st = IoCreateDevice(Driver, "Serial0", FILE_DEVICE_SERIAL_PORT, 0, NULL);
    ObCreateSymbolicLink("\\GLOBAL??\\COM1", "\\Device\\Serial0");
    return st;
}

/* ---- \Driver\Video (linear framebuffer miniport) ---------------------------------------- */
static NTSTATUS VideoDeviceControl(PDEVICE_OBJECT Device, PIRP Irp)
{
    (void)Device;
    if (Irp->IoControlCode != IOCTL_VIDEO_QUERY_CURRENT_MODE)
        return IoCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (Irp->Length < sizeof(VIDEO_MODE_INFORMATION))
        return IoCompleteRequest(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    VIDEO_MODE_INFORMATION *m = Irp->SystemBuffer;
    m->Width = HalFramebuffer.Width; m->Height = HalFramebuffer.Height;
    m->Pitch = HalFramebuffer.Pitch; m->BitsPerPixel = HalFramebuffer.Bpp;
    return IoCompleteRequest(Irp, STATUS_SUCCESS, sizeof(*m));
}

NTSTATUS VideoDriverEntry(PDRIVER_OBJECT Driver)
{
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VideoDeviceControl;
    NTSTATUS st = IoCreateDevice(Driver, "Video0", FILE_DEVICE_VIDEO, 0, NULL);
    ObCreateSymbolicLink("\\GLOBAL??\\DISPLAY1", "\\Device\\Video0");
    return st;
}

/* ---- Initialisation ---------------------------------------------------------------------- */
typedef struct _BOOT_FILE { ULONG64 PhysicalStart; ULONG64 Size; } BOOT_FILE;

void IoInitSystem(void)
{
    ObCreateObjectType("Driver", TAG('D', 'r', 'i', 'v'), NULL, &IoDriverObjectType);
    ObCreateObjectType("Device", TAG('D', 'e', 'v', 'i'), NULL, &IoDeviceObjectType);
    ObCreateObjectType("File", TAG('F', 'i', 'l', 'e'), NULL, &IoFileObjectType);

    IoCreateDriver("Null", "Null device (bit bucket)", NullDriverEntry);
    IoCreateDriver("Serial", "16550 UART serial port", SerialDriverEntry);
    IoCreateDriver("Video", "Linear framebuffer miniport", VideoDriverEntry);
    IoCreateDriver("i8042prt", "PS/2 keyboard and mouse port", I8042DriverEntry);

    /* Publish the boot images under \SystemRoot so they show up in the namespace. */
    for (ULONG i = 0; i < KeLoaderBlock.ModuleCount; i++) {
        char path[80];
        PVOID body;
        RtlStringPrintf(path, sizeof(path), "\\SystemRoot\\%s", KeLoaderBlock.Modules[i].Name);
        if (NT_SUCCESS(ObCreateObject(IoFileObjectType, path, sizeof(BOOT_FILE), &body))) {
            ((BOOT_FILE *)body)->PhysicalStart = KeLoaderBlock.Modules[i].Start;
            ((BOOT_FILE *)body)->Size = KeLoaderBlock.Modules[i].End - KeLoaderBlock.Modules[i].Start;
        }
    }
}
