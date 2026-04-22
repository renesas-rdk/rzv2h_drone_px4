/**
 * @file rzv_sdram_layout.h
 * @brief Shared SDRAM scratch/shared-memory layout for CR8 and CA55.
 */
#pragma once

#include <stdint.h>

#define RZV_SDRAM_CACHE_BASE             0x40800000u
#define RZV_SDRAM_CACHE_USABLE_END       0x41700000u
#define RZV_SDRAM_SCRATCH_START          0x41700000u
#define RZV_SDRAM_SCRATCH_END            0x41800000u
#define RZV_SDRAM_SCRATCH_BYTES          ( RZV_SDRAM_SCRATCH_END - RZV_SDRAM_SCRATCH_START )

#define RZV_SDRAM_CMDS_SIZE_ADDR         0x41700000u
#define RZV_SDRAM_CMDS_DATA_ADDR         0x41700008u
#define RZV_SDRAM_CMDS_REGION_END        0x41710000u
#define RZV_SDRAM_CMDS_MAX_PAYLOAD       ( RZV_SDRAM_CMDS_REGION_END - RZV_SDRAM_CMDS_DATA_ADDR )

#define RZV_SDRAM_PARAMS_SIZE_ADDR       0x41710000u
#define RZV_SDRAM_PARAMS_DATA_ADDR       0x41710008u
#define RZV_SDRAM_PARAMS_REGION_END      0x41720000u
#define RZV_SDRAM_PARAMS_MAX_PAYLOAD     ( RZV_SDRAM_PARAMS_REGION_END - RZV_SDRAM_PARAMS_DATA_ADDR )

#define RZV_SDRAM_CONFIG_SIZE_ADDR       0x41720000u
#define RZV_SDRAM_CONFIG_DATA_ADDR       0x41720008u
#define RZV_SDRAM_CONFIG_REGION_END      0x41730000u
#define RZV_SDRAM_CONFIG_MAX_PAYLOAD     ( RZV_SDRAM_CONFIG_REGION_END - RZV_SDRAM_CONFIG_DATA_ADDR )

#define RZV_SDRAM_EXTRAS_SIZE_ADDR       0x41730000u
#define RZV_SDRAM_EXTRAS_DATA_ADDR       0x41730008u
#define RZV_SDRAM_EXTRAS_REGION_END      0x41740000u
#define RZV_SDRAM_EXTRAS_MAX_PAYLOAD     ( RZV_SDRAM_EXTRAS_REGION_END - RZV_SDRAM_EXTRAS_DATA_ADDR )

#define RZV_SDRAM_LOGGER_SHM_ADDR        0x41740000u
#define RZV_SDRAM_LOGGER_SHM_BYTES       0x00080000u
#define RZV_SDRAM_LOGGER_STREAM_COUNT    2u
#define RZV_SDRAM_LOGGER_STREAM_BYTES    ( RZV_SDRAM_LOGGER_SHM_BYTES / RZV_SDRAM_LOGGER_STREAM_COUNT )
#define RZV_SDRAM_LOGGER_CTRL_BYTES      32u
#define RZV_SDRAM_LOGGER_BUF_SIZE        ( RZV_SDRAM_LOGGER_STREAM_BYTES - RZV_SDRAM_LOGGER_CTRL_BYTES )
#define RZV_SDRAM_LOGGER_SHM_END         ( RZV_SDRAM_LOGGER_SHM_ADDR + RZV_SDRAM_LOGGER_SHM_BYTES )

#ifdef __cplusplus
static_assert( RZV_SDRAM_CMDS_DATA_ADDR >= RZV_SDRAM_CMDS_SIZE_ADDR + sizeof( uint32_t ),
               "cmd scratch header/data layout invalid" );
static_assert( RZV_SDRAM_CMDS_REGION_END <= RZV_SDRAM_PARAMS_SIZE_ADDR,
               "cmd scratch overlaps params scratch" );
static_assert( RZV_SDRAM_PARAMS_REGION_END <= RZV_SDRAM_CONFIG_SIZE_ADDR,
               "params scratch overlaps config scratch" );
static_assert( RZV_SDRAM_CONFIG_REGION_END <= RZV_SDRAM_EXTRAS_SIZE_ADDR,
               "config scratch overlaps extras scratch" );
static_assert( RZV_SDRAM_EXTRAS_REGION_END <= RZV_SDRAM_LOGGER_SHM_ADDR,
               "extras scratch overlaps logger shm" );
static_assert( RZV_SDRAM_LOGGER_SHM_END <= RZV_SDRAM_SCRATCH_END,
               "logger shm exceeds scratch window" );
static_assert( ( RZV_SDRAM_LOGGER_SHM_BYTES % RZV_SDRAM_LOGGER_STREAM_COUNT ) == 0u,
               "logger shm stream split must be even" );
#else
_Static_assert( RZV_SDRAM_CMDS_DATA_ADDR >= RZV_SDRAM_CMDS_SIZE_ADDR + sizeof( uint32_t ),
                "cmd scratch header/data layout invalid" );
_Static_assert( RZV_SDRAM_CMDS_REGION_END <= RZV_SDRAM_PARAMS_SIZE_ADDR,
                "cmd scratch overlaps params scratch" );
_Static_assert( RZV_SDRAM_PARAMS_REGION_END <= RZV_SDRAM_CONFIG_SIZE_ADDR,
                "params scratch overlaps config scratch" );
_Static_assert( RZV_SDRAM_CONFIG_REGION_END <= RZV_SDRAM_EXTRAS_SIZE_ADDR,
                "config scratch overlaps extras scratch" );
_Static_assert( RZV_SDRAM_EXTRAS_REGION_END <= RZV_SDRAM_LOGGER_SHM_ADDR,
                "extras scratch overlaps logger shm" );
_Static_assert( RZV_SDRAM_LOGGER_SHM_END <= RZV_SDRAM_SCRATCH_END,
                "logger shm exceeds scratch window" );
_Static_assert( ( RZV_SDRAM_LOGGER_SHM_BYTES % RZV_SDRAM_LOGGER_STREAM_COUNT ) == 0u,
                "logger shm stream split must be even" );
#endif
