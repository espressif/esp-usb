# Changelog for USB Type-C library

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Initial pre-release scope of the USB Type-C (`usb_tcpm`) component.
- Core APIs for install/uninstall, port creation/destruction, and status snapshot.
- Shared worker-task runtime model and Type-C attach/detach event flow.
- FUSB302 backend, example app (`examples/pd_fusb302`), and target test app (`test_apps/pd_fusb302`).
