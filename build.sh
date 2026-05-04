#!/bin/bash
set -e

# ============================================================
# Build the Vulkan triangle test NRO for Nintendo Switch.
# Uses the packaged NVK Vulkan image/prefix.
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_NAME="build"
NRO_NAME="build/TriangleTest.nro"
LOCAL_BUILD="$SCRIPT_DIR/$BUILD_NAME"
NRO_OUT="$SCRIPT_DIR/$NRO_NAME"
USE_DOCKER="${TriangleTest_DOCKER:-1}"
DOCKER_IMAGE="${TriangleTest_DOCKER_IMAGE:-ticohq/switch-nvk-vulkan}"

run() {
    if [ "$USE_DOCKER" = "1" ]; then
        docker run --rm \
            -v "$SCRIPT_DIR:$PROJECT_PREFIX" \
            --workdir "$PROJECT_PREFIX" \
            "$DOCKER_IMAGE" bash -lc "$1"
    else
        bash -c "$1"
    fi
}

if [ "$USE_DOCKER" = "1" ]; then
    PROJECT_PREFIX="${TriangleTest_PROJECT_PREFIX:-/work}"
    NVK_PREFIX="${TriangleTest_NVK_PREFIX:-/opt/nvk-switch}"
else
    PROJECT_PREFIX="$SCRIPT_DIR"
    NVK_PREFIX="${TriangleTest_NVK_PREFIX:-/opt/nvk-switch}"
fi

echo "=== Building Vulkan Triangle NRO ==="

run "rm -rf '$PROJECT_PREFIX/$BUILD_NAME' && mkdir -p '$PROJECT_PREFIX/$BUILD_NAME'"

# DevkitPro paths
DKP=/opt/devkitpro
DKA=$DKP/devkitA64
LIBNX=$DKP/libnx
PORTLIBS=$DKP/portlibs/switch

CXX=$DKA/bin/aarch64-none-elf-g++

# Compile flags
CXXFLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
CXXFLAGS+=" -O2 -std=c++17 -ffunction-sections -fdata-sections"
CXXFLAGS+=" -I$LIBNX/include"
CXXFLAGS+=" -I$PORTLIBS/include"
CXXFLAGS+=" -I$PROJECT_PREFIX/$BUILD_NAME"
PKG_CONFIG_ENV="export PKG_CONFIG_PATH='$NVK_PREFIX/lib/pkgconfig:$PORTLIBS/lib/pkgconfig':\${PKG_CONFIG_PATH:-}"

# Compile shaders (runs on HOST, not in Docker)
echo "Compiling shaders..."
GLSLANG=$(command -v glslangValidator 2>/dev/null || echo "")
GLSLC_BIN=$(command -v glslc 2>/dev/null || echo "")
mkdir -p "$LOCAL_BUILD"

if [ -n "$GLSLANG" ]; then
    "$GLSLANG" -V "$SCRIPT_DIR/triangle.vert" -o "$LOCAL_BUILD/triangle.vert.spv"
    "$GLSLANG" -V "$SCRIPT_DIR/triangle.frag" -o "$LOCAL_BUILD/triangle.frag.spv"
elif [ -n "$GLSLC_BIN" ]; then
    "$GLSLC_BIN" "$SCRIPT_DIR/triangle.vert" -o "$LOCAL_BUILD/triangle.vert.spv"
    "$GLSLC_BIN" "$SCRIPT_DIR/triangle.frag" -o "$LOCAL_BUILD/triangle.frag.spv"
else
    echo "ERROR: No GLSL compiler found (need glslangValidator or glslc)"
    echo "  brew install glslang   or   brew install shaderc"
    exit 1
fi
# Generate C include headers from SPIR-V binary
xxd -i < "$LOCAL_BUILD/triangle.vert.spv" > "$LOCAL_BUILD/triangle_vert.h"
xxd -i < "$LOCAL_BUILD/triangle.frag.spv" > "$LOCAL_BUILD/triangle_frag.h"
echo "Shaders compiled OK"

echo "Compiling TriangleTest.cpp..."
run "
$PKG_CONFIG_ENV
$CXX $CXXFLAGS \$(pkg-config --cflags nvk-switch-vulkan) \
    -c '$PROJECT_PREFIX/TriangleTest.cpp' \
    -o '$PROJECT_PREFIX/$BUILD_NAME/TriangleTest.o'
"

# Link order
LDFLAGS="-specs=$LIBNX/switch.specs"
LDFLAGS+=" -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
LDFLAGS+=" -L$DKA/lib -L$LIBNX/lib -L$PORTLIBS/lib"
LDFLAGS+=" -Wl,--gc-sections"

echo "Linking..."
run "
$PKG_CONFIG_ENV
$CXX $LDFLAGS \
    '$PROJECT_PREFIX/$BUILD_NAME/TriangleTest.o' \
    \$(pkg-config --static --libs nvk-switch-vulkan) \
    -o '$PROJECT_PREFIX/$BUILD_NAME/TriangleTest.elf'
"

# Generate NRO
echo "Generating NRO..."
run "
nacptool --create 'NVK Triangle' 'NVK Examples' '0.0.1' '$PROJECT_PREFIX/$BUILD_NAME/TriangleTest.nacp'
elf2nro '$PROJECT_PREFIX/$BUILD_NAME/TriangleTest.elf' '$PROJECT_PREFIX/$NRO_NAME' --nacp='$PROJECT_PREFIX/$BUILD_NAME/TriangleTest.nacp'
"

if [ -f "$NRO_OUT" ]; then
    SIZE=$(stat -c%s "$NRO_OUT" 2>/dev/null || stat -f%z "$NRO_OUT" 2>/dev/null)
    echo "SUCCESS: $NRO_OUT ($SIZE bytes)"
else
    echo "FAILED: NRO not produced"
    exit 1
fi
