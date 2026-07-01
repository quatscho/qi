# qi

**qi** (pronounced *key*) is a lightweight, terminal-based text editor for macOS and Linux. It is built on ncurses and written in C, with an emphasis on simplicity, low memory use, and files of any size.

## Building from source

### Dependencies

qi requires the ncurses library and a C compiler.

### macOS

Install the Xcode Command Line Tools if you have not already:

```sh
xcode-select --install
```

ncurses is included with macOS. Clone the repository and build:

```sh
git clone https://github.com/quatscho/qi.git
cd qi
make
```

### Linux

Install ncurses using your distribution's package manager.

**Debian / Ubuntu:**
```sh
sudo apt install libncurses-dev
```

**Fedora / RHEL:**
```sh
sudo dnf install ncurses-devel
```

**Arch Linux:**
```sh
sudo pacman -S ncurses
```

Then build:

```sh
git clone https://github.com/quatscho/qi.git
cd qi
make
```

### Installing the binary

Copy the resulting `qi` binary to a directory on your `PATH`:

```sh
sudo cp qi /usr/local/bin/
```

## Usage

```sh
qi filename
```

If the file does not exist it will be created when you first save.

## License

As of version 1.1.9, qi is licensed under the GPL, version 3.
Versions of qi prior to 1.1.9 are proprietary code and may not be copied, modified, or distributed. In cases where older code is unchanged in the 1.1.9 or later versions, that code is covered by the GPL only when it is included in the appropriately-licensed version of the software.

Copyright (C) 2026 Christopher Camacho
