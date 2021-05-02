#!/usr/bin/env python3

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see https://www.gnu.org/licenses/.

import argparse
import json
import os
import re
import requests
import shutil
import subprocess
import urllib.parse
from wand.image import Image

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description = 'Bundle Goupile instance as EXE for offline use')
    parser.add_argument('url', type = str, help = 'URL of instance')
    parser.add_argument('-O', '--output_dir', dest = 'output_dir', action = 'store', help = 'Output directory')
    parser.add_argument('--shortcut', dest = 'shortcut', action = 'store', help = 'Shortcut name')
    args = parser.parse_args()

    # Find repository directory
    root_directory = os.path.dirname(os.path.realpath(__file__))
    while not os.path.exists(root_directory + '/FelixBuild.ini'):
        new_directory = os.path.realpath(root_directory + '/..')
        if new_directory == root_directory:
            raise ValueError('Could not find FelixBuild.ini')
        root_directory = new_directory

    # Download manifest information
    url = args.url.rstrip('/') + '/manifest.json'
    response = requests.get(url)
    if response.status_code != 200:
        raise ValueError('Failed to download manifest.json from this instance')
    manifest = json.loads(response.content)

    # URL safe name
    safe_name = re.sub('[^a-zA-Z0-9]', '_', manifest["name"])

    # Prepare build directory
    if args.output_dir is None:
        build_directory = os.path.join(root_directory, 'bin/GoupilePortable', safe_name)
    else:
        build_directory = args.output_dir
    os.makedirs(build_directory + '/build', exist_ok = True)
    shutil.copytree('app', build_directory, dirs_exist_ok = True)

    # Update settings
    update_version = subprocess.check_output('git log -n1 --pretty=format:%cd --date=format:%Y.%m.%d', shell = True)
    update_version = update_version.decode()
    update_version = update_version.replace('.0', '.')
    update_url = f'https://goupile.fr/files/{safe_name.lower()}/'
    shortcut_name = args.shortcut or manifest["name"]

    # Update package.json
    with open(build_directory + '/package.json', 'r') as f:
        package = json.load(f)
    package["name"] = safe_name
    package["homepage"] = args.url
    package["version"] = update_version
    package["build"]["publish"][0]["url"] = update_url
    with open(build_directory + '/package.json', 'w') as f:
        json.dump(package, f, indent = 4, ensure_ascii = False)

    # Prepare PNG and icon
    url = args.url.rstrip('/') + '/favicon.png'
    response = requests.get(url)
    if response.status_code != 200:
        raise ValueError('Failed to download favicon.png from this instance')
    with open(build_directory + '/build/icon.png', 'wb') as f:
        f.write(response.content)
    with Image() as ico:
        with Image(filename = build_directory + '/build/icon.png') as img:
            for size in [16, 24, 32, 48, 64, 96, 128, 256]:
                with img.clone() as it:
                    it.resize(size, size)
                    ico.sequence.append(it)
        ico.save(filename = build_directory + '/build/icon.ico')

    # Customize installation path
    with open(build_directory + '/build/installer.nsh', 'w') as f:
        root_dir = f'$LOCALAPPDATA\\GoupilePortable\\{safe_name}'

        nsh = f'''
            !macro preInit
                SetRegView 32
                WriteRegExpandStr HKLM "${{INSTALL_REGISTRY_KEY}}" InstallLocation "{root_dir}\\app"
                WriteRegExpandStr HKCU "${{INSTALL_REGISTRY_KEY}}" InstallLocation "{root_dir}\\app"
            !macroend

            !macro customInstall
                CreateShortCut "$DESKTOP\\{shortcut_name}.lnk" "{root_dir}\\app\\{manifest["name"]}.exe" --user-data-dir="{root_dir}\\profiles"
                CreateShortCut "$STARTMENU\\{shortcut_name}.lnk" "{root_dir}\\app\\{manifest["name"]}.exe" --user-data-dir="{root_dir}\\profiles"
            !macroend

            !macro customUnInstall
                Delete "$DESKTOP\\{shortcut_name}.lnk"
                Delete "$STARTMENU\\{shortcut_name}.lnk"
            !macroend
        '''
        f.write(nsh)

    # Run electron-builder
    subprocess.run('npm install', shell = True, cwd = build_directory)
    subprocess.run('npm run dist', shell = True, cwd = build_directory)
