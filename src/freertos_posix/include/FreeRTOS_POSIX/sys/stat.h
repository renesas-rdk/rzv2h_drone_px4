/* Stub sys/stat.h for FreeRTOS POSIX compatibility */
#ifndef _FREERTOS_POSIX_SYS_STAT_H_
#define _FREERTOS_POSIX_SYS_STAT_H_

/* Include newlib's sys/stat.h for struct stat and stat() function */
#include_next <sys/stat.h>

/* lstat() stub - just alias to stat() since no symlinks in FreeRTOS */
#ifndef lstat
#define lstat stat
#endif

#endif /* _FREERTOS_POSIX_SYS_STAT_H_ */
