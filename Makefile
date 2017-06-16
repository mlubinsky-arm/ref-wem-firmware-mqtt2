PROG:=fota-demo

# Specify the default target and toolchain to build.  The defaults are used
# if 'mbed target' and 'mbed toolchain' are not set.
DEFAULT_TARGET:=K64F
DEFAULT_TOOLCHAIN:=GCC_ARM

SRCDIR:=.
SRCS:=$(wildcard $(SRCDIR)/*.cpp)
HDRS:=$(wildcard $(SRCDIR)/*.h)

# Specify the path to the build profile.  If empty, the --profile option will
# not be provided to 'mbed compile' which causes it to use the builtin default.
ifeq (${DEBUG}, )
	BUILD_PROFILE:=mbed-os/tools/profiles/release.json
else
	BUILD_PROFILE:=mbed-os/tools/profiles/debug.json
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

# Specifies the path to DAPLINK for firmware flashing
MBED_DAPLINK_MOUNT_PATH:=$(shell mbed detect | grep "mounted" | awk '{ print $$NF }')

# Specifies the path to the directory containing build output files
MBED_BUILD_DIR:="./BUILD/${MBED_TARGET}/${MBED_TOOLCHAIN}"

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
	cmd="mbed compile $${opts}"; \
	echo "$${cmd}"; \
	$${cmd}
endef

.PHONY: all
all: build

.PHONY: clean-build
clean-build: .deps
	@$(call Build/Compile,"--clean")

.PHONY: build
build: .deps
	@$(call Build/Compile)

.PHONY: stats
stats:
	@cmd="python mbed-os/tools/memap.py -d -t ${MBED_TOOLCHAIN} ${MBED_BUILD_DIR}/${PROG}.map"; \
	echo "$${cmd}"; \
	$${cmd}

.PHONY: install flash
install flash:
	@cmd="cp ${MBED_BUILD_DIR}/${PROG}.bin ${MBED_DAPLINK_MOUNT_PATH}"; \
	echo "$${cmd}"; \
	$${cmd}

tags: Makefile $(SRCS) $(HDRS)
	ctags -R

.PHONY: clean
clean:
	rm -rf BUILD

.PHONY: distclean
distclean:
	rm -rf Chainable_RGB_LED
	rm -rf esp8266_driver
	rm -rf mbed-os
	rm -f .deps

.deps:
	mbed deploy && touch .deps
