# AR0234 userspace for the Orange Pi Zero 3W (A733), built natively on the board:
#   make -j8 && sudo make install
# libisp and cedarc export clashing iniparser symbols, so no binary links both.

CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-missing-field-initializers -Iinclude -MMD -MP
PREFIX   ?= /usr/local
UNITDIR  ?= /etc/systemd/system
BUILD    := build

ISP_LIBS  := -lAWIspApi -lisp -lisp_ini -lpthread
VENC_LIBS := -lvencoder -lvenc_base -lvenc_codec -lVE -lMemAdapter -lcdc_base

BINS := $(BUILD)/ar0234-3ad $(BUILD)/ar0234-rec

all: $(BINS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/ar0234-3ad: $(BUILD)/apps/ar0234-3ad.o $(BUILD)/src/v4l2.o $(BUILD)/src/isp3a.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(ISP_LIBS)

$(BUILD)/ar0234-rec: $(BUILD)/apps/ar0234-rec.o $(BUILD)/src/v4l2.o $(BUILD)/src/encoder.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(VENC_LIBS)

install: $(BINS)
	install -D -m 755 $(BUILD)/ar0234-3ad $(DESTDIR)$(PREFIX)/bin/ar0234-3ad
	install -D -m 755 $(BUILD)/ar0234-rec $(DESTDIR)$(PREFIX)/bin/ar0234-rec
	install -D -m 644 systemd/ar0234-3ad.service $(DESTDIR)$(UNITDIR)/ar0234-3ad.service

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all install clean
