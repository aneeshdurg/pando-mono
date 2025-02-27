# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR  ?= $(shell git rev-parse --show-toplevel)
PLATFORM_DIR := $(DRV_DIR)/riscv-examples/platform_ph

SCRIPT   := $(DRV_DIR)/tests/PANDOHammerDrvR.py

RISCV_COMPILE_FLAGS += -nostartfiles
RISCV_COMPILE_FLAGS += -I$(DRV_DIR)/riscv-examples/platform_ph
RISCV_CFLAGS   += $(RISCV_COMPILE_FLAGS)
RISCV_CXXFLAGS += $(RISCV_COMPILE_FLAGS)

RISCV_LDFLAGS += -Wl,-T$(DRV_DIR)/riscv-examples/platform_ph/bsg_link.ld
RISCV_LDFLAGS += -L$(DRV_DIR)/riscv-examples/platform_ph/pandohammer

# include platform crt by default
RISCV_PLATFORM_CRT ?= yes
RISCV_PLATFORM_LIBC_LOCKING ?= $(RISCV_PLATFORM_CRT)
COMMAND_PROCESSOR_PLATFORM ?= yes
COMMAND_PROCESSOR_PLATFORM_LOADER ?= $(COMMAND_PROCESSOR_PLATFORM)

# platform command processor source
vpath %.cpp $(PLATFORM_DIR)/pandocommand
vpath %.c   $(PLATFORM_DIR)/pandocommand
COMMAND_PROCESSOR_PLATFORM_CXXSOURCE-$(COMMAND_PROCESSOR_PLATFORM_LOADER) += pandocommand_default.cpp
COMMAND_PROCESSOR_PLATFORM_CXXSOURCE-$(COMMAND_PROCESSOR_PLATFORM) += pandocommand_control.cpp
COMMAND_PROCESSOR_PLATFORM_CXXSOURCE-$(COMMAND_PROCESSOR_PLATFORM) += pandocommand_loader.cpp
COMMAND_PROCESSOR_PLATFORM_COMPILE_FLAGS-$(COMMAND_PROCESSOR_PLATFORM)    += -I$(PLATFORM_DIR)

# platform asm sources
RISCV_PLATFORM_ASMSOURCE-$(RISCV_PLATFORM_CRT) += crt.S
RISCV_PLATFORM_CSOURCE-$(RISCV_PLATFORM_LIBC_LOCKING) += lock.c


RISCV_ASMSOURCE                 += $(RISCV_PLATFORM_ASMSOURCE-yes)
RISCV_CSOURCE                   += $(RISCV_PLATFORM_CSOURCE-yes)
RISCV_CXXSOURCE                 += $(RISCV_PLATFORM_CXXSOURCE-yes)
COMMAND_PROCESSOR_CXXSOURCE     += $(COMMAND_PROCESSOR_PLATFORM_CXXSOURCE-yes)
COMMAND_PROCESSOR_CSOURCE       += $(COMMAND_PROCESSOR_PLATFORM_CSOURCE-yes)
COMMAND_PROCESSOR_COMPILE_FLAGS += $(COMMAND_PROCESSOR_PLATFORM_COMPILE_FLAGS-yes)
