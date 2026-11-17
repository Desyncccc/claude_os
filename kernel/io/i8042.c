/*
 * RadiaNT -- io/i8042.c
 * \Driver\i8042prt: PS/2 controller port driver exposing
 * \Device\KeyboardClass0 and \Device\PointerClass0.
 *
 * The ISR only moves bytes into ring buffers and queues a DPC; the DPC signals
 * IoInputEvent; consumers drain the queues with IRP_MJ_READ.
 */
#include "io.h"
#include "hal.h"
#include "rtl.h"

#define I8042_DATA   0x60
#define I8042_STATUS 0x64
#define I8042_CMD    0x64

KEVENT IoInputEvent;

static KEYBOARD_INPUT_DATA KbdQueue[128];
static MOUSE_INPUT_DATA    MouQueue[256];
static volatile ULONG KbdHead, KbdTail, MouHead, MouTail;
static KDPC    I8042Dpc;
static BOOLEAN KbdE0;
static UCHAR   MouPacket[3];
static ULONG   MouIndex;
static BOOLEAN MousePresent;

typedef struct _I8042_EXTENSION { BOOLEAN IsMouse; } I8042_EXTENSION;

static void I8042WaitWrite(void)
{
    for (int i = 0; i < 100000 && (HalIn8(I8042_STATUS) & 2); i++) HalIoDelay();
}

static BOOLEAN I8042WaitRead(void)
{
    for (int i = 0; i < 100000; i++) {
        if (HalIn8(I8042_STATUS) & 1) return TRUE;
        HalIoDelay();
    }
    return FALSE;
}

static void I8042Command(UCHAR c) { I8042WaitWrite(); HalOut8(I8042_CMD, c); }
static void I8042Write(UCHAR d)   { I8042WaitWrite(); HalOut8(I8042_DATA, d); }
static void I8042Flush(void)
{
    for (int i = 0; i < 64 && (HalIn8(I8042_STATUS) & 1); i++) { HalIn8(I8042_DATA); HalIoDelay(); }
}

static BOOLEAN I8042MouseCommand(UCHAR c)
{
    I8042Command(0xD4);
    I8042Write(c);
    for (int tries = 0; tries < 4; tries++) {
        if (!I8042WaitRead()) return FALSE;
        if (HalIn8(I8042_DATA) == 0xFA) return TRUE;
    }
    return FALSE;
}

static void I8042DpcRoutine(KDPC *Dpc, PVOID Context)
{
    (void)Dpc; (void)Context;
    KeSetEvent(&IoInputEvent);
}

static void I8042Interrupt(PVOID Context)
{
    (void)Context;
    BOOLEAN queued = FALSE;
    for (int budget = 0; budget < 32; budget++) {
        UCHAR status = HalIn8(I8042_STATUS);
        if (!(status & 1)) break;
        UCHAR data = HalIn8(I8042_DATA);
        if (status & 0x20) {                        /* auxiliary (mouse) byte */
            if (MouIndex == 0 && !(data & 0x08)) continue;      /* resynchronise */
            MouPacket[MouIndex++] = data;
            if (MouIndex == 3) {
                MouIndex = 0;
                if (MouPacket[0] & 0xC0) continue;  /* overflow: discard */
                ULONG next = (MouHead + 1) % ARRAY_SIZE(MouQueue);
                if (next != MouTail) {
                    MOUSE_INPUT_DATA *m = &MouQueue[MouHead];
                    m->LastX = (LONG)MouPacket[1] - ((MouPacket[0] & 0x10) ? 256 : 0);
                    m->LastY = -((LONG)MouPacket[2] - ((MouPacket[0] & 0x20) ? 256 : 0));
                    m->Buttons = MouPacket[0] & 7;
                    m->Flags = 0;
                    MouHead = next;
                    queued = TRUE;
                }
            }
        } else {                                    /* keyboard byte (scan code set 1) */
            if (data == 0xE0) { KbdE0 = TRUE; continue; }
            if (data == 0xE1 || data == 0xFA) { KbdE0 = FALSE; continue; }     /* pause prefix, ACK */
            ULONG next = (KbdHead + 1) % ARRAY_SIZE(KbdQueue);
            if (next != KbdTail) {
                KbdQueue[KbdHead].MakeCode = data & 0x7F;
                KbdQueue[KbdHead].Flags = ((data & 0x80) ? KEY_BREAK : KEY_MAKE) | (KbdE0 ? KEY_E0 : 0);
                KbdHead = next;
                queued = TRUE;
            }
            KbdE0 = FALSE;
        }
    }
    if (queued) KeInsertQueueDpc(&I8042Dpc);
}

static NTSTATUS I8042Read(PDEVICE_OBJECT Device, PIRP Irp)
{
    I8042_EXTENSION *ext = Device->DeviceExtension;
    ULONG_PTR copied = 0;
    ULONG64 f = HalSaveFlagsAndCli();
    if (ext->IsMouse) {
        MOUSE_INPUT_DATA *out = Irp->SystemBuffer;
        ULONG max = Irp->Length / sizeof(*out);
        while (copied < max && MouTail != MouHead) {
            out[copied++] = MouQueue[MouTail];
            MouTail = (MouTail + 1) % ARRAY_SIZE(MouQueue);
        }
        copied *= sizeof(*out);
    } else {
        KEYBOARD_INPUT_DATA *out = Irp->SystemBuffer;
        ULONG max = Irp->Length / sizeof(*out);
        while (copied < max && KbdTail != KbdHead) {
            out[copied++] = KbdQueue[KbdTail];
            KbdTail = (KbdTail + 1) % ARRAY_SIZE(KbdQueue);
        }
        copied *= sizeof(*out);
    }
    HalRestoreFlags(f);
    return IoCompleteRequest(Irp, STATUS_SUCCESS, copied);
}

NTSTATUS I8042DriverEntry(PDRIVER_OBJECT Driver)
{
    KeInitializeEvent(&IoInputEvent, EventSynchronizationObject, FALSE);
    KeInitializeDpc(&I8042Dpc, I8042DpcRoutine, NULL);
    Driver->MajorFunction[IRP_MJ_READ] = I8042Read;

    PDEVICE_OBJECT kbd, mou;
    NTSTATUS st = IoCreateDevice(Driver, "KeyboardClass0", FILE_DEVICE_KEYBOARD, sizeof(I8042_EXTENSION), &kbd);
    if (!NT_SUCCESS(st)) return st;
    st = IoCreateDevice(Driver, "PointerClass0", FILE_DEVICE_MOUSE, sizeof(I8042_EXTENSION), &mou);
    if (!NT_SUCCESS(st)) return st;
    ((I8042_EXTENSION *)mou->DeviceExtension)->IsMouse = TRUE;

    ULONG64 f = HalSaveFlagsAndCli();
    I8042Command(0xAD);                     /* disable both ports while configuring */
    I8042Command(0xA7);
    I8042Flush();
    I8042Command(0x20);
    UCHAR config = I8042WaitRead() ? HalIn8(I8042_DATA) : 0x45;
    config |= 0x03 | 0x40;                  /* IRQ1 + IRQ12, scan code translation */
    config &= ~0x30;                        /* enable both clocks */
    I8042Command(0x60);
    I8042Write(config);
    I8042Command(0xAE);
    I8042Command(0xA8);
    I8042Flush();
    I8042MouseCommand(0xF6);                /* defaults */
    MousePresent = I8042MouseCommand(0xF4); /* enable data reporting */
    I8042Flush();
    HalRestoreFlags(f);

    HalConnectInterrupt(1, I8042Interrupt, NULL, "i8042prt keyboard");
    HalConnectInterrupt(12, I8042Interrupt, NULL, "i8042prt mouse");
    DbgPrint("i8042prt: keyboard ready, mouse %s\n", MousePresent ? "ready" : "not detected");
    return STATUS_SUCCESS;
}
