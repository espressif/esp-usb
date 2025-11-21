# ESP-USB Documentation

This folder contains the ESP-Docs scaffolding for ESP-USB documentation.

## Structure

- `conf_common.py` - Common configuration for all languages
- `en/` - English documentation source files
  - `conf.py` - English-specific configuration
  - `index.rst` - Main documentation index
- `_static/` - Static files (CSS, JS, images)
  - `docs_version.js` - Target and version selector configuration
- `Doxyfile` - Doxygen configuration file
- `requirements.txt` - Python dependencies for building documentation
- `build_docs.sh` - Build script for documentation

## Building Documentation

### Prerequisites

1. **Python 3.7 or newer**
2. **Doxygen** - Install via system package manager:
   - Ubuntu/Debian: `sudo apt-get install doxygen graphviz`
   - macOS: `brew install doxygen graphviz`
   - Windows: Download from [Doxygen website](https://www.doxygen.nl/download.html)

### Build Steps

To build the documentation locally:

```bash
cd docs
pip install -r requirements.txt
./build_docs.sh
```

The built documentation will be in `docs/_build/`.

## Adding Documentation Content

1. Add reStructuredText (`.rst`) files to `docs/en/`
2. Update `docs/en/index.rst` to include new pages in the table of contents
3. For API documentation, ensure header files are included in `Doxyfile` INPUT paths

## Existing Documentation

The following folders contain existing markdown documentation that can be migrated to reStructuredText format:

- `device/` - Device-related documentation
- `host/` - Host-related documentation
