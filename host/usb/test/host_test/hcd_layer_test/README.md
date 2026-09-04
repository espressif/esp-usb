# Description

This directory contains test code for `HCD layer` of USB Host stack. Namely:

- HCD public API calls to initialize and deinitialize the HCD port with partially mocked USB Host stack to test Linux build and Cmock run for this partial Mock
- Mocked are all layers of the USB Host stack below the HCD layer, which is used as a real component

Tests are written using [Catch2](https://github.com/catchorg/Catch2) test framework, use CMock, so you must install Ruby on your machine to run them.

This test directory uses freertos as a real component

# Build

Tests build regularly like an idf project. Currently only working on Linux machines.

```
idf.py --preview set-target linux
idf.py build
```

# Run

The build produces an executable in the build folder.

Just run:

```
idf.py monitor
```

or run the executable directly:

```
./build/host_test_hcd_layer.elf
```
