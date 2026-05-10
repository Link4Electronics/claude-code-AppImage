#!/bin/sh

set -eu

ARCH=$(uname -m)
VERSION=$(pacman -Q claude-code | awk '{print $2; exit}') # example command to get version of application here
export ARCH VERSION
export OUTPATH=./dist
export ADD_HOOKS="self-updater.hook"
export UPINFO="gh-releases-zsync|${GITHUB_REPOSITORY%/*}|${GITHUB_REPOSITORY#*/}|latest|*$ARCH.AppImage.zsync"
export ICON=https://github.com/user-attachments/assets/0de7bd75-fd58-44f0-ba5f-74bad7261a3b
export DESKTOP=DUMMY
export MAIN_BIN=claude

# Deploy dependencies
quick-sharun /opt/claude-code/bin/claude
echo 'DISABLE_AUTOUPDATER=1' >> ./AppDir/.env

# Additional changes can be done in between here

# Turn AppDir into AppImage
quick-sharun --make-appimage

# Test the app for 12 seconds, if the test fails due to the app
# having issues running in the CI use --simple-test instead
quick-sharun --test ./dist/*.AppImage
