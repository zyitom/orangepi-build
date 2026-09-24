# AR0234 userspace for the Orange Pi Zero 3W (A733), built natively on the board:
#   make -j8 && sudo make install
# libisp and cedarc export clashing iniparser symbols, so no binary links both.

CXX      ?= g++
CXXFLAGS ?= -O2 -g
# hwtest-include/ holds the sunxi-g2d UAPI header (src/g2d.cpp needs
# G2D_FORMAT_Y8, g2d_blt_h, ...); the kernel does not export it, so the copy in
# the repo is the one everybody builds against.
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-missing-field-initializers -Iinclude -I../hwtest-include -MMD -MP
PREFIX   ?= /usr/local
UNITDIR  ?= /etc/systemd/system
BUILD    := build

ISP_LIBS  := -lAWIspApi -lisp -lisp_ini -lpthread
VENC_LIBS := -lvencoder -lvenc_base -lvenc_codec -lVE -lMemAdapter -lcdc_base

# ar0234-g2d-selftest is built but not installed: it is the acceptance test for
# userspace/include/ar0234/g2d.hpp, not a shipped binary (packaging/build-deb.sh
# lists the payload explicitly).
# ar0234-npu-zerocopy is the C10 acceptance test (NEXT-TASKS.md): real ISP
# frames fed to the NPU via vip_create_buffer_from_fd and compared against a
# CPU-copy control. It links libNBGlinker.so (the VIPLite entry point that is
# present on the board) and takes vip_lite.h from hwapi/board-include (the
# copy of the board's /usr/include header). Built, not installed.
BINS := $(BUILD)/ar0234-3ad $(BUILD)/ar0234-rec $(BUILD)/ar0234-g2d-selftest $(BUILD)/ar0234-npu-zerocopy $(BUILD)/ar0234-exppoll

all: $(BINS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/ar0234-3ad: $(BUILD)/apps/ar0234-3ad.o $(BUILD)/src/v4l2.o $(BUILD)/src/isp3a.o $(BUILD)/src/ar0234conf.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(ISP_LIBS)

$(BUILD)/ar0234-rec: $(BUILD)/apps/ar0234-rec.o $(BUILD)/src/v4l2.o $(BUILD)/src/encoder.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(VENC_LIBS)

$(BUILD)/ar0234-g2d-selftest: $(BUILD)/apps/ar0234-g2d-selftest.o $(BUILD)/src/g2d.o $(BUILD)/src/v4l2.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(BUILD)/apps/ar0234-npu-zerocopy.o: CXXFLAGS += -I../hwapi/board-include
$(BUILD)/ar0234-npu-zerocopy: $(BUILD)/apps/ar0234-npu-zerocopy.o $(BUILD)/src/g2d.o $(BUILD)/src/v4l2.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lNBGlinker -lpthread

$(BUILD)/ar0234-exppoll: $(BUILD)/apps/ar0234-exppoll.o $(BUILD)/src/v4l2.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

# Optional, not in `all`: the real-OpenCV consumer sample (NEXT-TASKS N1).
# Needs libopencv-dev + pkg-config; built explicitly with `make cv-consumer`.
cv-consumer: $(BUILD)/ar0234-cv-consumer
$(BUILD)/apps/ar0234-cv-consumer.o: CXXFLAGS += $(shell pkg-config --cflags opencv4)
$(BUILD)/ar0234-cv-consumer: $(BUILD)/apps/ar0234-cv-consumer.o $(BUILD)/src/g2d.o $(BUILD)/src/v4l2.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(shell pkg-config --libs opencv4) -lpthread

install: $(BINS)
	install -D -m 755 $(BUILD)/ar0234-3ad $(DESTDIR)$(PREFIX)/bin/ar0234-3ad
	install -D -m 755 $(BUILD)/ar0234-rec $(DESTDIR)$(PREFIX)/bin/ar0234-rec
	install -D -m 644 systemd/ar0234-3ad.service $(DESTDIR)$(UNITDIR)/ar0234-3ad.service

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all install clean
