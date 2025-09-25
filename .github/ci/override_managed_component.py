#!/usr/bin/env python
#
# SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""
Managed components overriding
------------------------------------

The script:
- finds all examples on specified path, based on presence of idf_component.yml inside main folder
- skips CherryUSB example
- creates idf_component.yml for usb_host_lib example, as it does not have any
- override all the components defined in the example's idf_component.yml
  with a local paths to the components present in esp-usb
- it can separately override esp_tinyusb, usb (usb_host_lib) or all class drivers

Usage Examples:
---------------

To override esp_tinyusb component in usb device examples:
override_managed_component.py esp_tinyusb device/esp_tinyusb ${IDF_PATH}/examples/peripherals/usb/device/*

To override usb (usb_host_lib) component in usb host examples:
override_managed_component.py usb host/usb ${IDF_PATH}/examples/peripherals/usb/host/*

To override all class drivers in usb host examples:
override_managed_component.py class host/class ${IDF_PATH}/examples/peripherals/usb/host/*
"""

import sys
import argparse
import yaml
from pathlib import Path
from glob import glob
from idf_component_tools.manager import ManifestManager


def _has_manifest(path: Path) -> bool:
    """Check if a given directory contains main/idf_component.yml"""
    return (path / "main" / "idf_component.yml").exists()

def _create_usb_host_lib_manifest(path: Path) -> None:
    """Ensure usb_host_lib has a manifest file"""
    main_dir = path / "main"
    main_dir.mkdir(parents=True, exist_ok=True)
    manifest_file = main_dir / "idf_component.yml"
    if not manifest_file.exists():
        try:
            print(f"[Info] Creating idf_component.yml for usb_host_lib: {manifest_file}")
            with open(manifest_file, "w", encoding="utf8") as f:
                f.write("dependencies: {}\n")
        except (OSError, PermissionError) as e:
            print(f"[Error] Failed to create manifest {manifest_file}: {e}")
            raise

def find_apps_with_manifest(component, apps):
    """
    Given a list of paths or glob patterns, return a list of directories
    that contain 'main/idf_component.yml'. Checks one level of subfolders.
    Excludes any path that contains 'CherryUSB'.
    """
    apps_with_glob = []

    for app in apps:
        # Expand wildcards
        for match in glob(app):
            if 'cherryusb' in match:
                continue  # Skip CHerryUSB examples, path containing 'cherryusb'

            p = Path(match)
            if not p.is_dir():
                continue

            # Check main/idf_component.yml directly inside
            if _has_manifest(p):
                apps_with_glob.append(str(p))
                continue  # already found, no need to check subfolders

            # Check one level of subfolders
            for sub in p.iterdir():
                if sub.is_dir() and _has_manifest(sub):
                    apps_with_glob.append(str(sub))

    # Special handling: create empty manifest for usb_host_lib if missing
    if component == "usb":
        for app in apps:
            for match in glob(app):
                p = Path(match)
                if p.is_dir() and p.name == "usb_host_lib":
                    _create_usb_host_lib_manifest(p)
                    apps_with_glob.append(str(p))

    return apps_with_glob

def _get_class_name(app_path: Path) -> str:
    """Determine class name from example path"""
    known_classes = {"cdc", "hid", "msc", "uvc"}
    parts = app_path.parts

    if parts and parts[-1] in known_classes:
        return app_path.parts[-1]
    elif len(parts) >= 2 and parts[-2] in known_classes:
        return app_path.parts[-2]
    else:
        raise ValueError(f"Could not determine class name from path: {app_path}")


def override_with_local_component(component, local_path, app):
    """Override managed component with local component"""
    app_path = Path(app)
    absolute_local_path = Path(local_path).absolute()

    if not absolute_local_path.exists():
        raise FileNotFoundError(f'[Error] {local_path} path does not exist')
    if not app_path.exists():
        raise FileNotFoundError(f'[Error] {app_path} path does not exist')

    print(f'[Info] Processing app {app}')
    manager = ManifestManager(app_path / 'main', 'app')

    # Normalize main component name
    component_with_namespace = component if '/' in component else f'espressif/{component}'

    try:
        manifest_tree = yaml.safe_load(Path(manager.path).read_text(encoding='utf8'))
    except yaml.YAMLError as e:
        raise FileNotFoundError(f"[Error] Failed to parse manifest {manager.path}: {e}") from e
    deps = manifest_tree.get('dependencies', {})

    # --- Override esp_tinyusb component in usb device examples ---
    if component == "esp_tinyusb":
        deps[component_with_namespace] = {
            'version': '*',
            'override_path': str(absolute_local_path)
        }
        print(f"[Info] Overridden esp_tinyusb {component_with_namespace} -> {absolute_local_path}")

    # --- Override usb component in usb host examples ---
    if component == "usb":
        deps[component_with_namespace] = {
            'version': '*',
            'override_path': str(absolute_local_path)
        }
        print(f"[Info] Overridden usb {component_with_namespace} -> {absolute_local_path}")

    # --- Override all class drivers components in usb host examples ---
    if component == "class":
        # Determine class name from example path
        class_name = _get_class_name(app_path)

        for usb_host_dep in [dep for dep in deps if dep.split("/")[-1].startswith("usb_host_")]:
            short_name = usb_host_dep.split("/")[-1]
            local_subpath = absolute_local_path.parent / "class" / class_name / short_name
            if not local_subpath.exists():
                raise FileNotFoundError(f"[Error] Local path for {usb_host_dep} not found at {local_subpath}")

            usb_host_dep_with_ns = usb_host_dep if '/' in usb_host_dep else f'espressif/{usb_host_dep}'

            # Remove old key if it’s the non-namespaced one
            if usb_host_dep_with_ns != usb_host_dep and usb_host_dep in deps:
                deps.pop(usb_host_dep)

            deps[usb_host_dep_with_ns] = {
                'version': '*',
                'override_path': str(local_subpath)
            }
            print(f"[Info] Overridden class {usb_host_dep} -> {local_subpath}")

        # Special override for cdc_acm_vcp examples
        if app_path.name == "cdc_acm_vcp":
            extra_dep = "usb_host_cdc_acm"
            extra_path = absolute_local_path.parent / "class" / class_name / extra_dep
            if not extra_path.exists():
                raise FileNotFoundError(f"[Error] Local path for {extra_dep} not found at {extra_path}")

            extra_dep_with_ns = f'espressif/{extra_dep}'
            deps[extra_dep_with_ns] = {
                'version': '*',
                'override_path': str(extra_path)
            }
            print(f"[Info] Overridden extra component {extra_dep} -> {extra_path}")

    manifest_tree['dependencies'] = deps

    try:
        with open(manager.path, 'w', encoding='utf8') as f:
            yaml.dump(manifest_tree, f, allow_unicode=True, Dumper=yaml.SafeDumper)
    except (OSError, PermissionError) as e:
        print(f"[Error] Failed to write manifest {manager.path}: {e}")
        raise


def override_with_local_component_all(component, local_path, apps):
    """Find apps and override components"""

    # Process wildcard, e.g. "app_prefix_*"
    apps_with_glob = find_apps_with_manifest(component, apps)

    print("[Info] Apps found:")
    for app in apps_with_glob:
        print(f"[Info] {app}")

    # Go through all collected apps
    for app in apps_with_glob:
        try:
            override_with_local_component(component, local_path, app)
        except Exception as e:
            print(f"[Error] Could not process app {app}: {e}")
            return -1

    print("[Info] Overriding complete")
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('component', help='Existing component that the app depends on')
    parser.add_argument('local_path', help='Path to component that will be used instead of the managed version')
    parser.add_argument('apps', nargs='*', help='List of apps to process')
    args = parser.parse_args()
    sys.exit(override_with_local_component_all(args.component, args.local_path, args.apps))
