# Variables
CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lncurses
TARGET = qi
SRCS = qi.c tracker.c syntax.c color.c
INSTALL_DIR = /usr/local/bin

# Detect Operating System
UNAME_S := $(shell uname -s)

# --- AUTOMATIC HOMEBREW PATH DISCOVERY ---
# If 'brew' is available, append its include and library directories dynamically
ifeq ($(shell which brew > /dev/null 2>&1 && echo yes),yes)
    BREW_PREFIX = $(shell brew --prefix)
    CFLAGS += -I$(BREW_PREFIX)/include
    LDFLAGS += -L$(BREW_PREFIX)/lib
endif

# Default target to compile the executable
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $(TARGET) $(LIBS)

# Install target  defaults to /usr/local/bin.
# Use 'make install LOCAL=1' to install to ~/bin instead.
ifdef LOCAL
INSTALL_DIR = $(HOME)/bin
endif

install: $(TARGET)
	mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/$(TARGET)
ifeq ($(UNAME_S),Darwin)
	codesign -s - -f $(INSTALL_DIR)/$(TARGET)
endif

# Clean up build files in the local directory
clean:
	rm -f $(TARGET)

.PHONY: all install clean
