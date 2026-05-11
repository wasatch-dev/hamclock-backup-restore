CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
FLTK_CONFIG ?= fltk-config

TARGET = hamclock-backup
SOURCES = hamclock_backup.cpp

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install

USER_NAME ?= $(shell if [ -n "$$SUDO_USER" ]; then printf '%s' "$$SUDO_USER"; else id -un; fi)
USER_HOME ?= $(shell getent passwd $(USER_NAME) | cut -d: -f6)
DESKTOP_DIR ?= $(USER_HOME)/Desktop
HAMCLOCK_DIR ?= $(USER_HOME)/.hamclock
ICON_NAME = hamclock-backup.svg
ICON_PATH = $(HAMCLOCK_DIR)/$(ICON_NAME)
DESKTOP_FILE = $(DESKTOP_DIR)/HamClock\ Backup.desktop

all: $(TARGET)

$(TARGET): $(SOURCES) cfg_info_format.h
	$(CXX) $(CXXFLAGS) $(shell $(FLTK_CONFIG) --cxxflags) -o $@ $(SOURCES) $(shell $(FLTK_CONFIG) --ldflags)

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

install-desktop: install
	$(INSTALL) -d "$(HAMCLOCK_DIR)"
	$(INSTALL) -m 0755 hamclock-backup.desktop "$(HOME)/Desktop/HamClock Backup.desktop"
	gio set "$(HOME)/Desktop/HamClock Backup.desktop" metadata::trusted true || true
	$(INSTALL) -d "$(DESKTOP_DIR)"
	sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@ICON_PATH@|$(ICON_PATH)|g' hamclock-backup.desktop.in > "$(DESKTOP_DIR)/HamClock Backup.desktop"
	chmod 0755 "$(DESKTOP_DIR)/HamClock Backup.desktop"
	@echo "Installed desktop launcher: $(DESKTOP_DIR)/HamClock Backup.desktop"
	@echo "Installed icon: $(ICON_PATH)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall-desktop:
	rm -f "$(DESKTOP_DIR)/HamClock Backup.desktop"
	rm -f "$(ICON_PATH)"

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean install install-desktop uninstall uninstall-desktop
