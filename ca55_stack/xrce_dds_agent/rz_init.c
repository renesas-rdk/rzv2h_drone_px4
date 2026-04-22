/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RZ Remote Processor initialization wrapper
 * This provides a simple initialization interface for the RZV2H platform
 */

#include <stdio.h>
#include <stdlib.h>
#include <metal/sys.h>
#include <metal/device.h>
#include <metal/io.h>
#include <openamp/open_amp.h>
#include "platform_info.h"
#include "helper.h"

/* External initialization functions */
extern void init_global_vars(void);
extern void cleanup_global_vars(void);

/* Simple initialization function called from main */
int rz_rproc_init(void) {
    int ret = 0;
    
    printf("Initializing RZV2H platform for uXRCE-DDS Agent...\n");
    
    /* Initialize global variables first */
    init_global_vars();
    
    /* Initialize metal library */
    struct metal_init_params metal_param = METAL_INIT_DEFAULTS;
    ret = metal_init(&metal_param);
    if (ret) {
        printf("ERROR: Failed to initialize libmetal: %d\n", ret);
        return ret;
    }
    printf("Libmetal initialized successfully\n");
    
    /* TODO: Add platform-specific initialization here */
    /* This could include:
     * - OpenAMP initialization
     * - RPMsg setup
     * - Shared memory configuration
     * - Interrupt handling setup
     */
    
    printf("RZV2H platform initialization completed\n");
    return 0;
}

/* Cleanup function */
void rz_rproc_cleanup(void) {
    printf("Cleaning up RZV2H platform...\n");
    cleanup_global_vars();
    metal_finish();
    printf("RZV2H platform cleanup completed\n");
}
