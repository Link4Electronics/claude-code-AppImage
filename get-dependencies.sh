#!/bin/sh

set -eu

ARCH=$(uname -m)

echo "Installing package dependencies..."
echo "---------------------------------------------------------------"
# pacman -Syu --noconfirm PACKAGESHERE

echo "Installing debloated packages..."
echo "---------------------------------------------------------------"
get-debloated-pkgs --add-common --prefer-nano

# If the application needs to be manually built that has to be done down here
echo "Downloading claude-code binary..."
echo "---------------------------------------------------------------"
case "$ARCH" in
	x86_64)  farch=x64;;
	aarch64) farch=arm64;;
esac
VERSION=$(wget https://registry.npmjs.org/@anthropic-ai/claude-code/latest -O - | jq -r '.version')
link="https://downloads.claude.ai/claude-code-releases/$VERSION/linux-$farch/claude"

mkdir -p ./AppDir/bin
wget --retry-connrefused --tries=30 "$link" -O ./AppDir/bin/claude
chmod +x ./AppDir/bin/claude

echo "$VERSION" > ~/version

