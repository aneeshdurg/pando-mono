# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

DRV_DIR := $(shell git rev-parse --show-toplevel)
include $(DRV_DIR)/mk/config.mk
APP_NAME := fence
include $(DRV_DIR)/mk/application_common.mk
SIM_OPTIONS += --num-pxn=1 --pod-cores=1 --core-threads=2
