#!/bin/bash
# check-agent-runtime-closure.sh — deploy preflight for the CA55 CustomXRCEAgent.
#
# When the agent is cross-built against the Ubuntu 24.04 ARM64 sysroot (Docker toolchain)
# but deployed onto an older Yocto board, the binary may require a newer glibc/libstdc++
# or pull in libraries the board does not ship. This tool verifies, FAIL-CLOSED, that the
# target can actually run the binary before anything is copied.
#
# Policy:
#   * REJECT if the target's glibc or libstdc++ is older than the binary requires
#     (you cannot fix this by shipping libs — the loader/ABI must match).
#   * FORBID shipping core system libs: libc / ld-linux / libstdc++ / libssl / libcrypto.
#     If any of those is missing on the target, that is a board-image problem, not something
#     to paper over with LD_LIBRARY_PATH.
#   * ALLOW staging only genuinely-missing NON-system libs (e.g. libjpeg.so.8) into
#     /home/root/agent_libs, which the service adds via LD_LIBRARY_PATH (reversible).
#
# Usage:
#   tools/check-agent-runtime-closure.sh root@<board-ip> [binary] [--stage]
#     binary   defaults to ca55_stack/xrce_dds_agent/src/build/CustomXRCEAgent
#     --stage  copy the allowed missing libs into /home/root/agent_libs on the target
#
# Lib source for --stage: the Docker image's ARM64 sysroot (same one the binary was built
# against), so SONAMEs match. Override with LIB_SRC=/path to a local lib dir.

set -euo pipefail

IMAGE="ghcr.io/renesas-rdk/rzv2h_ubuntu_xbuild:latest"
SYSROOT_IN_IMAGE="/opt/arm64_sysroot"
TARGET_STAGE_DIR="/home/root/agent_libs"
# Core system libraries that must come from the board image, never staged.
FORBIDDEN_RE='^(ld-linux-aarch64|libc|libstdc\+\+|libssl|libcrypto)\.'

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_BIN="${REPO_ROOT}/ca55_stack/xrce_dds_agent/src/build/CustomXRCEAgent"

TARGET=""
BINARY="${DEFAULT_BIN}"
STAGE=0
for arg in "$@"; do
    case "$arg" in
        --stage) STAGE=1 ;;
        *@*)     TARGET="$arg" ;;
        *)       BINARY="$arg" ;;
    esac
done

[ -n "$TARGET" ] || { log_error "Usage: $0 root@<board-ip> [binary] [--stage]"; exit 2; }
[ -f "$BINARY" ] || { log_error "Binary not found: $BINARY"; exit 2; }
command -v readelf >/dev/null || { log_error "readelf not found (install binutils)"; exit 2; }

SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8"

# ── version helpers ───────────────────────────────────────────────────────────
# Highest dotted version among args; ver_ge A B → true if A >= B.
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }
max_ver() { printf '%s\n' "$@" | grep -E '^[0-9]+(\.[0-9]+)*$' | sort -V | tail -1; }

# Required symbol-version floors the binary imports (from .gnu.version_r).
req_versions() {
    local prefix="$1"  # GLIBC_ or GLIBCXX_
    readelf -V "$BINARY" 2>/dev/null \
        | grep -oE "${prefix}[0-9]+(\.[0-9]+)*" \
        | sed "s/${prefix}//" | sort -V | uniq
}

log_info "Binary : $BINARY"
log_info "Target : $TARGET"

REQ_GLIBC="$(max_ver $(req_versions 'GLIBC_'))"
REQ_GLIBCXX="$(max_ver $(req_versions 'GLIBCXX_'))"
log_info "Requires: GLIBC ${REQ_GLIBC:-none}, GLIBCXX ${REQ_GLIBCXX:-none}"

# ── target capability ─────────────────────────────────────────────────────────
TGT_GLIBC="$($SSH "$TARGET" "getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print \$2}'" 2>/dev/null || true)"
[ -n "$TGT_GLIBC" ] || TGT_GLIBC="$($SSH "$TARGET" "ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\$'" 2>/dev/null || true)"
TGT_GLIBCXX="$($SSH "$TARGET" "
    for d in /usr/lib /lib /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu; do
        [ -e \$d/libstdc++.so.6 ] && { strings \$d/libstdc++.so.6 | grep -oE 'GLIBCXX_[0-9.]+'; break; }
    done | sed 's/GLIBCXX_//' | sort -V | tail -1" 2>/dev/null || true)"
log_info "Target has: GLIBC ${TGT_GLIBC:-?}, GLIBCXX ${TGT_GLIBCXX:-?}"

FAIL=0

if [ -n "$REQ_GLIBC" ] && [ -n "$TGT_GLIBC" ] && ! ver_ge "$TGT_GLIBC" "$REQ_GLIBC"; then
    log_error "Target glibc $TGT_GLIBC < required $REQ_GLIBC — binary will NOT load. Rebuild against an older sysroot or update the board image."
    FAIL=1
fi
if [ -n "$REQ_GLIBCXX" ] && [ -n "$TGT_GLIBCXX" ] && ! ver_ge "$TGT_GLIBCXX" "$REQ_GLIBCXX"; then
    log_error "Target libstdc++ GLIBCXX $TGT_GLIBCXX < required $REQ_GLIBCXX — forbidden to ship libstdc++. Update the board image."
    FAIL=1
fi

# ── shared-library closure ────────────────────────────────────────────────────
NEEDED="$(readelf -d "$BINARY" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
# Gather every SONAME the target loader knows about: the ld.so cache plus a raw listing
# of the standard lib dirs (covers libs present but not yet in the cache).
TARGET_LIBS="$($SSH "$TARGET" '
    ldconfig -p 2>/dev/null
    for d in /usr/lib /lib /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu; do
        [ -d "$d" ] && ls -1 "$d" 2>/dev/null
    done' 2>/dev/null || true)"

# here-string (not a pipe) so grep -q's early exit can't SIGPIPE a producer under pipefail.
target_has() { grep -qF "$1" <<<"$TARGET_LIBS"; }

STAGE_LIST=()
for lib in $NEEDED; do
    if target_has "$lib"; then
        continue  # present on target (cache or on-disk)
    fi
    if printf '%s' "$lib" | grep -qE "$FORBIDDEN_RE"; then
        log_error "Missing CORE system lib on target: $lib — forbidden to ship; fix the board image."
        FAIL=1
    else
        log_warn "Missing non-system lib on target: $lib — eligible for staging into $TARGET_STAGE_DIR"
        STAGE_LIST+=("$lib")
    fi
done

if [ "$FAIL" = "1" ]; then
    log_error "Closure check FAILED — do not deploy."
    exit 1
fi

if [ "${#STAGE_LIST[@]}" -eq 0 ]; then
    log_info "✓ Closure check PASSED — target satisfies the binary; nothing to stage."
    exit 0
fi

log_info "✓ ABI OK. ${#STAGE_LIST[@]} non-system lib(s) need staging: ${STAGE_LIST[*]}"
if [ "$STAGE" != "1" ]; then
    log_warn "Re-run with --stage to copy them into ${TARGET_STAGE_DIR} on the target."
    exit 0
fi

# ── stage allowed libs (resolve SONAME symlinks to real files) ─────────────────
log_info "Staging libs into ${TARGET_STAGE_DIR}..."
$SSH "$TARGET" "mkdir -p ${TARGET_STAGE_DIR}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
for lib in "${STAGE_LIST[@]}"; do
    if [ -n "${LIB_SRC:-}" ] && [ -e "${LIB_SRC}/${lib}" ]; then
        cp -L "${LIB_SRC}/${lib}" "${TMP}/${lib}"
    else
        # Pull from the Docker image sysroot so the SONAME matches the build.
        docker run --rm "${IMAGE}" \
            sh -c "f=\$(find ${SYSROOT_IN_IMAGE}/usr/lib ${SYSROOT_IN_IMAGE}/lib -name '${lib}' 2>/dev/null | head -1); \
                   [ -n \"\$f\" ] && cat \"\$(readlink -f \"\$f\")\"" > "${TMP}/${lib}" 2>/dev/null || true
    fi
    if [ ! -s "${TMP}/${lib}" ]; then
        log_error "Could not source ${lib} (set LIB_SRC=/path to a dir containing it)."
        exit 1
    fi
    scp -o StrictHostKeyChecking=no "${TMP}/${lib}" "${TARGET}:${TARGET_STAGE_DIR}/"
    log_info "  staged ${lib}"
done
log_info "✓ Staged ${#STAGE_LIST[@]} lib(s). Ensure the service exports LD_LIBRARY_PATH=${TARGET_STAGE_DIR}."
