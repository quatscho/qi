# Variables
CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lncurses
TARGET = qi
INSTALL_DIR = $(HOME)/bin

# Default target to compile the executable
all: $(TARGET)

$(TARGET): qi.c
	$(CC) $(CFLAGS) qi.c -o $(TARGET) $(LIBS)

# Install target to move the binary to ~/bin
install: $(TARGET)
	mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/$(TARGET)

# Clean up build files in the local directory
clean:
	rm -f $(TARGET)

.PHONY: all install clean
