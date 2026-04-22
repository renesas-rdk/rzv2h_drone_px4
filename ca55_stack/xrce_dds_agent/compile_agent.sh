#!/bin/bash
# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Build script for Custom XRCE Agent
# Builds the actual custom_agent.cpp source to CustomXRCEAgent binary

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_DIR="$SCRIPT_DIR"
BUILD_DIR="$AGENT_DIR/src/build"
MICRO_XRCE_BUILD_DIR="$AGENT_DIR/Micro-XRCE-DDS-Agent/build"
MICRO_XRCE_TEMP_INSTALL_DIR="$MICRO_XRCE_BUILD_DIR/temp_install"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

# Deployment defaults
RZV_TARGET_HOST="${RZV_TARGET_HOST:-${RDK_IP:-192.168.1.150}}"
RZV_TARGET_USER="${RZV_TARGET_USER:-root}"
RZV_TARGET_BIN_DIR="${RZV_TARGET_BIN_DIR:-/usr/bin}"
RZV_TARGET_LIB_DIR="${RZV_TARGET_LIB_DIR:-/lib/aarch64-linux-gnu}"
RZV_AGENT_AUTO_DEPLOY="${RZV_AGENT_AUTO_DEPLOY:-OFF}"
RZV_SSH_OPTIONS="${RZV_SSH_OPTIONS:--o StrictHostKeyChecking=no}"

# Helpers for deployment
target_identifier() {
    echo "${RZV_TARGET_USER}@${RZV_TARGET_HOST}"
}

agent_binary_candidate() {
    if [ -f "$BUILD_DIR/CustomXRCEAgent" ]; then
        echo "$BUILD_DIR/CustomXRCEAgent"
    else
        return 1
    fi
}

ensure_target_config() {
    if [ -z "$RZV_TARGET_HOST" ] || [ -z "$RZV_TARGET_USER" ]; then
        log_error "Set RZV_TARGET_USER and RZV_TARGET_HOST to point at the CA55 board."
        return 1
    fi
    if [ -z "$RZV_TARGET_BIN_DIR" ] || [ -z "$RZV_TARGET_LIB_DIR" ]; then
        log_error "Set RZV_TARGET_BIN_DIR and RZV_TARGET_LIB_DIR to valid directories on the board."
        return 1
    fi
    return 0
}

copy_file_to_target() {
    local src="$1"
    local dest_dir="$2"
    if [ ! -f "$src" ]; then
        log_error "Source file missing: $src"
        return 1
    fi
    ensure_target_config || return 1
    dest_dir="${dest_dir:-$RZV_TARGET_BIN_DIR}"
    log_info "Copying $(basename "$src") to $(target_identifier):${dest_dir}"
    scp $RZV_SSH_OPTIONS "$src" "${RZV_TARGET_USER}@${RZV_TARGET_HOST}:${dest_dir}/"
}

copy_agent_to_target() {
    local binary
    binary="$(agent_binary_candidate)" || {
        log_error "Binary not found. Run './compile.sh build' first."
        return 1
    }
    ensure_target_config || return 1

    local dest="${RZV_TARGET_BIN_DIR}/CustomXRCEAgent"
    local temp="${dest}.new"
    local target_id
    target_id="$(target_identifier)"

    # SCP to a temp name so we never open-for-write the executing binary
    # ("Text file busy" happens when scp tries to truncate the running file).
    log_info "Uploading to ${target_id}:${temp} ..."
    scp $RZV_SSH_OPTIONS "$binary" "${RZV_TARGET_USER}@${RZV_TARGET_HOST}:${temp}" || {
        log_error "scp failed"
        return 1
    }

    # Atomic rename on the target — rename(2) never opens the file for writing,
    # so the running process keeps using the old inode while the new binary is
    # now in place for the next exec().
    log_info "Replacing binary atomically on target..."
    ssh $RZV_SSH_OPTIONS "${RZV_TARGET_USER}@${RZV_TARGET_HOST}" \
        "chmod +x '${temp}' && mv '${temp}' '${dest}'" || {
        log_error "Atomic move failed; ${temp} left on target"
        return 1
    }
    log_info "✓ Deployed to ${target_id}:${dest}"

    # Restart the agent so it picks up the new binary.
    # Tries systemctl first; falls back to pkill (service watchdog will restart it).
    local service="${RZV_AGENT_SERVICE_NAME:-custom_xrce_agent}"
    log_info "Restarting agent (service: ${service})..."
    ssh $RZV_SSH_OPTIONS "${RZV_TARGET_USER}@${RZV_TARGET_HOST}" "
        if systemctl is-active --quiet '${service}' 2>/dev/null; then
            systemctl restart '${service}' && echo '[OK] systemctl restart ${service}'
        else
            pkill -f 'CustomXRCEAgent' 2>/dev/null && echo '[OK] pkill sent; watchdog will restart' || echo '[INFO] no running instance found'
        fi
    " || true
}

copy_dependencies() {
    ensure_target_config || return 1
    if [ -z "$SDKTARGETSYSROOT" ]; then
        log_error "Poky environment not configured. Set POKY_ENVIRONMENT_SETUP and run 'build' or 'copy-deps' after sourcing."
        return 1
    fi
    local libs=()
    for lib in "$MICRO_XRCE_BUILD_DIR"/libmicroxrcedds_agent.*; do
        [ -e "$lib" ] && libs+=("$lib")
    done
    for lib in "$MICRO_XRCE_TEMP_INSTALL_DIR"/fastdds-3.1/lib/libfastdds*; do
        [ -e "$lib" ] && libs+=("$lib")
    done
    for lib in "$MICRO_XRCE_TEMP_INSTALL_DIR"/fastcdr-2.2.4/lib/libfastcdr*; do
        [ -e "$lib" ] && libs+=("$lib")
    done
    for lib in "$SDKTARGETSYSROOT"/usr/lib64/libssl.so.1.1 "$SDKTARGETSYSROOT"/usr/lib64/libcrypto.so.1.1; do
        [ -e "$lib" ] && libs+=("$lib")
    done
    if [ ${#libs[@]} -eq 0 ]; then
        log_warn "No dependency libraries found; build Micro-XRCE-DDS-Agent and fastdds/fastcdr first."
        return 1
    fi
    for lib in "${libs[@]}"; do
        copy_file_to_target "$lib" "$RZV_TARGET_LIB_DIR"
    done
}
# Setup Poky environment
setup_poky_environment() {
    log_step "Setting up Poky cross-compilation environment..."
    # If not provided, default to the commonly used Poky env script path.
    if [ -z "${POKY_ENVIRONMENT_SETUP:-}" ]; then
        POKY_ENVIRONMENT_SETUP="/opt/toolchains/poky/3.1.31/environment-setup-aarch64-poky-linux"
        log_warn "POKY_ENVIRONMENT_SETUP not set; defaulting to $POKY_ENVIRONMENT_SETUP"
    fi

    if [ ! -f "$POKY_ENVIRONMENT_SETUP" ]; then
        log_error "✗ Poky environment script not found at $POKY_ENVIRONMENT_SETUP"
        return 1
    fi
    log_info "✓ Poky environment script found"

    # Temporarily clear LD_LIBRARY_PATH while sourcing the Poky env to avoid
    # SDK warnings or loader conflicts. Restore it afterwards.
    if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        OLD_LD_LIBRARY_PATH="$LD_LIBRARY_PATH"
        unset LD_LIBRARY_PATH
        _POKY_UNSET_LD=1
    fi

    # shellcheck disable=SC1090
    source "$POKY_ENVIRONMENT_SETUP"
    log_info "✓ Poky environment configured"

    if [ "${_POKY_UNSET_LD:-}" = 1 ]; then
        export LD_LIBRARY_PATH="$OLD_LD_LIBRARY_PATH"
        unset OLD_LD_LIBRARY_PATH _POKY_UNSET_LD
        log_info "Restored previous LD_LIBRARY_PATH"
    fi
}

# Check and initialize submodule
check_submodule() {
    log_step "Checking Micro-XRCE-DDS-Agent submodule..."
    
    if [ ! -f "$AGENT_DIR/Micro-XRCE-DDS-Agent/CMakeLists.txt" ]; then
        log_warn "Submodule not initialized, initializing now..."
        cd "$AGENT_DIR"
        git submodule update --init --recursive
        if [ $? -ne 0 ]; then
            log_error "Failed to initialize submodule"
            return 1
        fi
    fi
    
    log_info "✓ Submodule ready"
}

# Pre-build fastcdr 2.2.4 with system cmake to avoid Poky cmake 3.16.5 version check failure.
# fastcdr CMakeLists.txt requires cmake >= 3.20; Poky SDK only ships 3.16.5.
# We build fastcdr separately with system cmake and install it into temp_install so that
# the Micro-XRCE-DDS-Agent superbuild's find_package(fastcdr) succeeds and skips
# ExternalProject_Add(fastcdr).
prebuild_fastcdr() {
    local FASTCDR_INSTALL="$MICRO_XRCE_TEMP_INSTALL_DIR/fastcdr-2.2.4"
    local FASTCDR_CMAKE_CONFIG="$FASTCDR_INSTALL/lib/cmake/fastcdr/fastcdr-config.cmake"

    if [ -f "$FASTCDR_CMAKE_CONFIG" ]; then
        log_info "✓ fastcdr already pre-built at $FASTCDR_INSTALL"
        return 0
    fi

    local FASTCDR_SRC="$MICRO_XRCE_BUILD_DIR/fastcdr/src/fastcdr"
    if [ ! -f "$FASTCDR_SRC/CMakeLists.txt" ]; then
        log_warn "fastcdr source not yet downloaded; will be fetched during Micro-XRCE cmake configure"
        return 0
    fi

    log_step "Pre-building fastcdr with system cmake..."
    local CC_COMPILER="/opt/toolchains/poky/3.1.31/sysroots/x86_64-pokysdk-linux/usr/bin/aarch64-poky-linux/aarch64-poky-linux-gcc"
    local CXX_COMPILER="/opt/toolchains/poky/3.1.31/sysroots/x86_64-pokysdk-linux/usr/bin/aarch64-poky-linux/aarch64-poky-linux-g++"
    local SYSROOT="/opt/toolchains/poky/3.1.31/sysroots/aarch64-poky-linux"

    local saved_path="$PATH"
    export PATH="/usr/local/bin:/usr/bin:$PATH"
    local sys_cmake="$(command -v cmake)"

    local fastcdr_build
    fastcdr_build="$(mktemp -d)"
    if [ -n "${LD_LIBRARY_PATH:-}" ]; then OLD_LD="$LD_LIBRARY_PATH"; unset LD_LIBRARY_PATH; _UNSET_LD=1; fi

    "$sys_cmake" "$FASTCDR_SRC" -B "$fastcdr_build" \
        -DCMAKE_C_COMPILER="$CC_COMPILER" \
        -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
        -DCMAKE_SYSROOT="$SYSROOT" \
        -DCMAKE_C_FLAGS="--sysroot=$SYSROOT" \
        -DCMAKE_CXX_FLAGS="--sysroot=$SYSROOT" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX="$FASTCDR_INSTALL"
    local r=$?
    [ "${_UNSET_LD:-}" = 1 ] && { export LD_LIBRARY_PATH="$OLD_LD"; unset OLD_LD _UNSET_LD; }
    export PATH="$saved_path"
    [ $r -ne 0 ] && { log_error "fastcdr cmake configure failed"; rm -rf "$fastcdr_build"; return 1; }

    if [ -n "${LD_LIBRARY_PATH:-}" ]; then OLD_LD="$LD_LIBRARY_PATH"; unset LD_LIBRARY_PATH; _UNSET_LD=1; fi
    make -C "$fastcdr_build" -j$(( $(nproc) / 2 )) && make -C "$fastcdr_build" install
    r=$?
    [ "${_UNSET_LD:-}" = 1 ] && { export LD_LIBRARY_PATH="$OLD_LD"; unset OLD_LD _UNSET_LD; }
    rm -rf "$fastcdr_build"
    [ $r -ne 0 ] && { log_error "fastcdr build/install failed"; return 1; }

    # Patch version file: fastcdr source reports 2.2.7 but superbuild requires find_package(fastcdr 2.2.4 EXACT)
    local ver_file="$FASTCDR_INSTALL/lib/cmake/fastcdr/fastcdr-config-version.cmake"
    if [ -f "$ver_file" ]; then
        sed -i 's/set(PACKAGE_VERSION "2\.2\.[0-9]*")/set(PACKAGE_VERSION "2.2.4")/' "$ver_file"
        sed -i 's/if("2\.2\.[0-9]*" MATCHES/if("2.2.4" MATCHES/' "$ver_file"
        log_info "✓ Patched fastcdr version to 2.2.4 for superbuild compatibility"
    fi

    log_info "✓ fastcdr pre-built and installed to $FASTCDR_INSTALL"
}

clean_fastdds_after_truncated_object_error() {
    log_warn "Detected truncated object while linking fastdds; cleaning fastdds artifacts for a one-time retry"

    # Remove failed fastdds build/install outputs so ExternalProject rebuilds cleanly.
    rm -rf "$MICRO_XRCE_BUILD_DIR/fastdds/src/fastdds-build"
    rm -rf "$MICRO_XRCE_BUILD_DIR/temp_install/fastdds-3.1"

    # Keep downloaded sources, but reset build/install stamps that may otherwise skip rebuild.
    rm -f "$MICRO_XRCE_BUILD_DIR/fastdds/src/fastdds-stamp/fastdds-configure"
    rm -f "$MICRO_XRCE_BUILD_DIR/fastdds/src/fastdds-stamp/fastdds-build"
    rm -f "$MICRO_XRCE_BUILD_DIR/fastdds/src/fastdds-stamp/fastdds-install"
    rm -f "$MICRO_XRCE_BUILD_DIR/fastdds/src/fastdds-stamp/fastdds-done"
}

clean_micro_xrce_after_truncated_object_error() {
    log_warn "Detected truncated object while linking microxrcedds_agent; cleaning local build artifacts for a one-time retry"

    rm -rf "$MICRO_XRCE_BUILD_DIR/CMakeFiles/microxrcedds_agent.dir"
    rm -f "$MICRO_XRCE_BUILD_DIR/libmicroxrcedds_agent.so"*
}

run_micro_xrce_superbuild_make() {
    local log_file="$1"
    local jobs="$2"

    [ -n "$jobs" ] || jobs=1

    if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        OLD_LD="$LD_LIBRARY_PATH"
        unset LD_LIBRARY_PATH
        _UNSET_LD=1
    fi

    export PATH="/usr/local/bin:/usr/bin:$PATH"
    if POKY_ENVIRONMENT_SETUP="$POKY_ENVIRONMENT_SETUP" make -C "$MICRO_XRCE_BUILD_DIR" -j"$jobs" 2>&1 | tee "$log_file"; then
        local build_ret=0
    else
        local build_ret=${PIPESTATUS[0]}
    fi

    [ "${_UNSET_LD:-}" = 1 ] && { export LD_LIBRARY_PATH="$OLD_LD"; unset OLD_LD _UNSET_LD; }

    return "$build_ret"
}

# Build Micro-XRCE-DDS-Agent library (prerequisite for CustomXRCEAgent)
build_micro_xrce_agent_lib() {
    local lib_so="$MICRO_XRCE_BUILD_DIR/libmicroxrcedds_agent.so"

    if [ -f "$lib_so" ]; then
        log_info "✓ Micro-XRCE-DDS-Agent library already built"
        return 0
    fi

    log_step "Building Micro-XRCE-DDS-Agent library (first-time, may take several minutes)..."
    mkdir -p "$MICRO_XRCE_BUILD_DIR"
    cd "$MICRO_XRCE_BUILD_DIR"

    # The Poky SDK overrides PATH with its own cmake 3.16.5, but fastcdr (a superbuild
    # dependency) requires cmake >= 3.20. We pre-build fastcdr with system cmake and
    # pass fastcdr_DIR so find_package succeeds and ExternalProject_Add(fastcdr) is skipped.
    local saved_path="$PATH"
    export PATH="/usr/local/bin:/usr/bin:$PATH"
    local sys_cmake
    sys_cmake="$(command -v cmake)"
    log_info "Using cmake: $sys_cmake ($(cmake --version | head -1))"

    # First configure pass: download fastcdr source via ExternalProject (may fail at configure).
    # This seeds the fastcdr/src/fastcdr source directory.
    local FASTCDR_CMAKE_DIR="$MICRO_XRCE_TEMP_INSTALL_DIR/fastcdr-2.2.4/lib/cmake/fastcdr"
    local fastcdr_dir_arg=""
    if [ -d "$FASTCDR_CMAKE_DIR" ]; then
        fastcdr_dir_arg="-Dfastcdr_DIR=$FASTCDR_CMAKE_DIR"
        log_info "Using pre-built fastcdr: $FASTCDR_CMAKE_DIR"
    fi

    if [ -n "${LD_LIBRARY_PATH:-}" ]; then OLD_LD="$LD_LIBRARY_PATH"; unset LD_LIBRARY_PATH; _UNSET_LD=1; fi

    POKY_ENVIRONMENT_SETUP="$POKY_ENVIRONMENT_SETUP" "$sys_cmake" "$AGENT_DIR/Micro-XRCE-DDS-Agent" \
        -DCMAKE_TOOLCHAIN_FILE="$AGENT_DIR/cross.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DUAGENT_BUILD_TESTS=OFF \
        -DUAGENT_BUILD_USAGE_EXAMPLES=OFF \
        -DUAGENT_P2P_PROFILE=OFF \
        -DUAGENT_SOCKETCAN_PROFILE=OFF \
        -DUAGENT_BUILD_EXECUTABLE=OFF \
        $fastcdr_dir_arg

    local cmake_ret=$?
    [ "${_UNSET_LD:-}" = 1 ] && { export LD_LIBRARY_PATH="$OLD_LD"; unset OLD_LD _UNSET_LD; }
    export PATH="$saved_path"

    if [ $cmake_ret -ne 0 ]; then
        log_error "Micro-XRCE-DDS-Agent cmake configuration failed"
        return 1
    fi

    # If fastcdr wasn't pre-built, build it now (source downloaded by cmake configure above)
    prebuild_fastcdr

    # If fastcdr was just built, re-run cmake configure with fastcdr_DIR
    FASTCDR_CMAKE_DIR="$MICRO_XRCE_TEMP_INSTALL_DIR/fastcdr-2.2.4/lib/cmake/fastcdr"
    if [ -d "$FASTCDR_CMAKE_DIR" ] && [ -z "$fastcdr_dir_arg" ]; then
        log_step "Re-configuring with pre-built fastcdr..."
        export PATH="/usr/local/bin:/usr/bin:$PATH"
        sys_cmake="$(command -v cmake)"
        if [ -n "${LD_LIBRARY_PATH:-}" ]; then OLD_LD="$LD_LIBRARY_PATH"; unset LD_LIBRARY_PATH; _UNSET_LD=1; fi
        POKY_ENVIRONMENT_SETUP="$POKY_ENVIRONMENT_SETUP" "$sys_cmake" "$AGENT_DIR/Micro-XRCE-DDS-Agent" \
            -DCMAKE_TOOLCHAIN_FILE="$AGENT_DIR/cross.cmake" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DUAGENT_BUILD_TESTS=OFF \
            -DUAGENT_BUILD_USAGE_EXAMPLES=OFF \
            -DUAGENT_P2P_PROFILE=OFF \
            -DUAGENT_SOCKETCAN_PROFILE=OFF \
            -DUAGENT_BUILD_EXECUTABLE=OFF \
            -Dfastcdr_DIR="$FASTCDR_CMAKE_DIR"
        cmake_ret=$?
        [ "${_UNSET_LD:-}" = 1 ] && { export LD_LIBRARY_PATH="$OLD_LD"; unset OLD_LD _UNSET_LD; }
        export PATH="$saved_path"
        [ $cmake_ret -ne 0 ] && { log_error "Re-configure with fastcdr failed"; return 1; }
    fi

    local jobs=$(( $(nproc) / 2 ))
    if [ "$jobs" -lt 1 ]; then
        jobs=1
    fi

    local build_log="$MICRO_XRCE_BUILD_DIR/micro_xrce_make.log"
    if ! run_micro_xrce_superbuild_make "$build_log" "$jobs"; then
        local make_ret=$?

        if grep -Fq "file not recognized: file truncated" "$build_log"; then
            clean_fastdds_after_truncated_object_error
            clean_micro_xrce_after_truncated_object_error
            log_info "Retrying Micro-XRCE-DDS-Agent build after cleaning truncated objects..."

            local retry_log="$MICRO_XRCE_BUILD_DIR/micro_xrce_make_retry.log"
            local retry_jobs=1
            if ! run_micro_xrce_superbuild_make "$retry_log" "$retry_jobs"; then
                make_ret=$?
                export PATH="$saved_path"
                log_error "Micro-XRCE-DDS-Agent build failed after retry (see $retry_log)"
                return "$make_ret"
            fi
        elif grep -Fq "fatal error: tinyxml2.h: No such file or directory" "$build_log"; then
            clean_fastdds_after_truncated_object_error
            log_info "Retrying Micro-XRCE-DDS-Agent build after forcing tinyxml2 from source..."

            local retry_log="$MICRO_XRCE_BUILD_DIR/micro_xrce_make_retry.log"
            if ! run_micro_xrce_superbuild_make "$retry_log" "$jobs"; then
                make_ret=$?
                export PATH="$saved_path"
                log_error "Micro-XRCE-DDS-Agent build failed after tinyxml2 retry (see $retry_log)"
                return "$make_ret"
            fi
        else
            export PATH="$saved_path"
            log_error "Micro-XRCE-DDS-Agent build failed (see $build_log)"
            return "$make_ret"
        fi
    fi
    export PATH="$saved_path"

    if [ ! -f "$lib_so" ]; then
        # Library is produced inside the superbuild; search recursively.
        local found
        found=$(find "$MICRO_XRCE_BUILD_DIR" -name "libmicroxrcedds_agent.so" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            cp "$found" "$lib_so"
            log_info "✓ Copied library from superbuild: $found"
        else
            log_error "libmicroxrcedds_agent.so not found after build"
            return 1
        fi
    fi

    log_info "✓ Micro-XRCE-DDS-Agent library built: $lib_so"
}

# Build custom agent
build_custom_agent() {
    log_step "Building Custom XRCE Agent..."
    
    # Create build directory
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Configure with CMake
    # Temporarily unset LD_LIBRARY_PATH to avoid cmake relocation errors
    log_info "Configuring with CMake..."
    local saved_path="$PATH"
    export PATH="/usr/local/bin:/usr/bin:$PATH"
    local sys_cmake
    sys_cmake="$(command -v cmake)"

    if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        OLD_CMAKE_LD_PATH="$LD_LIBRARY_PATH"
        unset LD_LIBRARY_PATH
    fi

    # ENABLE_AI_CAMERA=ON adds YOLOv8n DRP-AI + QGC H.264 stream support
    # Requires: rzv_drp-ai_tvm symlink at project root with pre-built V2H runtime
    # Default: OFF. To build with AI camera: ENABLE_AI_CAMERA=ON ./compile_agent.sh build
    local AI_CAMERA_FLAG="${ENABLE_AI_CAMERA:-OFF}"

    POKY_ENVIRONMENT_SETUP="$POKY_ENVIRONMENT_SETUP" "$sys_cmake" "$AGENT_DIR" \
        -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$AGENT_DIR/cross.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DENABLE_AI_CAMERA="${AI_CAMERA_FLAG}"

    CMAKE_RET=$?

    # Restore LD_LIBRARY_PATH
    if [ -n "${OLD_CMAKE_LD_PATH:-}" ]; then
        export LD_LIBRARY_PATH="$OLD_CMAKE_LD_PATH"
        unset OLD_CMAKE_LD_PATH
    fi
    export PATH="$saved_path"

    if [ $CMAKE_RET -ne 0 ]; then
        log_error "CMake configuration failed"
        return 1
    fi
    
    # Build
    log_info "Building with make..."
    export PATH="/usr/local/bin:/usr/bin:$PATH"
    if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        OLD_MAKE_LD_PATH="$LD_LIBRARY_PATH"
        unset LD_LIBRARY_PATH
    fi

    POKY_ENVIRONMENT_SETUP="$POKY_ENVIRONMENT_SETUP" make -C "$BUILD_DIR" -j$(( $(nproc) / 2 ))
    MAKE_RET=$?

    # Restore LD_LIBRARY_PATH
    if [ -n "${OLD_MAKE_LD_PATH:-}" ]; then
        export LD_LIBRARY_PATH="$OLD_MAKE_LD_PATH"
        unset OLD_MAKE_LD_PATH
    fi
    export PATH="$saved_path"

    if [ $MAKE_RET -ne 0 ]; then
        log_error "Build failed"
        return 1
    fi
    
    # Check if binary was created
    if [ -f "$BUILD_DIR/CustomXRCEAgent" ]; then
        # Copy to agent directory root for easier access
        log_info "✓ Build completed successfully"
        log_info "Binary: $BUILD_DIR/CustomXRCEAgent"
        if [ "$RZV_AGENT_AUTO_DEPLOY" = "ON" ]; then
            log_info "Auto-deploying CustomXRCEAgent to target board..."
            copy_agent_to_target
        fi

        # Check binary info
        log_info "Binary information:"
        if command -v file >/dev/null 2>&1; then
            file "$BUILD_DIR/CustomXRCEAgent"
        else
            log_warn "'file' utility not found; skipping ELF summary"
        fi
        ls -la "$BUILD_DIR/CustomXRCEAgent"
    else
        log_error "✗ Binary not found after build"
        return 1
    fi
}

# Clean build
clean_build() {
    log_step "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    log_info "✓ Build directory and binary cleaned"
}

# Show status
show_status() {
    echo "=== Custom XRCE Agent Build Status ==="
    echo ""
    
    if [ -f "$BUILD_DIR/CustomXRCEAgent" ]; then
        echo -e "Binary: ${GREEN}✓ Built${NC}"
        echo "  Location: $BUILD_DIR/CustomXRCEAgent"
        if command -v file >/dev/null 2>&1; then
            echo "  File info: $(file "$BUILD_DIR/CustomXRCEAgent")"
        fi
        echo "  Size: $(du -h "$BUILD_DIR/CustomXRCEAgent" | cut -f1)"
    else
        echo -e "Binary: ${RED}✗ Not built${NC}"
    fi
    
    # Check submodule
    if [ -f "$AGENT_DIR/Micro-XRCE-DDS-Agent/CMakeLists.txt" ]; then
        echo -e "Submodule: ${GREEN}✓ Ready${NC}"
    else
        echo -e "Submodule: ${RED}✗ Not initialized${NC}"
    fi
    
    # Check toolchain
    if [ -f "/opt/toolchains/poky/3.1.31/environment-setup-aarch64-poky-linux" ]; then
        echo -e "Toolchain: ${GREEN}✓ Poky aarch64 available${NC}"
    else
        echo -e "Toolchain: ${RED}✗ Poky toolchain missing${NC}"
    fi
    
    # Check CMake
    cmake_version=$(cmake --version 2>/dev/null | head -n1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+' || echo "not found")
    if [ "$cmake_version" != "not found" ]; then
        echo -e "CMake: ${GREEN}✓ $cmake_version${NC}"
    else
        echo -e "CMake: ${RED}✗ Not available${NC}"
    fi
}

# Show usage
show_usage() {
    echo "Usage: $0 {build|clean|status|deploy-ca55|copy-deps}"
    echo ""
    echo "Commands:"
    echo "  build        - Build custom XRCE agent"
    echo "  clean        - Clean build directory"
    echo "  status       - Show build status"
    echo "  deploy-ca55  - Deploy CustomXRCEAgent binary to the CA55 board (atomic replace + service restart)"
    echo "  copy-deps    - Copy Micro-XRCE/openssl dependencies to the CA55 board"
    echo ""
    echo "Environment variables:"
    echo "    POKY_ENVIRONMENT_SETUP   Required path to the Poky environment script"
    echo "    RZV_TARGET_HOST          CA55 board host (default: \$RDK_IP or 192.168.1.150)"
    echo "    RZV_TARGET_USER          CA55 SSH user (default: root)"
    echo "    RZV_TARGET_BIN_DIR       Destination directory for the binary (default: /usr/bin)"
    echo "    RZV_TARGET_LIB_DIR       Destination directory for dependencies (default: /lib/aarch64-linux-gnu)"
    echo "    RZV_AGENT_AUTO_DEPLOY     Set to ON to auto-copy the binary after build (default: OFF)"
    echo "    RZV_AGENT_SERVICE_NAME    systemd service name to restart after copy (default: custom_xrce_agent)"
    echo "    RZV_SSH_OPTIONS          Additional ssh/scp options (default: -o StrictHostKeyChecking=no)"
}

# Main execution
case "$1" in
    build)
        setup_poky_environment
        check_submodule
        build_micro_xrce_agent_lib
        build_custom_agent
        ;;
    clean)
        clean_build
        ;;
    status)
        show_status
        ;;
    deploy-ca55)
        copy_agent_to_target
        ;;
    copy-deps)
        setup_poky_environment
        copy_dependencies
        ;;
    *)
        show_usage
        exit 1
        ;;
esac

log_info "Done!"
