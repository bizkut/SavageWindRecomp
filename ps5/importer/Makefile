# SPDX-License-Identifier: GPL-3.0-or-later
ROOT := $(abspath ../..)
WORK := $(ROOT)/build/ps5/importer/host
NSTOOL := $(ROOT)/../nstool2
PIN := 7e0a49b61ae98b730900709e3ac5ba1945c1906e
.PHONY: all exefs package package-test clean
all exefs package package-test:
	@test "$$(git -C $(NSTOOL) rev-parse HEAD)" = "$(PIN)"
	@test -z "$$(git -C $(NSTOOL) status --porcelain --untracked-files=no)"
	@! git -C $(NSTOOL) submodule status --recursive | grep -qE '^[+-]'
	@mkdir -p $(WORK)/source
	@tar -C $(NSTOOL) --exclude=.git --exclude='./bin' --exclude='./build' --exclude='*.o' -cf - . | tar -C $(WORK)/source -xf -
	@patch -s -d $(WORK)/source -p1 < $(ROOT)/ps5/importer/nstool-streams.patch
	@cmake -S $(ROOT)/ps5/importer -B $(WORK)/build -G Ninja -DNSTOOL_SOURCE=$(WORK)/source -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(WORK)/build -j2 --target $(if $(filter exefs,$@),savagewind-exefs,$(if $(filter package-test,$@),savagewind-package-test,$(if $(filter package,$@),savagewind-package,savagewind-import)))
	@if [ "$@" = package-test ]; then \
		test -n "$(KEYS)" -a -n "$(MANIFEST)" -a -n "$(FOLDER)" -a -n "$(BASE)" -a -n "$(UPDATE)" || \
			{ echo 'Set KEYS, MANIFEST, FOLDER, BASE and UPDATE' >&2; exit 2; }; \
		$(WORK)/build/savagewind-package-test "$(KEYS)" "$(MANIFEST)" "$(FOLDER)" "$(BASE)" "$(UPDATE)"; \
	fi
clean:
	@rm -rf $(WORK)
