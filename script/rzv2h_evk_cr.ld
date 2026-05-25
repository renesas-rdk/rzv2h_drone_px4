/*
                  Linker File for Renesas FSP
*/

INCLUDE memory_regions.ld

ITCM_PRV_START          = DEFINED(CR8_0) ? SRAM_CR8_0_START : SRAM_CR8_1_START;
ITCM_PRV_LENGTH         = 0x0001FE00;
DTCM_PRV_START          = DEFINED(CR8_0) ? SRAM_CR8_0_START + 0x00020000 : SRAM_CR8_1_START + 0x00020000;
DTCM_PRV_LENGTH         = 0x00020000;
ITCM_PRV_AXI_START      = DEFINED(CR8_0) ? 0x12040000 : 0x12080000;
SRAM_CACHE_PRV_START    = DEFINED(CR8_0) ? SRAM_CR8_0_START + 0x08180000 : SRAM_CR8_1_START + 0x081C0000;
SRAM_CACHE_PRV_LENGTH   = DEFINED(CR8_0) ? 0x00020000 : 0x00020000;
SRAM_UNCACHE_PRV_START  = DEFINED(CR8_0) ? SRAM_CR8_0_START + 0x081A0000 : SRAM_CR8_1_START + 0x081E0000;
SRAM_UNCACHE_PRV_LENGTH = DEFINED(CR8_0) ? 0x0001FFFC : 0x0001FFFC;
XSPIFLASH_PRV_START     = DEFINED(CR8_0) ? XSPIFLASH_CR8_0_START : XSPIFLASH_CR8_1_START;
XSPIFLASH_PRV_LENGTH    = DEFINED(CR8_0) ? XSPIFLASH_CR8_0_LENGTH : XSPIFLASH_CR8_1_LENGTH;
DDR_CACHE_PRV_START     = DEFINED(CR8_0) ? DDR0_CR8_0_START + 0x00800000 : DDR0_CR8_1_START + 0x01800000;
DDR_CACHE_LENGTH        = DEFINED(CR8_0) ? 0x00F00000 : 0x00F00000;
DDR_UNCACHE_PRV_START   = DEFINED(CR8_0) ? DDR0_CR8_0_START + 0x01800000 : DDR0_CR8_1_START + 0x02800000;
DDR_UNCACHE_LENGTH      = DEFINED(CR8_0) ? 0x00800000 : 0x00800000;
DDR_SCRATCH_START       = DDR_CACHE_PRV_START + DDR_CACHE_LENGTH;
DDR_SCRATCH_LENGTH      = 0x00100000;
DDR_SCRATCH_END         = DDR_SCRATCH_START + DDR_SCRATCH_LENGTH;

/* Memory allocation OpenAMP using ddr. */
OPENAMP_RSCTBL_START    = DEFINED(CR8_0) ? DDR0_CR8_0_START + 0x02f00000 : DDR0_CR8_1_START + 0x02f00000;
OPENAMP_RSCTBL_LENGTH   = 0x00001000;
MHU_SHMEM_START         = DEFINED(CR8_0) ? DDR0_CR8_0_START + 0x02f01000 : DDR0_CR8_1_START + 0x02f01000;
MHU_SHMEM_LENGTH        = 0x00001000;
OPENAMP_VRING_START     = DEFINED(CR8_0) ? DDR0_CR8_0_START + 0x03000000 : DDR0_CR8_1_START + 0x03000000;
OPENAMP_VRING_LENGTH    = 0x00F00000;

/* Flash header infomation */
ITCM_HEADER_START  = DEFINED(CR8_0) ? SRAM_CR8_0_START + 0x0001FE00 : SRAM_CR8_1_START + 0x0001FE00;
FLASH_HEADER_START = DEFINED(CR8_0) ? XSPIFLASH_CR8_0_START + 0x00200000 : XSPIFLASH_CR8_1_START + 0x01300000;
HEADER_LENGTH      = 0x80;
FLASH_DATA_START   = FLASH_HEADER_START + HEADER_LENGTH;

/* Initialization waiting area */
INIT_WAIT_AREA_START = DEFINED(CR8_0) ? SRAM_CR8_0_START + 0x081BFFFC : SRAM_CR8_1_START + 0x081FFFFC;
INIT_WAIT_AREA_OTHER_CORE_START = DEFINED(CR8_0) ? SRAM_CR8_0_START + 0x081FFFFC : SRAM_CR8_1_START + 0x081BFFFC;
INIT_WAIT_AREA_LENGTH = 0x00000004;

/* Dummy area to provide a space between the code area and data area */
DUMMY_START = 0x0001FDF0;
DUMMY_LENGTH = 0x00000008;

MEMORY
{
    ITCM : ORIGIN = ITCM_PRV_START, LENGTH = ITCM_PRV_LENGTH
    DTCM : ORIGIN = DTCM_PRV_START, LENGTH = DTCM_PRV_LENGTH
    SRAM_CACHE : ORIGIN = SRAM_CACHE_PRV_START, LENGTH = SRAM_CACHE_PRV_LENGTH
    SRAM_UNCACHE : ORIGIN = SRAM_UNCACHE_PRV_START, LENGTH = SRAM_UNCACHE_PRV_LENGTH
    XSPI_FLASH : ORIGIN = XSPIFLASH_PRV_START, LENGTH = XSPIFLASH_PRV_LENGTH
    DDR_SDRAM_CACHE : ORIGIN = DDR_CACHE_PRV_START, LENGTH = DDR_CACHE_LENGTH
    DDR_SDRAM_UNCACHE : ORIGIN = DDR_UNCACHE_PRV_START, LENGTH = DDR_UNCACHE_LENGTH

    OPENAMP_RSCTBL(rw): ORIGIN = OPENAMP_RSCTBL_START,LENGTH = OPENAMP_RSCTBL_LENGTH
    MHU_SHMEM(rw)     : ORIGIN = MHU_SHMEM_START,     LENGTH = MHU_SHMEM_LENGTH
    OPENAMP_VRING(rw) : ORIGIN = OPENAMP_VRING_START, LENGTH = OPENAMP_VRING_LENGTH

    HEADER_REGION(r) : ORIGIN = ITCM_HEADER_START, LENGTH = HEADER_LENGTH

    INIT_WAIT_AREA(rw) : ORIGIN = INIT_WAIT_AREA_START, LENGTH = INIT_WAIT_AREA_LENGTH
    INIT_WAIT_AREA_OTHER_CORE(rw) : ORIGIN = INIT_WAIT_AREA_OTHER_CORE_START, LENGTH = INIT_WAIT_AREA_LENGTH

    DUMMY : ORIGIN = DUMMY_START, LENGTH = DUMMY_LENGTH
}

PHDRS
{
    itcm_load_ph     PT_LOAD FLAGS(5); /* R+X */
    itcm_data_ph     PT_LOAD FLAGS(6); /* RW */
    dtcm_ph          PT_LOAD FLAGS(6); /* RW */
    sram_ph          PT_LOAD FLAGS(6); /* RW */
    sram_data_ph     PT_LOAD FLAGS(6); /* RW */  
    sdram_text_ph    PT_LOAD FLAGS(5); /* R+X */
    sdram_data_ph    PT_LOAD FLAGS(6); /* RW */
    openamp_ph       PT_LOAD FLAGS(6); /* RW */
    wait_ph          PT_LOAD FLAGS(6); /* RW */
    dummy_ph         PT_LOAD FLAGS(6); /* RW */
}

ENTRY(Reset_Handler)
SECTIONS
{
    .itcm_load_section0 0x00000000 :
    {
        _itcm_load_start = .;
        /* vector */
        _fvector_start = .;
        KEEP(*(.intvec))
        _fvector_end = .;

        . = ALIGN(0x100);

        /* BSP code */
        _text_start = .;

        KEEP(*(.int_handler))

        ./rzv_gen/*.obj(.text*)
        ./rzv/fsp/*.obj(.text*)
        ./rzv/board/*.obj(.text*)

        KEEP(*(.init))
        KEEP(*(.fini))

        /* .ctors */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        _ctor_end = .;

        /* .dtors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)
        _dtor_end = .;

        KEEP(*(.warm_start))

        /* Skip ARM exception tables - causes relocation overflow with split memory layout */
        /* KEEP(*(.eh_frame*))

        . = ALIGN(0x04);
        *(.ARM.extab* .gnu.linkonce.armextab.*)

        . = ALIGN(0x04);
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .; */

        . = ALIGN(0x04);
        *(.got)
        *(.got.plt)
        
        /* Minimal .eh_frame section to satisfy C runtime references */
        . = ALIGN(0x04);
        __EH_FRAME_BEGIN__ = .;
        KEEP(*(.eh_frame))
        __EH_FRAME_END__ = .;
        
        /* Include only critical system text that must stay in ITCM - be selective */
        ./rzv_gen/*.obj (.text .text.*)
        ./rzv/fsp/*.obj (.text .text.*)
        ./rzv/board/*.obj (.text .text.*)
        ./rzv/aws/FreeRTOS/*.obj (.text .text.*)
        ./rzv/linaro/*.obj (.text .text.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/SEGGER_RTT/*.obj (.text .text.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/hal_entry.c.obj (.text .text.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/sys*.obj (.text .text.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/mpu*.obj (.text .text.*)
        
        _text_end = .;
    } > ITCM : itcm_load_ph
    .loader_dummy_itcm :
    {
        _loader_dummy_itcm_start = .;
        KEEP(*(.loader_dummy_itcm));
        _loader_dummy_itcm_end = .;
    } > DUMMY AT > ITCM : dummy_ph
    .itcm_load_section1 :
    {
        . = ALIGN(0x10);
        /* BSP data */
        _data_start = .;

        *(vtable)
        KEEP(*(.application_vectors))
        ./rzv_gen/*.obj(.data*)
        ./rzv/fsp/*.obj(.data*)
        ./rzv/board/*.obj(.data*)

        ./rzv_gen/*.obj(.rodata*)
        ./rzv/fsp/*.obj(.rodata*)
        ./rzv/board/*.obj(.rodata*)
        _erodata = .;

        . = ALIGN(0x04);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(0x04);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(0x04);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN (__fini_array_end = .);

        KEEP(*(.jcr*))

        . = ALIGN(0x04);
        
        /* Include only critical system data that must stay in ITCM - be selective */
        ./rzv_gen/*.obj (.data .data.*)
        ./rzv/fsp/*.obj (.data .data.*)
        ./rzv/board/*.obj (.data .data.*)
        ./rzv/aws/FreeRTOS/*.obj (.data .data.*)
        ./rzv/linaro/*.obj (.data .data.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/SEGGER_RTT/*.obj (.data .data.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/hal_entry.c.obj (.data .data.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/sys*.obj (.data .data.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/mpu*.obj (.data .data.*)
        
        ./rzv_gen/*.obj (.rodata .rodata.*)
        ./rzv/fsp/*.obj (.rodata .rodata.*)
        ./rzv/board/*.obj (.rodata .rodata.*)
        ./rzv/aws/FreeRTOS/*.obj (.rodata .rodata.*)
        ./rzv/linaro/*.obj (.rodata .rodata.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/SEGGER_RTT/*.obj (.rodata .rodata.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/hal_entry.c.obj (.rodata .rodata.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/sys*.obj (.rodata .rodata.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/mpu*.obj (.rodata .rodata.*)

        . = ALIGN(0x04);

        /* All data end */
        _data_end = .;
        _itcm_load_end = .;
    } > ITCM : itcm_data_ph
    .dtcm_noload_section (NOLOAD) :
    {
        /* system stack */
        . = ALIGN(8);
        __SysStackBase = .;
        /* Place the sys_stack here. */
        KEEP(*(.sys_stack))
        __SysStackLimit = .;

        /* SVC stack */
        . = ALIGN(8);
        __SvcStackBase = .;
        /* Place the svc_stack here. */
        KEEP(*(.svc_stack))
        __SvcStackLimit = .;

        /* IRQ stack */
        . = ALIGN(8);
        __IrqStackBase = .;
        /* Place the irq_stack here. */
        KEEP(*(.irq_stack))
        __IrqStackLimit = .;

        /* FIQ stack */
        . = ALIGN(8);
        __FiqStackBase = .;
        /* Place the fiq_stack here. */
        KEEP(*(.fiq_stack))
        __FiqStackLimit = .;

        /* Undefined stack */
        . = ALIGN(8);
        __UndStackBase = .;
        /* Place the und_stack here. */
        KEEP(*(.und_stack))
        __UndStackLimit = .;

        /* Abort stack */
        . = ALIGN(8);
        __AbtStackBase = .;
        /* Place the abt_stack here. */
        KEEP(*(.abt_stack))
        __AbtStackLimit = .;

        /* BSP BSS */
        . = ALIGN(0x04);
        __bss_start__ = .;
        _bss = .;
        ./rzv_gen/*.obj (.bss*)
        ./rzv/fsp/*.obj (.bss*)
        ./rzv/board/*.obj (.bss*)
        
        /* Include only critical system BSS that must stay in DTCM - be selective */
        ./rzv_gen/*.obj (.bss .bss.*)
        ./rzv/fsp/*.obj (.bss .bss.*)
        ./rzv/board/*.obj (.bss .bss.*)
        ./rzv/aws/FreeRTOS/*.obj (.bss .bss.*)
        ./rzv/linaro/*.obj (.bss .bss.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/SEGGER_RTT/*.obj (.bss .bss.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/hal_entry.c.obj (.bss .bss.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/sys*.obj (.bss .bss.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/mpu*.obj (.bss .bss.*)
        
        *(COMMON)
        . = ALIGN(0x04);
        __bss_end__ = .;
        _ebss = .;
        _end = .;
    } > DTCM : dtcm_ph
    
    /* FreeRTOS heap and dynamically-created task stacks placed in DDR */
    .rtos_heap_section (NOLOAD) : ALIGN(8)
    {
        /* Retain any BSP-provided heap buffers (e.g., g_heap) ahead of the RTOS heap. */
        KEEP(*(.heap))
        KEEP(*(.heap.*))
        KEEP(*(.bss.heap))
        KEEP(*(.bss.heap.*))

        . = ALIGN(8);
        __HeapBase = .;

        /* Actual FreeRTOS heap buffers (ucHeap).
         * port.c defines ucHeap without a section attribute so it lands in
         * .bss.ucHeap (via -fdata-sections).  Accept both the attributed and
         * the plain BSS placement so the 8 MB assertion always passes. */
        KEEP(*(.freertos_heap))
        KEEP(*(.freertos_heap.*))
        KEEP(*(.bss.ucHeap))

        . = ALIGN(8);
        __HeapLimit = .;

        /* Verify reservation at build time */
        ASSERT((__HeapLimit - __HeapBase) == 0x00800000,
               "ERROR: FreeRTOS heap must be exactly 8MB");

        . = ALIGN(8);
        __RTOSStackBase = .;
        KEEP(*(.stack.*))
        . = ALIGN(8);
        __RTOSStackLimit = .;
    } > DDR_SDRAM_CACHE : sdram_data_ph
    /* Separate executable code section in DDR */
    .sdram_text_section :  ALIGN( 0x04 )
    {
        . = ALIGN ( 0x04 ) ;
        _sdram_text_start = .;
        
        /* Only user PX4 code - exclude critical system code */
        __text_usr_start = .;
        ./px4/CMakeFiles/px4.dir/**/*.obj (.text .text.*)  /* Only PX4 specific objects */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/px4*.obj (.text .text.*)  /* PX4 related user code */
        
        /* Include other non-critical user code but exclude BSP/system */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/main_task_entry.c.obj (.text .text.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/FreeRTOS-Plus/FreeRTOS-Plus-POSIX/source/*.obj (.text .text.*)
        
        /* Catch-all for any remaining .text sections not explicitly placed in ITCM */
        *(.text .text.* .gnu.linkonce.t.*)
        
        /* Catch-all for other code sections that might not be in ITCM */
        *(.init)
        *(.fini) 
        *(.glue_7)
        *(.glue_7t)
        *(.vfp11_veneer)
        *(.v4_bx)
        
        . = ALIGN( 0x04 );
        __text_usr_end = .;
        
        . = ALIGN(0x04);
        _sdram_text_end = .;
    } > DDR_SDRAM_CACHE : sdram_text_ph
    
    /* Separate data section in DDR */
    .sdram_data_section :  ALIGN( 0x04 )
    {
        . = ALIGN ( 0x04 ) ;
        _sdram_data_start = .;
        ./rzv/linaro/*.o(.data*)
        
        /* Only user PX4 data - exclude critical system data */
        __data_usr_start = .;
        ./px4/CMakeFiles/px4.dir/**/*.obj (.data .data.*)  /* Only PX4 specific objects */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/px4*.obj (.data .data.*)  /* PX4 related user code */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/main_task_entry.c.obj (.data .data.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/FreeRTOS-Plus/FreeRTOS-Plus-POSIX/source/*.obj (.data .data.*)

        /* Catch-all for any remaining .data sections not explicitly placed in ITCM */
        *(.data .data.* .gnu.linkonce.d.*)

        . = ALIGN(0x04);

        __rodata_usr_start = .;
        ./px4/CMakeFiles/px4.dir/**/*.obj (.rodata .rodata.*) /* Only PX4 specific objects */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/px4*.obj (.rodata .rodata.*)  /* PX4 related user code */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/main_task_entry.c.obj (.rodata .rodata.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/FreeRTOS-Plus/FreeRTOS-Plus-POSIX/source/*.obj (.rodata .rodata.*)

        /* Catch-all for any remaining .rodata sections not explicitly placed in ITCM */
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        
        /* Catch-all for other data sections */
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        *(.sdata2 .sdata2.* .gnu.linkonce.s2.*)
        *(.got .got.plt)
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        
        /* Catch-all for constructor/destructor tables not in ITCM */
        *(SORT(.ctors.*))
        *(.ctors)
        *(SORT(.dtors.*))
        *(.dtors)
        
        /* Array initialization sections */
        *(.preinit_array)
        *(SORT(.init_array.*))
        *(.init_array)
        *(SORT(.fini_array.*))
        *(.fini_array)
        *(.jcr*)

        . = ALIGN(0x04);
        /* All data end */
        __data_usr_end = .;
        
        . = ALIGN(0x04);
        _sdram_data_end = .;
    } > DDR_SDRAM_CACHE : sdram_data_ph
    .sram_load_section0 : ALIGN( 0x04 )
    {
        /* User code moved to DDR - keep section for compatibility */
        _sram_load_start = .;
        . = ALIGN( 0x04 );
        __text_usr_end = .;
    } > SRAM_CACHE
    .loader_dummy_sram :
    {
        _loader_dummy_sram_start = .;
        KEEP(*(.loader_dummy_sram));
        _loader_dummy_sram_end = .;
    } > DUMMY AT > SRAM_CACHE : dummy_ph
    .sram_load_section1 : ALIGN( 0x04 )
    {
        /* user data moved to DDR - keep section for compatibility */
        . = ALIGN(0x04);
        _sram_load_end = .;
    } > SRAM_CACHE : sram_ph
    .sram_noload_section (NOLOAD) :
    {
        /* user BSS moved to DDR - keep section for compatibility */
        . = ALIGN(0x10);
        _cbuffer_start = .;
        KEEP(*(.cache_buffer*))
        _cbuffer_end = .;
    } > SRAM_CACHE
    .sram_uncache_section (NOLOAD) :
    {
        . = ALIGN(0x10);
        _ncbuffer_start = .;
        KEEP(*(.noncache_buffer*))
        _ncbuffer_end = .;
    } > SRAM_UNCACHE
    .wait_data  :
    {
        KEEP(*(.wait_data))
    } > INIT_WAIT_AREA : wait_ph
    .wait_data_other_core  :
    {
        KEEP(*(.wait_data_other_core))
    } > INIT_WAIT_AREA_OTHER_CORE : wait_ph

    .noinit (NOLOAD) :
    {
        *(.noinit)
        *(.noinit.*)
    } > SRAM_UNCACHE : sram_data_ph

    .sdram_noload_section (NOLOAD) :
    {
        . = ALIGN ( 0x10 ) ;
        PROVIDE(_cbuffer_sdram_start = .);
        
        /* Only user PX4 BSS - exclude critical system BSS */
        . = ALIGN( 0x04 );
        __bss_usr_start__ = .;

        ./px4/CMakeFiles/px4.dir/**/*.obj (.bss .bss.*)  /* Only PX4 specific objects */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/px4*.obj (.bss .bss.*)  /* PX4 related user code */
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/runtime/main_task_entry.c.obj (.bss .bss.*)
        CMakeFiles/rzv2h_px4_freertos.elf.dir/src/FreeRTOS-Plus/FreeRTOS-Plus-POSIX/source/*.obj (.bss .bss.*)

        /* Catch-all for any remaining .bss sections not explicitly placed in DTCM */
        *(.bss .bss.* .gnu.linkonce.b.*)
        
        /* Catch-all for other uninitialized data sections */
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.sbss2 .sbss2.* .gnu.linkonce.sb2.*)
        *(COMMON)

        . = ALIGN(0x04);
        __bss_usr_end__ = .;
        _end_usr = .;

        . = ALIGN(0x10);
        /* Use separate symbol for DDR cache buffer to avoid conflict with SRAM symbols */
        _cbuffer_ddr_start = .;
        KEEP(*(.cache_buffer*))
        _cbuffer_ddr_end = .;
        PROVIDE(_cbuffer_sdram_end = .);
    } > DDR_SDRAM_CACHE
    .sdram_uncache_section (NOLOAD) :
    {
        . = ALIGN ( 0x10 ) ;
        PROVIDE(_ncbuffer_sdram_start = .);
        *(.mutex_flags);
        KEEP(*(.sdram_uncache_section))
        KEEP(*(.sdram_uncache_section.*))
        PROVIDE(_ncbuffer_sdram_end = .);
    } > DDR_SDRAM_UNCACHE

    __tz_MHU_SHMEM = ABSOLUTE ( MHU_SHMEM_START ) ;
    . = __tz_MHU_SHMEM;
    .resource_table (NOLOAD) :
    {
        PROVIDE(__rsctbl_start = .);
        *  (  .resource_table  )
        PROVIDE(__rsctbl_end = .);
    } > OPENAMP_RSCTBL : openamp_ph
    __tz_MHU_SHMEM = ABSOLUTE ( MHU_SHMEM_START ) ;
    . = __tz_MHU_SHMEM;
    .mhu_shmem (NOLOAD) :
    {
        PROVIDE(__mhu_shmem_start = .);
        . += MHU_SHMEM_LENGTH;
        PROVIDE(__mhu_shmem_end = .);
    } > MHU_SHMEM : openamp_ph
    __tz_OPENAMP_VRING = ABSOLUTE ( OPENAMP_VRING_START ) ;
    . = __tz_OPENAMP_VRING;
    .vring (NOLOAD) :
    {
        PROVIDE(__vring_start = .);
        . += OPENAMP_VRING_LENGTH;
        PROVIDE(__vring_end = .);
    } > OPENAMP_VRING : openamp_ph

    _itcm_load_addr = FLASH_DATA_START;
    _itcm_load_size = _itcm_load_end - _itcm_load_start;
    _itcm_load_start = _itcm_load_start + ITCM_PRV_AXI_START;
    _sram_load_addr = FLASH_DATA_START + _itcm_load_size;
    _sram_load_size = _sram_load_end - _sram_load_start;
    _sdram_load_addr = FLASH_DATA_START + _itcm_load_size + _sram_load_size;
    _sdram_load_size = (_sdram_text_end - _sdram_text_start) + (_sdram_data_end - _sdram_data_start);
    _sdram_load_start = _sdram_text_start;
    .header : {
        _header_start = .;
        LONG(_itcm_load_addr)
        LONG(_itcm_load_start)
        LONG(_itcm_load_size)
        LONG(0xFFFFFFFF - _itcm_load_addr)
        LONG(0xFFFFFFFF - _itcm_load_start)
        LONG(0xFFFFFFFF - _itcm_load_size)
        LONG(_sram_load_addr)
        LONG(_sram_load_start)
        LONG(_sram_load_size)
        LONG(0xFFFFFFFF - _sram_load_addr)
        LONG(0xFFFFFFFF - _sram_load_start)
        LONG(0xFFFFFFFF - _sram_load_size)
        LONG(_sdram_load_addr)
        LONG(_sdram_load_start)
        LONG(_sdram_load_size)
        LONG(0xFFFFFFFF - _sdram_load_addr)
        LONG(0xFFFFFFFF - _sdram_load_start)
        LONG(0xFFFFFFFF - _sdram_load_size)
        LONG(INIT_WAIT_AREA_START)
        LONG(0x1234FEDC)
        _header_end = .;
    } > HEADER_REGION
    /* Header reserved area (signature, padding) */
    _header_padding_size = HEADER_LENGTH - ((_header_end + 4)- _header_start);
    .header_reserved : {
        LONG(0x562F5A52)
        FILL(0x00)
        . += _header_padding_size;
    } > HEADER_REGION
    
    /* Discard ARM exception tables that cause relocation overflow with split memory layout */
    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.gnu.linkonce.armexidx.*)
        *(.gnu.linkonce.armextab.*)
    }
}

/* Provide required symbols for libgcc even though we don't use exception handling */
__exidx_start = 0;
__exidx_end = 0;

ASSERT( ( DDR_SCRATCH_START == 0x41700000 ), "DDR scratch window start mismatch" )
ASSERT( ( DDR_SCRATCH_END == 0x41800000 ), "DDR scratch window end mismatch" )
ASSERT( _sdram_text_end <= DDR_SCRATCH_START, "DDR SDRAM text overlaps reserved scratch window" )
ASSERT( _sdram_data_end <= DDR_SCRATCH_START, "DDR SDRAM data overlaps reserved scratch window" )
ASSERT( _cbuffer_ddr_end <= DDR_SCRATCH_START, "DDR SDRAM bss/cache overlaps reserved scratch window" )
