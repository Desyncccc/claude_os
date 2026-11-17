/* RadiaNT -- io.h : I/O manager (drivers, devices, I/O request packets). */
#pragma once
#include "ntdef.h"
#include "ke.h"
#include "ob.h"

#define IRP_MJ_CREATE           0x00
#define IRP_MJ_CLOSE            0x02
#define IRP_MJ_READ             0x03
#define IRP_MJ_WRITE            0x04
#define IRP_MJ_DEVICE_CONTROL   0x0E
#define IRP_MJ_MAXIMUM_FUNCTION 0x1B

#define FILE_DEVICE_KEYBOARD    0x0B
#define FILE_DEVICE_MOUSE       0x0F
#define FILE_DEVICE_NULL        0x15
#define FILE_DEVICE_SERIAL_PORT 0x1B
#define FILE_DEVICE_VIDEO       0x23

typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK;

struct _DEVICE_OBJECT; struct _DRIVER_OBJECT;
typedef struct _IRP {
    UCHAR                  MajorFunction;
    ULONG                  IoControlCode;
    PVOID                  SystemBuffer;
    ULONG                  Length;
    IO_STATUS_BLOCK        IoStatus;
    struct _DEVICE_OBJECT *DeviceObject;
} IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *Device, PIRP Irp);
typedef NTSTATUS (*PDRIVER_INITIALIZE)(struct _DRIVER_OBJECT *Driver);

typedef struct _DRIVER_OBJECT {
    CHAR                   Name[32];
    CHAR                   Description[64];
    PDRIVER_DISPATCH       MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    struct _DEVICE_OBJECT *DeviceObject;       /* singly linked list */
    ULONG                  DeviceCount;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _DEVICE_OBJECT {
    PDRIVER_OBJECT         DriverObject;
    struct _DEVICE_OBJECT *NextDevice;
    ULONG                  DeviceType;
    PVOID                  DeviceExtension;
    ULONG64                IrpCount;
    CHAR                   Name[32];
} DEVICE_OBJECT, *PDEVICE_OBJECT;

extern POBJECT_TYPE IoDriverObjectType, IoDeviceObjectType;

void     IoInitSystem(void);
NTSTATUS IoCreateDriver(PCSTR Name, PCSTR Description, PDRIVER_INITIALIZE Entry);
NTSTATUS IoCreateDevice(PDRIVER_OBJECT Driver, PCSTR Name, ULONG Type, SIZE_T ExtensionSize,
                        PDEVICE_OBJECT *Device);
NTSTATUS IoCallDriver(PDEVICE_OBJECT Device, PIRP Irp);
NTSTATUS IoSynchronousRequest(PDEVICE_OBJECT Device, UCHAR Major, ULONG Ioctl,
                              PVOID Buffer, ULONG Length, ULONG_PTR *Information);
static inline NTSTATUS IoCompleteRequest(PIRP Irp, NTSTATUS Status, ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status; Irp->IoStatus.Information = Info; return Status;
}

/* ---- Input class data (i8042prt) ---- */
#define KEY_MAKE  0
#define KEY_BREAK 1
#define KEY_E0    2
typedef struct _KEYBOARD_INPUT_DATA { USHORT MakeCode; USHORT Flags; } KEYBOARD_INPUT_DATA;
#define MOUSE_LEFT_BUTTON   1
#define MOUSE_RIGHT_BUTTON  2
#define MOUSE_MIDDLE_BUTTON 4
typedef struct _MOUSE_INPUT_DATA { LONG LastX, LastY; USHORT Buttons; USHORT Flags; } MOUSE_INPUT_DATA;
extern KEVENT IoInputEvent;         /* set whenever input data is queued */

/* ---- Video miniport ---- */
#define IOCTL_VIDEO_QUERY_CURRENT_MODE 0x00230400
typedef struct _VIDEO_MODE_INFORMATION { ULONG Width, Height, Pitch, BitsPerPixel; } VIDEO_MODE_INFORMATION;

/* Built-in boot-start drivers */
NTSTATUS NullDriverEntry(PDRIVER_OBJECT Driver);
NTSTATUS SerialDriverEntry(PDRIVER_OBJECT Driver);
NTSTATUS I8042DriverEntry(PDRIVER_OBJECT Driver);
NTSTATUS VideoDriverEntry(PDRIVER_OBJECT Driver);
