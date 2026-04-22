/* Stub sys/file.h for FreeRTOS POSIX compatibility */
#ifndef _FREERTOS_POSIX_SYS_FILE_H_
#define _FREERTOS_POSIX_SYS_FILE_H_

/* Include stdint for integer types */
#include <stdint.h>

/* Try to include FreeRTOS POSIX types, but provide fallbacks */
#ifdef __has_include
    #if __has_include("FreeRTOS_POSIX/sys/types.h")
        #include "FreeRTOS_POSIX/sys/types.h"
    #endif
#endif

/* Fallback definitions if types.h didn't provide them */
#ifndef _OFF_T_DECLARED
    typedef long int off_t;
    #define _OFF_T_DECLARED
#endif

#ifndef _PID_T_DECLARED
    typedef int pid_t;
    #define _PID_T_DECLARED
#endif

/* flock() operations - stubs for PX4 compatibility */
#define LOCK_SH 1    /* Shared lock */
#define LOCK_EX 2    /* Exclusive lock */
#define LOCK_NB 4    /* Non-blocking */
#define LOCK_UN 8    /* Unlock */

/* fcntl() lock types for struct flock */
#define F_RDLCK 0    /* Read lock */
#define F_WRLCK 1    /* Write lock */
#define F_UNLCK 2    /* Unlock */

/* fcntl() commands for file locking */
#define F_GETLK 5    /* Get record locking information */
#define F_SETLK 6    /* Set record locking information */
#define F_SETLKW 7   /* Set record locking information; wait if blocked */

/* struct flock - file locking structure */
struct flock {
    short l_type;    /* Type of lock: F_RDLCK, F_WRLCK, F_UNLCK */
    short l_whence;  /* How to interpret l_start: SEEK_SET, SEEK_CUR, SEEK_END */
    off_t l_start;   /* Starting offset for lock */
    off_t l_len;     /* Number of bytes to lock (0 = EOF) */
    pid_t l_pid;     /* PID of process blocking our lock (F_GETLK only) */
};

/* flock() stub - always returns 0 (success) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
static inline int flock(int fd __attribute__((unused)), int operation __attribute__((unused)))
{
    return 0; /* Stub: no-op, always succeeds */
}
#pragma GCC diagnostic pop

#endif /* _FREERTOS_POSIX_SYS_FILE_H_ */
