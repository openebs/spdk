#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.


export SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

RUST_SRC_DIR = $(SPDK_ROOT_DIR)/rust
RUST_TGT_DIR := $(abspath $(CURDIR)/target)
RUST_OUT_DIR := $(RUST_TGT_DIR)/debug

.PHONY: all clean install uninstall

all:
	$(Q)cd $(RUST_SRC_DIR) && cargo build --target-dir=$(RUST_TGT_DIR)
	$(Q)cp $(RUST_OUT_DIR)/*.a $(SPDK_ROOT_DIR)/build/lib/

clean:
	$(Q)rm -rf $(RUST_TGT_DIR)

install:
	@:

uninstall:
	@:
