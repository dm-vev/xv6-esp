# Picasso Graphics Server Module Makefile
# Build as loadable kernel module for xv6-esp

K=../../..
KBUILD=../../../build/xv6-riscv/kernel
MODULE_BUILD=$(KBUILD)/modules/picasso

# Module sources
SRCS = \
	picasso.c \
	picasso_tdeck.c

OBJS = $(addprefix $(MODULE_BUILD)/,$(SRCS:.c=.o))

# riscv64 toolchain
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-none-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy

# Compiler flags
CFLAGS = -Wall -Werror -O2 -fno-omit-frame-pointer -ggdb
CFLAGS += -march=rv64gc
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib
CFLAGS += -fPIC  # Position independent code for modules
CFLAGS += -DKMOD_MODULE_ABI_VER=1
CFLAGS += -I$(K)
CFLAGS += -I$(K)/kernel
CFLAGS += -I$(K)/applets/include

# Linker flags for module
LDFLAGS = -shared -nostdlib -z max-page-size=4096

# Module output
MODULE = picasso.ko

# Default target
all: $(MODULE)

# Create build directory
$(MODULE_BUILD):
	@mkdir -p $(MODULE_BUILD)

# Compile source files
$(MODULE_BUILD)/%.o: %.c | $(MODULE_BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Link module
$(MODULE): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Clean
clean:
	rm -rf $(MODULE_BUILD) $(MODULE)

# Install to fsroot (optional)
install: $(MODULE)
	cp $(MODULE) $(K)/fsroot/lib/modules/

.PHONY: all clean install
