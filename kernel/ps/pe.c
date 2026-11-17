/*
 * RadiaNT -- ps/pe.c
 * Loader for PE32+ (x64) native images. User programs are real Portable
 * Executables: this maps their sections into a fresh address space at the
 * preferred image base with per-section page protection.
 */
#include "ps.h"
#include "mm.h"
#include "rtl.h"

typedef struct { USHORT e_magic; USHORT pad[29]; LONG e_lfanew; } PACKED IMAGE_DOS_HEADER;
typedef struct {
    USHORT Machine, NumberOfSections;
    ULONG  TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    USHORT SizeOfOptionalHeader, Characteristics;
} PACKED IMAGE_FILE_HEADER;
typedef struct { ULONG VirtualAddress, Size; } PACKED IMAGE_DATA_DIRECTORY;
typedef struct {
    USHORT  Magic; UCHAR MajorLinkerVersion, MinorLinkerVersion;
    ULONG   SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    ULONG   AddressOfEntryPoint, BaseOfCode;
    ULONG64 ImageBase;
    ULONG   SectionAlignment, FileAlignment;
    USHORT  MajorOsVersion, MinorOsVersion, MajorImageVersion, MinorImageVersion;
    USHORT  MajorSubsystemVersion, MinorSubsystemVersion;
    ULONG   Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    USHORT  Subsystem, DllCharacteristics;
    ULONG64 SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    ULONG   LoaderFlags, NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} PACKED IMAGE_OPTIONAL_HEADER64;
typedef struct {
    UCHAR  Name[8];
    ULONG  VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
    ULONG  PointerToRelocations, PointerToLinenumbers;
    USHORT NumberOfRelocations, NumberOfLinenumbers;
    ULONG  Characteristics;
} PACKED IMAGE_SECTION_HEADER;

#define IMAGE_DOS_SIGNATURE         0x5A4D
#define IMAGE_NT_SIGNATURE          0x00004550
#define IMAGE_FILE_MACHINE_AMD64    0x8664
#define IMAGE_NT_OPTIONAL_HDR64     0x020B
#define IMAGE_SCN_MEM_EXECUTE       0x20000000
#define IMAGE_SCN_MEM_WRITE         0x80000000
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1

NTSTATUS PspLoadImage(PEPROCESS Process, const UCHAR *File, SIZE_T Size)
{
    ULONG64 cr3 = Process->Pcb.DirectoryTableBase;
    IMAGE_DOS_HEADER dos;
    if (Size < sizeof(dos)) return STATUS_INVALID_IMAGE_FORMAT;
    memcpy(&dos, File, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        (SIZE_T)dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64) > Size)
        return STATUS_INVALID_IMAGE_FORMAT;

    const UCHAR *nt = File + dos.e_lfanew;
    ULONG sig; memcpy(&sig, nt, 4);
    IMAGE_FILE_HEADER fh; memcpy(&fh, nt + 4, sizeof(fh));
    IMAGE_OPTIONAL_HEADER64 oh; memcpy(&oh, nt + 4 + sizeof(fh), sizeof(oh));
    if (sig != IMAGE_NT_SIGNATURE || fh.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        oh.Magic != IMAGE_NT_OPTIONAL_HDR64)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT &&
        oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size != 0)
        return STATUS_INVALID_IMAGE_FORMAT;         /* native images are statically linked */
    if (oh.ImageBase < 0x10000 || (oh.ImageBase & 0xFFFF) || oh.SizeOfImage > (256u << 20) ||
        oh.ImageBase < 0x100000000ull || oh.ImageBase + oh.SizeOfImage > 0x00007FF000000000ull ||
        oh.AddressOfEntryPoint >= oh.SizeOfImage)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Headers: one read-only page at the image base. */
    NTSTATUS st = MmAllocateUserMemory(cr3, oh.ImageBase, PAGE_SIZE, PTE_NX, &Process->PrivatePages);
    if (!NT_SUCCESS(st)) return st;
    MmCopyToUserSpace(cr3, oh.ImageBase, File, MIN(MIN(oh.SizeOfHeaders, PAGE_SIZE), Size));

    const UCHAR *sp = nt + 4 + sizeof(fh) + fh.SizeOfOptionalHeader;
    for (ULONG i = 0; i < fh.NumberOfSections; i++, sp += sizeof(IMAGE_SECTION_HEADER)) {
        IMAGE_SECTION_HEADER sh;
        if ((SIZE_T)(sp - File) + sizeof(sh) > Size) return STATUS_INVALID_IMAGE_FORMAT;
        memcpy(&sh, sp, sizeof(sh));
        ULONG vsize = MAX(sh.VirtualSize, sh.SizeOfRawData);
        if (!vsize) continue;
        if ((ULONG64)sh.VirtualAddress + vsize > oh.SizeOfImage ||
            (ULONG64)sh.PointerToRawData + sh.SizeOfRawData > Size)
            return STATUS_INVALID_IMAGE_FORMAT;
        ULONG64 flags = 0;
        if (sh.Characteristics & IMAGE_SCN_MEM_WRITE) flags |= PTE_WRITE;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_EXECUTE)) flags |= PTE_NX;
        st = MmAllocateUserMemory(cr3, oh.ImageBase + sh.VirtualAddress, vsize, flags, &Process->PrivatePages);
        if (!NT_SUCCESS(st)) return st;
        ULONG raw = sh.VirtualSize ? MIN(sh.SizeOfRawData, sh.VirtualSize) : sh.SizeOfRawData;
        st = MmCopyToUserSpace(cr3, oh.ImageBase + sh.VirtualAddress, File + sh.PointerToRawData, raw);
        if (!NT_SUCCESS(st)) return st;
    }
    Process->ImageBase = oh.ImageBase;
    Process->ImageSize = oh.SizeOfImage;
    Process->EntryPoint = oh.ImageBase + oh.AddressOfEntryPoint;
    return STATUS_SUCCESS;
}
