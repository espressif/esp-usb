# Description

This directory contains test code for `USB Host MSC` driver. Namely:

- Simple public API call with mocked USB component to test Linux build and Cmock run for this class driver
- Legacy installation of LUN 0 without GET_MAX_LUN, and explicit installation of a configured LUN without probing other slots or falling back
- Complete LUN discovery followed by application selection: accepting a unique candidate, choosing among multiple candidates, or explicitly accepting the lowest-numbered ready candidate
- Selected-LUN preservation during I/O and reset recovery; GET_MAX_LUN STALL, malformed responses, parameter validation, and rejection of competing probe/install operations
- Ready and failed LUN masks, empty slots, supported sector sizes, one-pass and shared-window retries, rejection of partial results after transport errors, and temporary-session cleanup
- BOT and endpoint synchronization across LUN changes and failed sessions; cleanup after transport initialization failures and pending USB callback bookkeeping

Tests are written using [Catch2](https://github.com/catchorg/Catch2) test framework, use CMock, so you must install Ruby on your machine to run them.

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
./build/host_test_usb_msc.elf
```
