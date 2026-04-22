/**
 * @file drpai_helpers.h
 * @brief DRP-AI runtime definitions and ioctl wrappers for RZ/V2H
 *
 * If <linux/drpai.h> is available in the Poky sysroot it will be used.
 * Otherwise the necessary structures/ioctls are defined here as a fallback
 * (matching the Renesas RZ/V2H BSP kernel header v6.10).
 */
#ifndef AI_DRPAI_HELPERS_H
#define AI_DRPAI_HELPERS_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <errno.h>

/* ── Try system header first, else define locally ──────────────────── */

/*
 * NOTE: __has_include(<linux/drpai.h>) does NOT work with GCC 8 cross-compiler
 * and --sysroot.  Instead, check the include guard that PreRuntime.h may have
 * already pulled in.  If not yet included, try including it directly; if the
 * compiler/sysroot doesn't have it, the fallback structs below will be used.
 */
#ifndef _UAPI__DRPAI_H
#  include <linux/drpai.h>
#endif

/*
 * If _UAPI__DRPAI_H is now defined, the system header was found.
 * Otherwise fall back to minimal local definitions.
 */
#ifndef _UAPI__DRPAI_H

/* ── Fallback: Minimal DRP-AI kernel interface (RZ/V2H BSP) ───────── */

#define DRPAI_IO_TYPE               (46)
#define DRPAI_ASSIGN                _IOW (DRPAI_IO_TYPE, 0, drpai_data_t)
#define DRPAI_START                 _IOW (DRPAI_IO_TYPE, 1, drpai_data_t)
#define DRPAI_RESET                 _IO  (DRPAI_IO_TYPE, 2)
#define DRPAI_GET_STATUS            _IOR (DRPAI_IO_TYPE, 3, drpai_status_t)
#define DRPAI_ASSIGN_PARAM          _IOW (DRPAI_IO_TYPE, 6, drpai_assign_param_t)
#define DRPAI_GET_DRPAI_AREA        _IOR (DRPAI_IO_TYPE, 11, drpai_data_t)

#define DRPAI_INDEX_NUM             (10)
#define DRPAI_INDEX_INPUT           (0)
#define DRPAI_INDEX_DRP_DESC        (1)
#define DRPAI_INDEX_DRP_CFG         (2)
#define DRPAI_INDEX_DRP_PARAM       (3)
#define DRPAI_INDEX_AIMAC_DESC      (4)
#define DRPAI_INDEX_WEIGHT          (5)
#define DRPAI_INDEX_OUTPUT          (6)
#define DRPAI_INDEX_AIMAC_CMD       (7)
#define DRPAI_INDEX_AIMAC_PARAM_DESC (8)
#define DRPAI_INDEX_AIMAC_PARAM_CMD (9)

#define DRPAI_RESERVED_NUM          (32)
#define DRPAI_MAX_NODE_NAME         (256)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drpai_data {
    uint64_t address;
    uint64_t size;
} drpai_data_t;

typedef struct drpai_status {
    uint32_t status;
    int32_t  err;
    uint32_t reserved[DRPAI_RESERVED_NUM];
} drpai_status_t;

typedef struct drpai_assign_param {
    uint32_t     info_size;
    drpai_data_t obj;
} drpai_assign_param_t;

#ifdef __cplusplus
}
#endif

#endif /* _UAPI__DRPAI_H fallback */

/* ── Convenience constants ─────────────────────────────────────────── */

static constexpr const char* DRPAI_DEV      = "/dev/drpai0";
static constexpr int         DRPAI_TIMEOUT_MS  = 5000; // ms

/* ── Logging macros ────────────────────────────────────────────────── */

#ifndef DRPAI_LOG
#define DRPAI_LOG(fmt, ...) fprintf(stderr, "[DRP-AI] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef DRPAI_ERR
#define DRPAI_ERR(fmt, ...) fprintf(stderr, "[DRP-AI ERROR] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno))
#endif

#endif // AI_DRPAI_HELPERS_H
