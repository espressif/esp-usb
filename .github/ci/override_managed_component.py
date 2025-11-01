#!/usr/bin/env python
#
# SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import sys
import argparse
import yaml
from pathlib import Path
from glob import glob
from idf_component_tools.manager import ManifestManager

def find_apps_with_manifest(apps):
    """
    Given a list of paths or glob patterns, return a list of directories
    that contain 'main/idf_component.yml'. Checks one level of subfolders.
    """
    apps_with_glob = []

    for app in apps:
        # Expand wildcards
        for match in glob(app):
            p = Path(match)

            if p.is_dir():
                # Check main/idf_component.yml directly inside
                manifest = p / "main" / "idf_component.yml"
                if manifest.exists():
                    apps_with_glob.append(str(p))
                    continue  # already found, no need to check subfolders

                # Check one level of subfolders
                for sub in p.iterdir():
                    if sub.is_dir():
                        manifest = sub / "main" / "idf_component.yml"
                        if manifest.exists():
                            apps_with_glob.append(str(sub))

    return apps_with_glob

def override_with_local_component(component, local_path, app):
    """
    Override managed component in an example at local_path with a local component
    """
    app_path = Path(app)

    absolute_local_path = Path(local_path).absolute()
    if not absolute_local_path.exists():
        raise FileNotFoundError('[Error] {} path does not exist'.format(local_path))
    if not app_path.exists():
        raise FileNotFoundError('[Error] {} path does not exist'.format(app_path))

    print('[Info] Processing app {}'.format(app))
    manager = ManifestManager(app_path / 'main', 'app')
    # Add default namespace if missing
    component_with_namespace = component if "/" in component else f"espressif/{component}"

    try:
        manifest_tree = yaml.safe_load(Path(manager.path).read_text(encoding='utf8'))
        manifest_tree['dependencies'][component_with_namespace] = {
            'version': '*',
            'override_path': str(absolute_local_path)
        }
        with open(manager.path, 'w', encoding='utf8') as f:
            yaml.dump(manifest_tree, f, allow_unicode=True, Dumper=yaml.SafeDumper)
    except KeyError:
        print('[Error] {} app does not depend on {}'.format(app, component_with_namespace))
        raise KeyError

def override_with_local_component_all(component, local_path, apps):
    """
    Process all apps and apply the local component override.
    Returns 0 on success, non-zero on error.
    """
    apps_with_glob = find_apps_with_manifest(apps)
    if not apps_with_glob:
        print("[Error] No apps found containing 'main/idf_component.yml'")
        return -1

    print(f'[Info] Overriding {component} component with local path {local_path}')
    # Go through all collected apps
    for app in apps_with_glob:
        try:
            override_with_local_component(component, local_path, app)
        except Exception as exc:
            print("[Error] Could not process app {}: {}".format(app, exc))
            return -1
    print('[Info] All apps processed successfully')
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description="Override a managed component with a local one for multiple ESP-IDF apps.")
    parser.add_argument('component', help='Existing component that the app depends on')
    parser.add_argument('local_path', help='Path to component that will be used instead of the managed version')
    parser.add_argument('apps', nargs='*', help='List of apps to process')
    args = parser.parse_args()
    sys.exit(override_with_local_component_all(args.component, args.local_path, args.apps))
