CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
FLTK_CONFIG ?= fltk-config

TARGET = hamclock-backup
SOURCES = hamclock_backup.cpp

all: $(TARGET)

$(TARGET): $(SOURCES) cfg_info_format.h
	$(CXX) $(CXXFLAGS) $(shell $(FLTK_CONFIG) --cxxflags) -o $@ $(SOURCES) $(shell $(FLTK_CONFIG) --ldflags)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
