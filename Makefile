CXX ?= g++
TARGET = hamclock-backup

CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
FLTK_CXXFLAGS := $(shell fltk-config --cxxflags)
FLTK_LDFLAGS := $(shell fltk-config --ldflags)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install

$(TARGET): hamclock_backup.cpp cfg_info_format.h
	$(CXX) $(CXXFLAGS) $(FLTK_CXXFLAGS) -o $@ hamclock_backup.cpp $(FLTK_LDFLAGS)

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
