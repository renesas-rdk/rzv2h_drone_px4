#pragma once

/*
 * newlib exposes stub POSIX thread types (e.g. pthread_mutex_t as a uint32_t)
 * from sys/_pthreadtypes.h.  Those collide with the FreeRTOS+POSIX shim, which
 * requires its own structs.  Pretend the header has already been processed so
 * we can provide the shim-friendly definitions instead.
 */
#ifndef _SYS__PTHREADTYPES_H_
#define _SYS__PTHREADTYPES_H_
#endif

#ifdef __need_inttypes
#undef __need_inttypes
#endif

#include_next <sys/types.h>

/* Pull in the FreeRTOS+POSIX typedefs after newlib so only missing symbols are
 * provided by the shim layer.
 */
#include "FreeRTOS_POSIX_portable.h"
#include "FreeRTOS_POSIX_portable_default.h"
#include "FreeRTOS_POSIX/sys/types.h"

/* Some newlib builds omit the caddr_t typedef unless sys/types.h is included
 * without the __need_inttypes shortcut.  Ensure it is available for BSP glue.
 */
#ifndef __caddr_t_defined
typedef char * caddr_t;
#define __caddr_t_defined
#endif
