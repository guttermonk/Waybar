PLUGIN_NAME = taskbar-switcher

CXX = g++
CXXFLAGS = -shared -fPIC -std=c++23 -O2
CXXFLAGS += $(shell pkg-config --cflags hyprland pixman-1 libdrm)
LDFLAGS = $(shell pkg-config --libs hyprland 2>/dev/null)

SRC = taskbar-switcher.cpp
OUT = $(PLUGIN_NAME).so

PREFIX ?= /usr/lib/hyprland

.PHONY: all clean install

all: $(OUT)

$(OUT): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(OUT)

install: $(OUT)
	install -Dm755 $(OUT) $(DESTDIR)$(PREFIX)/$(OUT)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/$(OUT)