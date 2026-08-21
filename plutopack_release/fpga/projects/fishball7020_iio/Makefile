####################################################################################
## Copyright (c) 2018 - 2023 Analog Devices, Inc.
### SPDX short identifier: BSD-1-Clause
## Auto-generated, do not modify!
####################################################################################

PROJECT_NAME := fishball

M_DEPS += ../../adi-hdl/library/common/ad_iobuf.v
M_DEPS += ../../adi-hdl/library/axi_ad9361/axi_ad9361_delay.tcl
M_DEPS += ip_cores

LIB_DEPS += axi_ad9361

include ../../adi-hdl/projects/scripts/project-xilinx.mk


DEST = ../../../../tezuka_fw/board/fishball_7020/bitstream

ip_cores:
	$(MAKE) -C ../../ip

fsbl:
	bash -c "rm -rf build"
	bash -c "xsct create_fsbl_project.tcl"

install :
	bash -c "cp -f $(PROJECT_NAME).sdk/system_top.xsa $(DEST)/maia-iio/"
	bash -c "cp -f build/sdk/fsbl/Release/fsbl.elf $(DEST)/"
	bash "./all_overclock.sh"
	bash -c "cp -f build/sdk/fsbl/Release/fsbl*.elf $(DEST)/overclock/"
	bash -c "git describe > $(DEST)/overclock/bit.txt"
.PHONY: ip_cores fsbl install

all: ip_cores fsbl install