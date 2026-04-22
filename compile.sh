#!/bin/bash
# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"

BUILD_DIR="Debug"
PX4_DIR="px4"
PX4_BUILD_DIR="$PX4_DIR/build/rzv"
JOBS="${NINJA_JOBS:-$(nproc)}"
CMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE:-$REPO_ROOT/cross.cmake}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
MAVLINK_INSTALL_DIR="${MAVLINK_INSTALL_DIR:-$PX4_DIR/src/modules/mavlink/mavlink/install}"
RZV_MAVLINK_USE_OPENAMP="${RZV_MAVLINK_USE_OPENAMP:-OFF}"
PX4_ENABLE_STACK_MONITOR="${PX4_ENABLE_STACK_MONITOR:-OFF}"
CONFIG_RZV_MEM_TELEMETRY="${CONFIG_RZV_MEM_TELEMETRY:-OFF}"
CONFIG_SYSCALL_DEBUG_VERBOSE="${CONFIG_SYSCALL_DEBUG_VERBOSE:-ON}"
SUPER_EXTRA_CMAKE_ARGS="${SUPER_EXTRA_CMAKE_ARGS:-}"
PX4_EXTRA_CMAKE_ARGS="${PX4_EXTRA_CMAKE_ARGS:-}"
BUILD_VERBOSE="${BUILD_VERBOSE:-OFF}"
RZV_SKIP_CA55_AGENT="${RZV_SKIP_CA55_AGENT:-OFF}"

CCACHE_LAUNCHER=""
if command -v ccache >/dev/null 2>&1; then
    CCACHE_LAUNCHER="${CCACHE_LAUNCHER_OVERRIDE:-$(command -v ccache)}"
    : "${CCACHE_DIR:=$REPO_ROOT/.ccache}"
    mkdir -p "${CCACHE_DIR}"
    export CCACHE_DIR
fi

log_info() {
    echo "[INFO] $*"
}

log_warn() {
    echo "[WARN] $*"
}

# Simple helper to report which ninja outputs were rebuilt (uses px4 build tree)
log_recent_ninja_outputs() {
    local since_ms="$1"
    local ninja_log="$PX4_BUILD_DIR/.ninja_log"

    [ -f "$ninja_log" ] || return

    local outputs
    outputs=$(awk -v since="$since_ms" '
        BEGIN { FS=" " }
        NR==1 && $0 ~ /^# ninja log/ { next }
        NF >= 4 && $3 >= since { print $4 }
    ' "$ninja_log" | sort -u)

    [ -z "$outputs" ] && return

    local count
    count=$(echo "$outputs" | wc -l)
    if [ "$BUILD_VERBOSE" = "ON" ]; then
        log_info "Rebuilt $count output(s):"
        while IFS= read -r o; do
            echo "  + $o"
        done <<< "$outputs"
    else
        log_info "Rebuilt $count output(s) (BUILD_VERBOSE=ON to list)."
    fi
}

ensure_git_available() {
    if ! command -v git >/dev/null 2>&1; then
        log_warn "git not found in PATH; cannot fetch missing modules automatically."
        return 1
    fi
    return 0
}

is_nonempty_dir() {
    local dir_path="$1"
    [ -d "$dir_path" ] && [ -n "$(find "$dir_path" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]
}

is_registered_submodule_path() {
    local repo_path="$1"
    local module_path="$2"

    [ -f "$repo_path/.gitmodules" ] || return 1

    git -C "$repo_path" config --file .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null \
        | awk '{print $2}' \
        | grep -Fxq "$module_path"
}

ensure_repo_checkout() {
    local marker_file="$1"
    local target_dir="$2"
    local parent_repo="$3"
    local module_path="$4"
    local remote_url="$5"
    local branch="$6"
    local label="$7"

    if [ -f "$marker_file" ]; then
        return 0
    fi

    log_warn "$label is missing; attempting automatic recovery."

    if ensure_git_available && [ -d "$parent_repo/.git" ] && is_registered_submodule_path "$parent_repo" "$module_path"; then
        log_info "Trying git submodule update for $label..."
        if git -C "$parent_repo" submodule sync -- "$module_path" >/dev/null 2>&1 \
            && git -C "$parent_repo" submodule update --init --recursive --depth 1 -- "$module_path" >/dev/null 2>&1; then
            if [ -f "$marker_file" ]; then
                log_info "$label recovered via git submodule update."
                return 0
            fi
        fi

        log_warn "Submodule update did not restore $label; falling back to direct clone."
    fi

    mkdir -p "$(dirname "$target_dir")"

    if [ -d "$target_dir/.git" ]; then
        log_info "$label exists as git repo; refreshing from origin/$branch..."
        if git -C "$target_dir" fetch --depth 1 origin "$branch" >/dev/null 2>&1 \
            && git -C "$target_dir" checkout -q FETCH_HEAD >/dev/null 2>&1; then
            [ -f "$marker_file" ] && return 0
        fi
    elif [ -d "$target_dir" ]; then
        # Directory exists but incomplete (no .git or marker missing) → remove and re-clone
        if ! is_nonempty_dir "$target_dir" || [ ! -f "$marker_file" ]; then
            log_warn "$label directory appears incomplete; removing for fresh clone."
            rm -rf "$target_dir"
        fi
    fi

    if [ ! -d "$target_dir" ]; then
        log_info "Cloning $label from $remote_url (branch: $branch)..."
        git clone --depth 1 --branch "$branch" "$remote_url" "$target_dir" >/dev/null 2>&1 || true
    fi

    if [ ! -f "$marker_file" ]; then
        log_warn "Failed to auto-recover $label. Please check network/repo access and retry."
        return 1
    fi

    log_info "$label recovered via direct clone fallback."
}

ensure_required_executables() {
    local scripts=(
        "$REPO_ROOT/compile.sh"
        "$REPO_ROOT/script/postbuild.sh"
        "$REPO_ROOT/ca55_stack/xrce_dds_agent/compile_agent.sh"
    )

    local script
    for script in "${scripts[@]}"; do
        [ -f "$script" ] || continue
        if [ ! -x "$script" ]; then
            log_warn "Missing executable bit: $script; applying chmod +x"
            chmod +x "$script"
        fi
    done
}

python_deps_cache_key() {
    local py_bin py_ver
    py_bin="$(command -v python3 2>/dev/null || true)"
    py_ver="$(python3 -c 'import sys; print("{}.{}.{}".format(*sys.version_info[:3]))' 2>/dev/null || true)"
    printf '%s\n' "v1|${py_bin}|${py_ver}|kconfiglib,empy==3.3.4,pyros-genmsg,pyros-genpy,pyyaml,jinja2,jsonschema,lxml"
}

python_import_ok() {
    local module_name="$1"
    python3 - "$module_name" >/dev/null 2>&1 <<'PY'
import importlib
import sys

mod = sys.argv[1]
importlib.import_module(mod)
PY
}

empy_compatible_ok() {
    python3 >/dev/null 2>&1 <<'PY'
import em
import sys

sys.exit(0 if hasattr(em, "RAW_OPT") else 1)
PY
}

ensure_python_build_dependencies() {
    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "python3 not found in PATH; cannot validate/install build dependencies."
        return 1
    fi

    if ! python3 -m pip --version >/dev/null 2>&1; then
        log_warn "python3 -m pip is unavailable; please install pip for Python3."
        return 1
    fi

    local cache_file="$BUILD_DIR/.python_deps_cache"
    local expected_key
    expected_key="$(python_deps_cache_key)"

    # Fast path: skip full dependency scan when the Python environment signature
    # and required package set have not changed.
    if [ "${RZV_FORCE_PY_DEPS_CHECK:-OFF}" != "ON" ] \
        && [ -f "$cache_file" ] \
        && [ "$(cat "$cache_file" 2>/dev/null)" = "$expected_key" ]; then
        log_info "Python dependency preflight cache hit; skipping full dependency scan."
        return 0
    fi

    local missing_pkgs=()

    # kconfiglib exposes the menuconfig module used by PX4 kconfig.cmake
    python_import_ok "menuconfig" || missing_pkgs+=("kconfiglib")

    # PX4 templates currently require Empy 3.x API (RAW_OPT/BUFFERED_OPT)
    empy_compatible_ok || missing_pkgs+=("empy==3.3.4")

    python_import_ok "genmsg" || missing_pkgs+=("pyros-genmsg")
    python_import_ok "genpy" || missing_pkgs+=("pyros-genpy")
    python_import_ok "yaml" || missing_pkgs+=("pyyaml")
    python_import_ok "jinja2" || missing_pkgs+=("jinja2")
    python_import_ok "jsonschema" || missing_pkgs+=("jsonschema")
    python_import_ok "lxml.etree" || missing_pkgs+=("lxml")

    if [ ${#missing_pkgs[@]} -eq 0 ]; then
        mkdir -p "$BUILD_DIR"
        printf '%s\n' "$expected_key" > "$cache_file"
        log_info "Python build dependencies are already installed."
        return 0
    fi

    log_info "Installing missing Python build dependencies: ${missing_pkgs[*]}"
    if ! python3 -m pip install --user "${missing_pkgs[@]}"; then
        log_warn "Auto-install of Python dependencies failed. Please install manually and retry."
        return 1
    fi

    # Re-check key imports after installation.
    python_import_ok "menuconfig" || return 1
    empy_compatible_ok || return 1
    python_import_ok "genmsg" || return 1
    python_import_ok "genpy" || return 1
    python_import_ok "yaml" || return 1
    python_import_ok "jinja2" || return 1
    python_import_ok "jsonschema" || return 1
    python_import_ok "lxml.etree" || return 1

    mkdir -p "$BUILD_DIR"
    printf '%s\n' "$expected_key" > "$cache_file"
    log_info "Python build dependencies installed and validated."
}

# Ensure all git submodules are initialized (px4, ca55_stack, etc).
# This replaces individual ensure_* functions since all dependencies are already
# declared in .gitmodules files (px4/.gitmodules, ca55_stack/.gitmodules).
# Note: Micro-XRCE-DDS-Client source is NOT required — prebuilt static libs
# in px4/src/modules/uxrce_dds_client/prebuilt/ replace it entirely.
ensure_all_submodules() {
    ensure_git_available || return

    # Check if any critical submodules are missing
    local need_init=0

    # Check PX4 critical submodules
    [ ! -f "px4/src/modules/mavlink/mavlink/CMakeLists.txt" ] && need_init=1

    # Check CA55 agent submodule (always required)
    [ ! -f "ca55_stack/xrce_dds_agent/Micro-XRCE-DDS-Agent/CMakeLists.txt" ] && need_init=1

    [ $need_init -eq 0 ] && return

    log_info "Initializing git submodules (this may take a while on first build)..."

    # Try shallow clone first (faster), fallback to full clone if not supported
    if git submodule update --init --recursive --depth 1 2>/dev/null; then
        log_info "Submodules initialized successfully (shallow clone)"
    else
        log_warn "Shallow clone not supported, using full clone..."
        git submodule update --init --recursive
        log_info "Submodules initialized successfully"
    fi
}

# Ensure POKY environment script is available and sourced for CA55 builds.
# If the variable is not set, default it to the common Poky environment script
# path and export it so downstream steps can use it.
if [ -z "${POKY_ENVIRONMENT_SETUP:-}" ]; then
    export POKY_ENVIRONMENT_SETUP="/opt/toolchains/poky/3.1.31/environment-setup-aarch64-poky-linux"
    log_info "POKY_ENVIRONMENT_SETUP not set; defaulting to $POKY_ENVIRONMENT_SETUP"
fi


get_cache_value() {
    local var_name="$1"
    local cache_file="$BUILD_DIR/CMakeCache.txt"

    if [ ! -f "$cache_file" ]; then
        return 1
    fi

    local line
    line=$(grep -m1 "^${var_name}:" "$cache_file" 2>/dev/null || true)

    if [ -z "$line" ]; then
        return 1
    fi

    printf "%s\n" "${line##*=}"
    return 0
}

usage() {
    cat <<USAGE
Usage: $0 [build|rebuild|clean|deploy-cr8|help]

Commands:
  build        Configure (if needed) and build incrementally (default, RECOMMENDED for daily use)
               - Auto-clones missing repos/submodules
               - Auto-recovers missing nested dependencies (with git submodule + clone fallback)
               - Auto-fixes executable permission for required build scripts
               - Uses timestamp-based incremental builds (faster)
               - Only rebuilds files that changed since last build
  rebuild      Remove build trees then rebuild from scratch
               - Use when switching branches or major config changes
               - Slower but guarantees clean build
  clean        Remove generated build directories
  deploy-cr8   Deploy CR8 firmware binaries to the RDK board via scp
               - Copies rzv2h_px4_freertos_itcm.bin and _sdram.bin to /boot/cr8_data/
               - Uses RZV_TARGET_HOST (default: 192.168.1.150) and RZV_TARGET_USER (default: root)
  help         Show this message

Build Strategy:
  - For daily development: Use 'build' (incremental, timestamp-based)
  - After git pull/merge: Use 'build' first, 'rebuild' only if issues occur
  - When changing major flags: Use 'rebuild'

Environment variables:
    RZV_MAVLINK_USE_OPENAMP    Set to ON to route MAVLink/QGC over OpenAMP, OFF to use UART5/SIK (default: OFF)
    PX4_ENABLE_STACK_MONITOR   Set to ON or OFF to control the PX4 stack monitor task (default: OFF)
    CONFIG_RZV_MEM_TELEMETRY   Set to ON or OFF to enable runtime heap/IRQ telemetry (default: OFF)
    CONFIG_SYSCALL_DEBUG_VERBOSE Set to ON or OFF to toggle syscall verbose logging (default: ON)
    CMAKE_BUILD_TYPE           Override CMake build type (default: RelWithDebInfo)
    BUILD_VERBOSE              Set to ON to show detailed build output and changed files (default: OFF)
    RZV_TARGET_HOST            RDK board IP for deploy-cr8 (default: $RDK_IP or 192.168.1.150)
    RZV_TARGET_USER            SSH user for deploy-cr8 (default: RDK_USER env or root)
    RZV_CR8_BIN_DIR            Destination directory on board for CR8 binaries (default: /boot/cr8_data)
    RZV_SSH_OPTIONS            Extra ssh/scp options (default: -o StrictHostKeyChecking=no)

Examples:
    # Normal build (quiet)
    ./compile.sh build

    # Verbose build (show changed files and build commands)
    BUILD_VERBOSE=ON ./compile.sh build
USAGE
}

git_paths_dirty() {
    local paths=("$@")
    local status
    status=$(git -C "$REPO_ROOT" status -sb -- "${paths[@]}" 2>/dev/null || true)
    [ -n "$status" ]
}

# Check if any files in the given paths have been modified since last build
# (timestamp-based check, not git-based)
files_modified_since_last_build() {
    local paths=("$@")
    local reference_file="$BUILD_DIR/.last_build_timestamp"

    # If no previous build, always rebuild
    [ ! -f "$reference_file" ] && return 0

    # Check if any file is newer than the reference timestamp
    local found_newer=0
    local changed_files=()

    for path in "${paths[@]}"; do
        if [ -e "$path" ]; then
            while IFS= read -r -d '' file; do
                if [ "$file" -nt "$reference_file" ]; then
                    found_newer=1
                    changed_files+=("$file")
                fi
            done < <(find "$path" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.cmake" -o -name "CMakeLists.txt" -o -name "*.sh" -o -name "rc.*" -o -name "*.cmds" \) -print0 2>/dev/null)
        fi
    done

    if [ ${#changed_files[@]} -gt 0 ]; then
        if [ "$BUILD_VERBOSE" = "ON" ]; then
            log_info "Detected ${#changed_files[@]} changed file(s) since last build:"
            for file in "${changed_files[@]}"; do
                echo "  • $file"
            done
        else
            log_info "Detected ${#changed_files[@]} changed file(s) since last build."
        fi
    fi

    # For header files, touch ALL .c/.cpp files that include them to trigger Ninja rebuild
    # Ninja doesn't check header timestamps directly, only through depfiles
    if [ $found_newer -eq 1 ]; then
        for file in "${changed_files[@]}"; do
            case "$file" in
                *.h|*.hpp)
                    local header_name="$(basename "$file")"
                    local header_dir="$(dirname "$file")"
                    local touched_count=0

                    local search_dirs=("$header_dir" "$(dirname "$header_dir")")
                    for search_dir in "${search_dirs[@]}"; do
                        if [ -d "$search_dir" ]; then
                            while IFS= read -r -d '' src_file; do
                                if grep -q "#include.*[\"<]${header_name}[\">]" "$src_file" 2>/dev/null; then
                                    touch "$src_file"
                                    touched_count=$((touched_count + 1))
                                    [ "$BUILD_VERBOSE" = "ON" ] && log_info "  → Touched $src_file (includes $header_name)"
                                fi
                            done < <(find "$search_dir" -maxdepth 3 -type f \( -name "*.c" -o -name "*.cpp" \) -print0 2>/dev/null)
                        fi
                    done

                    [ $touched_count -gt 0 ] && [ "$BUILD_VERBOSE" = "ON" ] && log_info "  → Total: $touched_count file(s) touched for $header_name"
                    ;;
            esac
        done
    fi

    # Return 0 (success) if files were found, 1 (failure) if not
    # In bash: return 0 = true, return 1 = false
    [ $found_newer -eq 1 ] && return 0
    return 1
}

should_build_px4() {
    # Always build if build.ninja doesn't exist
    [ ! -f "$BUILD_DIR/build.ninja" ] && return 0

    # Use timestamp-based check for incremental builds
    # Check all source directories that affect the build
    files_modified_since_last_build \
        px4/src \
        px4/modules \
        px4/boards \
        px4/ROMFS \
        rzv \
        rzv_cfg \
        rzv_gen \
        script \
        src && return 0

    return 1
}

uXRCE_agent_sources_changed() {
    git_paths_dirty \
        ca55_stack/xrce_dds_agent \
        ca55_stack/xrce_dds_agent/cross.cmake \
        ca55_stack/xrce_dds_agent/compile_agent.sh
}

needs_ca55_agent_build() {
    [ ! -f "ca55_stack/xrce_dds_agent/src/build/CustomXRCEAgent" ] && return 0

    # Use timestamp-based check for incremental builds
    # Store timestamp in build directory to keep workspace clean
    local agent_timestamp="$BUILD_DIR/.last_agent_build_timestamp"
    if [ -f "$agent_timestamp" ]; then
        files_modified_since_last_build \
            ca55_stack/xrce_dds_agent \
            ca55_stack/xrce_dds_agent/cross.cmake \
            ca55_stack/xrce_dds_agent/compile_agent.sh && return 0
    else
        # No timestamp file, check using git status as fallback
        uXRCE_agent_sources_changed && return 0
    fi

    return 1
}

configure_super() {
    local need_reconfigure=0

    # If cache exists, re-run CMake when important flags change.
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then

        local cached_mavlink_path
        cached_mavlink_path=$(get_cache_value "RZV_MAVLINK_USE_OPENAMP" || true)
        if [ -n "$cached_mavlink_path" ] && [ "$cached_mavlink_path" != "$RZV_MAVLINK_USE_OPENAMP" ]; then
            log_info "RZV_MAVLINK_USE_OPENAMP changed (cache=$cached_mavlink_path, requested=$RZV_MAVLINK_USE_OPENAMP); reconfiguring."
            need_reconfigure=1
        fi

        local cached_verbose
        cached_verbose=$(get_cache_value "CMAKE_VERBOSE_MAKEFILE" || true)
        if [ -n "$cached_verbose" ] && [ "$cached_verbose" != "$BUILD_VERBOSE" ]; then
            log_info "CMAKE_VERBOSE_MAKEFILE changed (cache=$cached_verbose, requested=$BUILD_VERBOSE); reconfiguring."
            need_reconfigure=1
        fi
    fi

    if [ ! -f "$BUILD_DIR/build.ninja" ] || [ "$need_reconfigure" -eq 1 ]; then
        local cmake_args=(
            -S . -B "$BUILD_DIR" -G Ninja
            -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE"
            -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
            -DRZV_MAVLINK_USE_OPENAMP=${RZV_MAVLINK_USE_OPENAMP}
            -DCMAKE_PREFIX_PATH="$MAVLINK_INSTALL_DIR"
            -DMAVLINK_INCLUDE_DIR="$MAVLINK_INSTALL_DIR/include"
            -DMAVLINK_DIALECT=common
            -DPX4_ENABLE_STACK_MONITOR=${PX4_ENABLE_STACK_MONITOR}
            -DCONFIG_RZV_MEM_TELEMETRY=${CONFIG_RZV_MEM_TELEMETRY}
            -DCONFIG_SYSCALL_DEBUG_VERBOSE=${CONFIG_SYSCALL_DEBUG_VERBOSE}
        )

        # Always set CMAKE_VERBOSE_MAKEFILE explicitly to follow BUILD_VERBOSE
        cmake_args+=(-DCMAKE_VERBOSE_MAKEFILE=${BUILD_VERBOSE})

        if [ -n "$CCACHE_LAUNCHER" ]; then
            cmake_args+=(
                -DCMAKE_C_COMPILER_LAUNCHER="$CCACHE_LAUNCHER"
                -DCMAKE_CXX_COMPILER_LAUNCHER="$CCACHE_LAUNCHER"
            )
        fi

        if [ -n "$PX4_EXTRA_CMAKE_ARGS" ]; then
            cmake_args+=(-DPX4_EXTRA_CMAKE_ARGS="$PX4_EXTRA_CMAKE_ARGS")
        fi

        if [ -n "$SUPER_EXTRA_CMAKE_ARGS" ]; then
            # shellcheck disable=SC2206
            local extra_args=($SUPER_EXTRA_CMAKE_ARGS)
            cmake_args+=("${extra_args[@]}")
        fi

        if [ -n "$CCACHE_LAUNCHER" ]; then
            cmake_args+=(-DCCACHE_DIR="$CCACHE_DIR")
        fi

        cmake "${cmake_args[@]}"
    fi
}

build_super() {
    configure_super
    local build_start_ms
    build_start_ms=$(date +%s%N)

    if should_build_px4; then
        # Force ExternalProject to rebuild PX4 by deleting stamp files
        # ExternalProject doesn't check source timestamps, only stamp files
        local px4_stamp_dir="$BUILD_DIR/px4_external-prefix/src/px4_external-stamp"
        if [ -f "$px4_stamp_dir/px4_external-build" ]; then
            [ "$BUILD_VERBOSE" = "ON" ] && log_info "Removing px4_external stamp to force rebuild..."
            rm -f "$px4_stamp_dir/px4_external-build"
        fi

        local cmake_build_args=(--build "$BUILD_DIR" --parallel "$JOBS")

        # Add verbose flag if requested
        if [ "$BUILD_VERBOSE" = "ON" ]; then
            cmake_build_args+=(--verbose)
            log_info "Building with verbose output enabled..."
        fi

        cmake "${cmake_build_args[@]}" --target px4_lib_bundle
        cmake "${cmake_build_args[@]}" --target memory_layout_check

        # Summarize rebuilt outputs from px4 ninja log (since this build started)
        log_recent_ninja_outputs "$build_start_ms"

        # Update timestamp after successful build
        touch "$BUILD_DIR/.last_build_timestamp"
        log_info "Build timestamp updated for incremental builds."
    else
        log_info "px4/fsp sources unchanged; skipping px4 build targets."
    fi
}

build_ca55_agent_if_needed() {
    if [ "$RZV_SKIP_CA55_AGENT" = "ON" ]; then
        log_info "Skipping CA55 CustomXRCEAgent build (RZV_SKIP_CA55_AGENT=ON)."
        return
    fi

    if needs_ca55_agent_build; then
        log_info "Building CA55 CustomXRCEAgent because sources changed or binary missing."
        if [ -z "$POKY_ENVIRONMENT_SETUP" ]; then
            log_warn "Skipping CA55 CustomXRCEAgent build: set POKY_ENVIRONMENT_SETUP to the Poky env script."
            return
        fi

        if [ ! -r "$POKY_ENVIRONMENT_SETUP" ]; then
            log_warn "Skipping CA55 CustomXRCEAgent build: Poky environment script '$POKY_ENVIRONMENT_SETUP' not found or not readable."
            return
        fi

        # Run the CA55 agent build in a subshell and source the Poky environment
        # only for that subshell so it doesn't affect the outer build tools.
        (
            if [ -n "${LD_LIBRARY_PATH:-}" ]; then
                log_warn "LD_LIBRARY_PATH is set; temporarily unsetting while sourcing Poky environment script."
                OLD_LD_LIBRARY_PATH="$LD_LIBRARY_PATH"
                unset LD_LIBRARY_PATH
                _POKY_UNSET_LD=1
            fi

            # shellcheck disable=SC1090
            . "$POKY_ENVIRONMENT_SETUP"
            log_info "Sourced POKY environment from $POKY_ENVIRONMENT_SETUP"

            # Keep LD_LIBRARY_PATH unset during the agent build to avoid host
            # glibc/libpthread conflicts with the Poky toolchain.
            cd "$REPO_ROOT/ca55_stack/xrce_dds_agent" && ./compile_agent.sh build

            if [ -n "${_POKY_UNSET_LD:-}" ]; then
                export LD_LIBRARY_PATH="$OLD_LD_LIBRARY_PATH"
                unset OLD_LD_LIBRARY_PATH _POKY_UNSET_LD
                log_info "Restored previous LD_LIBRARY_PATH"
            fi
        )
        # Update timestamp after successful agent build
        if [ $? -eq 0 ]; then
            mkdir -p "$BUILD_DIR"
            touch "$BUILD_DIR/.last_agent_build_timestamp"
            log_info "Agent build timestamp updated for incremental builds."
        fi
    else
        log_info "CA55 agent sources unchanged; skipping agent rebuild."
    fi
}

# ensure_microcdr_bundled: previously cloned/patched microcdr for offline builds.
# No longer needed — prebuilt static libs in prebuilt/ replace Micro-XRCE-DDS-Client
# source entirely. Kept as no-op for backwards compatibility with any call sites.
ensure_microcdr_bundled() {
    :
}

build() {
    ensure_required_executables
    ensure_all_submodules
    ensure_python_build_dependencies
    ensure_microcdr_bundled
    build_super
    build_ca55_agent_if_needed
}

_preserve_and_remove_build_dir() {
    # e2studio generates memory_regions.ld directly into the build directory.
    # Preserve it across clean/rebuild so CMake does not overwrite it with the
    # script/ fallback on re-configure.
    local mem_ld="$BUILD_DIR/memory_regions.ld"
    local mem_ld_content=""
    [ -f "$mem_ld" ] && mem_ld_content=$(cat "$mem_ld")

    rm -rf "$BUILD_DIR" "$PX4_BUILD_DIR"

    if [ -n "$mem_ld_content" ]; then
        mkdir -p "$BUILD_DIR"
        printf '%s' "$mem_ld_content" > "$mem_ld"
        log_info "Preserved $mem_ld across clean."
    fi
}

rebuild() {
    _preserve_and_remove_build_dir
    build
}

clean() {
    _preserve_and_remove_build_dir
}

# Deploy CR8 firmware binaries to the RDK board via scp.
# Reads RZV_TARGET_HOST / RZV_TARGET_USER (fall back to RDK_IP / RDK_USER from env).
deploy_cr8() {
    local host="${RZV_TARGET_HOST:-${RDK_IP:-192.168.1.150}}"
    local user="${RZV_TARGET_USER:-${RDK_USER:-root}}"
    local dest_dir="${RZV_CR8_BIN_DIR:-/boot/cr8_data}"
    local ssh_opts="${RZV_SSH_OPTIONS:--o StrictHostKeyChecking=no}"

    local itcm="$BUILD_DIR/rzv2h_px4_freertos_itcm.bin"
    local sdram="$BUILD_DIR/rzv2h_px4_freertos_sdram.bin"

    for f in "$itcm" "$sdram"; do
        if [ ! -f "$f" ]; then
            log_warn "Firmware binary not found: $f — run './compile.sh build' first."
            exit 1
        fi
    done

    log_info "Deploying CR8 firmware to ${user}@${host}:${dest_dir} ..."
    scp $ssh_opts "$itcm" "$sdram" "${user}@${host}:${dest_dir}/"
    
    # Create U-Boot compatible aliases
    ssh $ssh_opts "${user}@${host}" "cd ${dest_dir} && ln -sf rzv2h_px4_freertos_itcm.bin asxxx_drones_itcm.bin && ln -sf rzv2h_px4_freertos_sdram.bin asxxx_drones_sdram.bin"
    
    log_info "Done. Reboot or reload CR8 to apply the new firmware."
}

cmd=${1:-build}
case "$cmd" in
    build) build ;;
    rebuild) rebuild ;;
    clean) clean ;;
    deploy-cr8) deploy_cr8 ;;
    help|-h|--help) usage ;;
    *) usage; exit 1 ;;
esac
