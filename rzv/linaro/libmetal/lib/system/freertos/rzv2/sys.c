/*
 * Copyright (c) 2014, Mentor Graphics Corporation
 * Copyright (c) 2016, Xilinx Inc. and Contributors. All rights reserved.
 * Copyright (c) 2017, eForce Co Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Xilinx nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * @file    rzv2/sys.c
 * @brief   machine specific system primitives implementation.
 */

#include <stdint.h>
#include "metal/io.h"
#include "metal/sys.h"
#include "bsp_api.h"

void sys_irq_restore_enable (unsigned int flags)
{
#if BSP_SUPPORT_CORE_CR8
R_BSP_IrqMaskLevelSet(flags);
#else
    vClearInterruptMask(flags);
#endif
}

unsigned int sys_irq_save_disable (void)
{
#if BSP_SUPPORT_CORE_CR8
    int32_t imask;
    imask = R_BSP_IrqMaskLevelGet();

    if (imask != 0)
    {
        R_BSP_IrqMaskLevelSet(0);
    }
    return imask;
#else
    return ulSetInterruptMask();
#endif
}

void metal_machine_cache_flush (void * addr, unsigned int len)
{
    /* Cortex-R8 (ARMv7-R): Clean data cache lines by MVA to PoC (DCCMVAC).
     * This writes back any dirty cache lines to DRAM so the CA55 (uncached UIO)
     * sees the data CR8 just wrote into the RPMsg vring shared memory. */
    if (!addr || len == 0) return;
    const unsigned int kCacheLineSize = 32U; /* Cortex-R8 D-cache line = 32 bytes */
    uintptr_t p = (uintptr_t)addr & ~((uintptr_t)(kCacheLineSize - 1U));
    uintptr_t end = (uintptr_t)addr + len;
    for (; p < end; p += kCacheLineSize) {
        /* MCR p15, 0, Rd, c7, c10, 1 = DCCMVAC: Clean data cache line by MVA to PoC */
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb" : : : "memory"); /* Data Synchronization Barrier */
}

void metal_machine_cache_invalidate (void * addr, unsigned int len)
{
    /* Cortex-R8 (ARMv7-R): Invalidate data cache lines by MVA to PoC (DCIMVAC).
     * This discards any cached copy so CR8 re-reads CA55's writes from DRAM. */
    if (!addr || len == 0) return;
    const unsigned int kCacheLineSize = 32U; /* Cortex-R8 D-cache line = 32 bytes */
    uintptr_t p = (uintptr_t)addr & ~((uintptr_t)(kCacheLineSize - 1U));
    uintptr_t end = (uintptr_t)addr + len;
    for (; p < end; p += kCacheLineSize) {
        /* MCR p15, 0, Rd, c7, c6, 1 = DCIMVAC: Invalidate data cache line by MVA to PoC */
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb" : : : "memory"); /* Data Synchronization Barrier */
}

/**
 * @brief poll function until some event happens
 */
void __attribute__((weak)) metal_generic_default_poll (void)
{
    __asm volatile ("wfi");
}

void * metal_machine_io_mem_map (void * va, metal_phys_addr_t pa, size_t size, unsigned int flags)
{
    (void) pa;
    (void) size;
    (void) flags;

    return va;
}
