# RadiaNT OS -- build system
#
#   make            build the kernel, the user programs and the bootable ISO
#   make test       boot the ISO headless in QEMU and check that the desktop starts
#   make site       copy the ISO + checksum into docs/download for the website
#   make run        boot the ISO in QEMU (BIOS)
#   make run-uefi   boot the ISO in QEMU with OVMF (UEFI)
#   make FB_WIDTH=1920 FB_HEIGHT=1080   build for another preferred resolution
#   make fonts      regenerate kernel/win32k/fontdata.c (needs Pillow + fonts)
#   make clean
#
# Requirements: gcc, binutils, nasm, clang + lld (PE user programs),
#               grub-mkrescue (grub-pc-bin, grub-efi-amd64-bin), xorriso, mtools

BUILD   := build
FB_WIDTH  ?= 1280
FB_HEIGHT ?= 720
ISO     := $(BUILD)/radiant-x64.iso
KERNEL  := $(BUILD)/radkrnl.elf

CC      := gcc
AS      := nasm
LD      := ld
CLANG   := clang
LLDLINK := lld-link

CFLAGS  := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -fno-omit-frame-pointer \
           -fno-asynchronous-unwind-tables -fno-strict-aliasing -fcf-protection=none \
           -Wall -Wextra -Wno-unused-parameter -Ikernel/include
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments --build-id=none -T kernel/linker.ld

UCFLAGS := --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-stack-protector -fno-builtin \
           -mno-red-zone -mgeneral-regs-only -mno-stack-arg-probe -Wall -Wno-pragma-pack
ULFLAGS := /subsystem:native /entry:NtProcessStartup /nodefaultlib /fixed /dynamicbase:no \
           /base:0x140000000 /nologo

C_SRCS   := $(shell find kernel -name '*.c' | sort)
ASM_SRCS := kernel/boot/start.asm kernel/ke/amd64.asm
OBJS     := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS)) $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
HEADERS  := $(wildcard kernel/include/*.h)

PROGRAMS := hello primes clock crash spin
EXES     := $(patsubst %,$(BUILD)/user/%.exe,$(PROGRAMS))

.PHONY: all kernel user iso site test run run-uefi fonts clean
all: iso
kernel: $(KERNEL)
user: $(EXES)
iso: $(ISO)

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 -DFB_WIDTH=$(FB_WIDTH) -DFB_HEIGHT=$(FB_HEIGHT) $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/user/%.obj: user/%.c user/ntdll.h kernel/include/ntapi.h
	@mkdir -p $(dir $@)
	$(CLANG) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.exe: $(BUILD)/user/%.obj $(BUILD)/user/crt.obj
	$(LLDLINK) $(ULFLAGS) /out:$@ $^

$(ISO): $(KERNEL) $(EXES) boot/grub.cfg
	@rm -rf $(BUILD)/iso
	@mkdir -p $(BUILD)/iso/boot/grub $(BUILD)/iso/system
	cp $(KERNEL) $(BUILD)/iso/boot/radkrnl.elf
	strip --strip-all $(BUILD)/iso/boot/radkrnl.elf
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	cp $(EXES) $(BUILD)/iso/system/
	grub-mkrescue -o $@ $(BUILD)/iso --locales="" --fonts="" --themes="" \
	    --install-modules="normal multiboot2 all_video iso9660 part_gpt part_msdos search configfile echo test boot fat" \
	    > $(BUILD)/mkrescue.log 2>&1 || (cat $(BUILD)/mkrescue.log; false)
	@ls -l $@

# Refresh the copy of the ISO that the website (docs/) offers for download.
site: $(ISO)
	@mkdir -p docs/download
	cp $(ISO) docs/download/radiant-x64.iso
	python3 tools/mkrelease.py $(ISO) docs/download

test: $(ISO)
	tools/smoke-test.sh $(ISO)

run: $(ISO)
	qemu-system-x86_64 -m 512 -cdrom $(ISO) -serial stdio -vga std

run-uefi: $(ISO)
	qemu-system-x86_64 -m 512 -cdrom $(ISO) -serial stdio -vga std \
	    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd

fonts:
	python3 tools/mkfont.py > kernel/win32k/fontdata.c

clean:
	rm -rf $(BUILD)
