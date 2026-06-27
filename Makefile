# Variables
CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lncurses
TARGET = qi
SRCS = qi.c tracker.c
INSTALL_DIR = $(HOME)/bin

# Default target to compile the executable
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LIBS)

# Install target to move the binary to ~/bin
install: $(TARGET)
	mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/$(TARGET)

# Clean up build files in the local directory
clean:
	rm -f $(TARGET)

.PHONY: all install clean
