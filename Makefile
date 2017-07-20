PROG:=fota-demo

# We use some bashisms like pipefail.  The default GNU Make SHELL is /bin/sh
# which is bash on MacOS but not necessarily on Linux.  Explicitly set bash as
# the SHELL here.
SHELL=/bin/bash

# Specify the default target and toolchain to build.  The defaults are used
# if 'mbed target' and 'mbed toolchain' are not set.
DEFAULT_TARGET:=K64F
DEFAULT_TOOLCHAIN:=GCC_ARM

SRCDIR:=.
SRCS:=$(wildcard $(SRCDIR)/*.cpp)
HDRS:=$(wildcard $(SRCDIR)/*.h)
LIBS:=$(wildcard $(SRCDIR)/*.lib)

# Specify the path to the build profile.  If empty, the --profile option will
# not be provided to 'mbed compile' which causes it to use the builtin default.
ifeq (${DEBUG}, )
	BUILD_PROFILE:=mbed-os/tools/profiles/release.json
else
	BUILD_PROFILE:=mbed-os/tools/profiles/debug.json
endif

# Capture the GIT version that we are building from.  Later in the compile
# phase, this value will get built into the code.
DEVTAG:="$(shell git rev-parse --short HEAD)-$(shell git rev-parse --abbrev-ref HEAD)"
ifneq ("$(shell git status -s)","")
	DEVTAG:="${DEVTAG}-dev-${USER}"
endif

# Specifies the name of the target board to compile for
#
# The following methods are checked for a target board type, in order:
# 1. 'mbed target'.  To specify a target using this mechanism, run
# 	'mbed target <target>' in your build environment.
# 2. 'mbed detect'
# 3. otherwise a default target is used as specified at the top of this file
#
# Note this reads the file .mbed directly because 'mbed target'
# returns a "helpful" string instead of an empty string if no value is set.
MBED_TARGET:=$(shell cat .mbed 2>/dev/null | grep TARGET | awk -F'=' '{print $$2}')
ifeq (${MBED_TARGET},)
  MBED_TARGET:=$(shell mbed detect | grep "Detected" | awk '{ print $$3 }' | sed 's/,//')
  ifeq (${MBED_TARGET},)
    MBED_TARGET:=${DEFAULT_TARGET}
  else
    # We only support K64F at this time
    ifneq (${MBED_TARGET},K64F)
      $(error We only support K64F at this time.)
    endif
  endif
endif

# Specifies the name of the toolchain to use for compilation
#
# The following methods are checked for a toolchain, in order:
# 1. 'mbed toolchain'.  To specify a toolchain using this mechanism, run
# 	'mbed toolchain <toolchain>' in your build environment.
# 2. otherwise a default toolchain is used as specified at the top of this file
#
# Note this reads the file .mbed directly because 'mbed toolchain'
# returns a "helpful" string instead of an empty string if no value is set.
MBED_TOOLCHAIN:=$(shell cat .mbed 2>/dev/null | grep TOOLCHAIN | awk -F'=' '{print $$2}')
ifeq (${MBED_TOOLCHAIN},)
  MBED_TOOLCHAIN:=${DEFAULT_TOOLCHAIN}
endif

# Specifies the name of the build profile
#
# This simply copies the value of the BUILD_PROFILE variable
# if the variable is not empty and the file exists.
ifeq ($(wildcard ${BUILD_PROFILE}),)
	MBED_PROFILE:=
else
	MBED_PROFILE:=${BUILD_PROFILE}
endif

# Specifies the path to the directory containing build output files
MBED_BUILD_DIR:=./BUILD/${MBED_TARGET}/${MBED_TOOLCHAIN}

# This file is the combination of the bootloader and the program.
# Initially flash this file via the USB interface.
# Subsequently, update using just the program file.
COMBINED_BIN_FILE:=${MBED_BUILD_DIR}/combined.bin

# Determine the correct bootloader and patches to use
# The linker script patch allows the compiled application to run after the mbed bootloader.
# The ram patch gives us more than 130k of ram to use
ifeq (${MBED_TARGET},K64F)
  BOOTLOADER:=tools/mbed-bootloader-k64f.bin
  APP_OFFSET:=0x20400
  HEADER_OFFSET:=0x20000
  ifeq (${MBED_TOOLCHAIN},GCC_ARM)
    PATCHES:=../tools/MK64FN1M0xxx12.ld.diff ../tools/gcc_k64f_ram_patch.diff
  else ifeq (${MBED_TOOLCHAIN},IAR)
    PATCHES:=../tools/MK64FN1M0xxx12.icf.diff
  else ifeq (${MBED_TOOLCHAIN},ARM)
    PATCHES:=./tools/MK64FN1M0xxx12.sct.diff
  endif
endif

# Builds the command to call 'mbed compile'.
# $1: add extra options to the final command line
# $2: override all command line arguments.  final command is 'mbed compile $2'
define Build/Compile
	opts=""; \
	extra_opts=${1}; \
	force_opts=${2}; \
	opts="$${opts} -t ${MBED_TOOLCHAIN}"; \
	opts="$${opts} -m ${MBED_TARGET}"; \
	[ -n "${MBED_PROFILE}" ] && { \
		opts="$${opts} --profile ${MBED_PROFILE}"; \
	}; \
	[ -n "$${extra_opts}" ] && { \
		opts="$${opts} $${extra_opts}"; \
	}; \
	[ -n "$${force_opts}" ] && { \
		opts="$${force_opts}"; \
	}; \
	opts="$${opts} -N ${PROG}"; \
	cmd="mbed compile $${opts}"; \
	echo "$${cmd}"; \
	$${cmd}
	tools/combine_bootloader_with_app.py -b ${BOOTLOADER} -a ${MBED_BUILD_DIR}/${PROG}.bin --app-offset ${APP_OFFSET} --header-offset ${HEADER_OFFSET} -o ${COMBINED_BIN_FILE}
endef

.PHONY: all
all: build

.PHONY: clean-build
clean-build: .deps .patches update_default_resources.c
	@$(call Build/Compile,"--clean")

.PHONY: build
build: .deps .patches update_default_resources.c
	@$(call Build/Compile,"-DDEVTAG=${DEVTAG}")

# Update global config with local config if it exists,
# or else it just copies the gobal config.
mbed_app.json:
	python merge_json.py config_global.json config_local.json > mbed_app.json

${COMBINED_BIN_FILE}: .deps .patches update_default_resources.c ${SRCS} ${HDRS} mbed_app.json
	@$(call Build/Compile,"-DDEVTAG=${DEVTAG}")

${MBED_BUILD_DIR}/${PROG}.bin: .deps .patches update_default_resources.c ${SRCS} ${HDRS} mbed_app.json
	@$(call Build/Compile,"-DDEVTAG=${DEVTAG}")

.PHONY: stats
stats:
	@cmd="python mbed-os/tools/memap.py -d -t ${MBED_TOOLCHAIN} ${MBED_BUILD_DIR}/${PROG}.map"; \
	echo "$${cmd}"; \
	$${cmd}

.PHONY: install flash
install flash: .targetpath $(COMBINED_BIN_FILE)
	@cmd="cp ${COMBINED_BIN_FILE} $$(cat .targetpath)"; \
	echo "$${cmd}"; \
	$${cmd}

tags: Makefile $(SRCS) $(HDRS)
	ctags -R

.PHONY: clean
clean:
	rm -rf BUILD
	rm -f mbed_app.json

.PHONY: distclean
distclean: clean
	rm -rf Chainable_RGB_LED
	rm -rf esp8266_driver
	rm -rf mbed-os
	rm -rf sd-driver
	rm -rf ws2801
	rm -rf mbed-cloud-client-restricted
	rm -rf mbed-cloud-client-internal
	rm -rf TextLCD
	rm -rf manifest-tool-restricted
	rm -f update_default_resources.c
	rm -f .deps
	rm -f .targetpath
	rm -f .patches
	rm -f .firmware-url
	rm -f .manifest-id
	rm -f .manifest_tool.json
	rm -f ${MANIFEST_FILE}

.mbed:
	mbed config ROOT .

.deps: .mbed ${LIBS} mbed_app.json
	mbed deploy --protocol ssh && touch .deps

# Acquire (and cache) the mount point of the board.
# If this fails, check that the board is mounted, and 'mbed detect' works.
# If the mount point changes, run 'make distclean'
.targetpath: .deps
	@set -o pipefail; TARGETPATH=$$(mbed detect | grep "mounted" | awk '{ print $$NF }') && \
		(echo $$TARGETPATH > .targetpath) || \
		(echo Error: could not detect mount path for the mbed board.  Verify that 'mbed detect' works.; exit 1)

.patches: .deps
	cd mbed-os && git apply ../tools/${PATCHES}
	touch .patches

################################################################################
# Update related rules
################################################################################

update_default_resources.c: .deps
	@which manifest-tool || (echo Error: manifest-tool not found.  Install it with \"pip install git+ssh://git@github.com/ARMmbed/manifest-tool-restricted.git@v1.2rc2\"; exit 1)
	manifest-tool init -d "arm.com" -m "fota-demo" -q
	touch update_default_resources.c

.mbed-cloud-key:
	@echo "Error: You need to save an mbed cloud API key in .mbed-cloud-key"
	@echo "Please go to https://cloud.mbed.com/docs/v1.2/mbed-cloud-web-apps/access-mbed-cloud-with-api-keys.html"
	@exit 1

.PHONY: campaign
campaign: .deps .mbed-cloud-key .manifest-id
	python mbed-cloud-update-cli/create-campaign.py $$(cat .manifest-id) --key-file .mbed-cloud-key

MANIFEST_FILE=dev-manifest
.manifest-id: .firmware-url .mbed-cloud-key ${COMBINED_BIN_FILE}
	@which manifest-tool || (echo Error: manifest-tool not found.  Install it with \"pip install git+ssh://git@github.com/ARMmbed/manifest-tool-restricted.git@v1.2rc2\"; exit 1)
	manifest-tool create -u $$(cat .firmware-url) -p ${MBED_BUILD_DIR}/${PROG}.bin -o ${MANIFEST_FILE}
	python mbed-cloud-update-cli/upload-manifest.py ${MANIFEST_FILE} --key-file .mbed-cloud-key -o $@

.firmware-url: .mbed-cloud-key ${COMBINED_BIN_FILE}
	python mbed-cloud-update-cli/upload-firmware.py ${MBED_BUILD_DIR}/${PROG}.bin --key-file .mbed-cloud-key -o $@

.PHONY: certclean
certclean:
	rm -rf .update-certificates
	rm -rf .manifest_tool.json
	rm -f update_default_resources.c
