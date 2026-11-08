#!/bin/bash
# Build script with automatic version management and changelog prompts
# for vdr-plugin-mcp (fork of unf/vdr-plugin-mcp), adapted from the one
# of vdr-plugin-epg2vdr.
#
# Run it on the branch ubuntu/noble (Ubuntu 24.04). It is upstream's Debian
# trixie packaging (branch debian/trixie) with a noble changelog entry and
# vdr-dev (>= 2.8.1~) for the yavdr
# VDR 2.8.1 of this host (upstream asks for 2.8.4, without a documented
# reason; running on 2.8.1 is not verified).
#
# Produces:
#   vdr-plugin-mcp          (the plugin, amd64)
#   vdr-plugin-mcp-dbgsym   (debug symbols, .ddeb, created by dh_strip)
#
# Version scheme: ${VERSION from mcp.cpp}-1fnu0~noble+local-N
#
# "1fnu0" is upstream's Debian revision, "~noble+local-N" marks the local
# build. ~noble sorts below upstream's ~trixie entries of the same version,
# hence dch --allow-lower-version.
set -e
set -o pipefail

PROJECT_NAME="vdr-plugin-mcp"
SOURCE_DIR="/usr/local/src/user/vdr-plugin-mcp"
BUILD_BASE_DIR="/usr/local/src/user"
DISTRO="noble"

PACKAGES=(
    "vdr-plugin-mcp:deb"
    "vdr-plugin-mcp-dbgsym:ddeb"
)

cd "$SOURCE_DIR"

if [ ! -f debian/control ]; then
    echo "ERROR: no debian/ directory, check out the branch ubuntu/noble"
    exit 1
fi

# Extract version from mcp.cpp (static const char *VERSION = "0.4.2";),
# the same way the Makefile does
BASE_VERSION=$(grep 'static const char \*VERSION' mcp.cpp | awk '{ print $6 }' | sed -e 's/[";]//g')
GIT_HASH=$(git rev-parse --short HEAD)
GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD)

if [ -z "$BASE_VERSION" ]; then
    echo "ERROR: VERSION not found in mcp.cpp"
    exit 1
fi

echo "==> Building version: ${BASE_VERSION}"
echo "==> Git branch: $GIT_BRANCH (commit $GIT_HASH)"
if [ -n "$(git status --porcelain --untracked-files=no -- . ':!debian/changelog')" ]; then
    echo "==> WARNING: uncommitted changes, they go into the package:"
    git status --short --untracked-files=no -- . ':!debian/changelog'
fi
echo ""

read -p "Enter changelog description: " CHANGELOG_MSG

if [ -z "$CHANGELOG_MSG" ]; then
    echo "ERROR: Changelog description cannot be empty"
    exit 1
fi

DEBIAN_REVISION="1fnu0~${DISTRO}"
BUILD_DIR="${BUILD_BASE_DIR}/${PROJECT_NAME}-${BASE_VERSION}-${DEBIAN_REVISION}"

# Auto-increment +local-N if we already built this exact version
LAST_VERSION=$(head -1 debian/changelog | sed -n 's/.*(\([^)]*\)).*/\1/p')
BASE_VERSION_ESCAPED=$(echo "$BASE_VERSION" | sed 's/[+.]/\\&/g')
DEBIAN_REVISION_ESCAPED=$(echo "$DEBIAN_REVISION" | sed 's/[~]/\\&/g')
if [[ "$LAST_VERSION" =~ ^${BASE_VERSION_ESCAPED}-${DEBIAN_REVISION_ESCAPED}\+local-([0-9]+)$ ]]; then
    LOCAL_NUM=$((${BASH_REMATCH[1]} + 1))
else
    LOCAL_NUM=1
fi

PACKAGE_VERSION="${BASE_VERSION}-${DEBIAN_REVISION}+local-${LOCAL_NUM}"

echo "==> Creating changelog entry for version: $PACKAGE_VERSION"

DEBFULLNAME="VDR Builder" DEBEMAIL="builder@localhost" \
    dch --newversion "$PACKAGE_VERSION" --distribution "$DISTRO" \
    --allow-lower-version "~${DISTRO}\+local-[0-9]+$" \
    "$CHANGELOG_MSG"

# Clean up build artifacts
make clean 2>/dev/null || true

[ -d "$BUILD_DIR" ] && rm -rf "$BUILD_DIR"

# Copy source including updated debian/ directory from source tree
rsync -a \
    --exclude='.git' \
    --exclude='*.o' \
    --exclude='*.so' \
    --exclude='.dependencies' \
    --exclude='.claude' \
    --exclude='CLAUDE.md' \
    --exclude='build-package.sh' \
    "$SOURCE_DIR/" "$BUILD_DIR/"

cd "$BUILD_DIR"

# Full output (compiler warnings included) goes to the terminal and a log
BUILD_LOG="${BUILD_BASE_DIR}/${PROJECT_NAME}_${PACKAGE_VERSION}_build.log"
echo "==> Package version: $PACKAGE_VERSION"
echo "==> Building packages, log: $BUILD_LOG"
dpkg-buildpackage -b -uc -us -j$(nproc) 2>&1 | tee "$BUILD_LOG"

echo ""
echo "==> Build complete. Checking generated packages:"
echo ""

SUCCESS_COUNT=0
FAIL_COUNT=0

for ENTRY in "${PACKAGES[@]}"; do
    PKG="${ENTRY%%:*}"
    EXT="${ENTRY##*:}"
    DEB_FILE="${BUILD_BASE_DIR}/${PKG}_${PACKAGE_VERSION}_amd64.${EXT}"

    if [ -f "$DEB_FILE" ]; then
        SIZE=$(ls -lh "$DEB_FILE" | awk '{print $5}')
        echo "  [OK] $PKG ($SIZE)"
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
    else
        echo "  [FAIL] $PKG - not found"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo ""
echo "==> Summary: $SUCCESS_COUNT packages built successfully, $FAIL_COUNT failed"

if [ $FAIL_COUNT -eq 0 ]; then
    echo ""
    echo "==> All packages in: $BUILD_BASE_DIR/"
    ls -lh "${BUILD_BASE_DIR}"/*_${PACKAGE_VERSION}_*.*deb 2>/dev/null
else
    echo ""
    echo "==> ERROR: Some packages failed to build"
    exit 1
fi
