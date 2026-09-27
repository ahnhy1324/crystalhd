# SPDX-License-Identifier: LGPL-2.1-or-later
# Shared userspace policy. Kernel architecture remains a Kbuild choice.
LEGACY_CPU ?= 0
CRYSTALHD_ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

ifeq ($(LEGACY_CPU),1)
# Explicit feature switches (for example -mbmi2) can survive a later -march.
# Reject them instead of silently claiming the selected i686 baseline.
legacy_machine_flags := $(filter-out -m32,$(filter -m%,$(CC) $(CXX) $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS) $(LDFLAGS)))
ifneq ($(strip $(legacy_machine_flags)),)
$(error LEGACY_CPU=1 selects its own ISA; remove machine flags: $(legacy_machine_flags))
endif
override CRYSTALHD_CPU_FLAGS := -m32 -march=i686 -mtune=generic -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 -include $(CRYSTALHD_ROOT)/include/crystalhd_legacy.h
else ifeq ($(LEGACY_CPU),0)
override CRYSTALHD_CPU_FLAGS := $(CRYSTALHD_NORMAL_CPU_FLAGS)
else
$(error LEGACY_CPU must be 0 or 1)
endif

# A mode/flags change must rebuild in-tree outputs, in either direction.
# Compare before replacing the stamp so an unchanged build remains a no-op.
CRYSTALHD_CPU_STAMP := .crystalhd-cpu-config
CRYSTALHD_BUILD_DEPS = $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_ROOT)/cpu.mk $(CRYSTALHD_ROOT)/include/crystalhd_legacy.h
crystalhd_quote = '$(subst ','"'"',$(1))'
crystalhd_build_config = $(LEGACY_CPU)|$(CC)|$(CXX)|$(CPPFLAGS)|$(CFLAGS)|$(CXXFLAGS)|$(LDFLAGS)|$(LDLIBS)|$(CRYSTALHD_CPU_FLAGS)

.PHONY: crystalhd-cpu-force print-cpu-flags
print-cpu-flags:
	@printf '%s\n' $(call crystalhd_quote,$(CRYSTALHD_CPU_FLAGS))

$(CRYSTALHD_CPU_STAMP): crystalhd-cpu-force
	@printf '%s\n' $(call crystalhd_quote,$(crystalhd_build_config)) > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi
