echo "Post build script started"

if [ -n "$BuildArtifactFileName" ]; then
  inputfilename=$BuildArtifactFileName
fi
if [ -n "$BUILDARTIFACTFILENAME" ]; then
  inputfilename=$BUILDARTIFACTFILENAME
fi

filebody=${inputfilename%.*}

if [ -n "$BuildArtifactFileName" ]; then
  inputfilename=$BuildArtifactFileName
fi
if [ -n "$BUILDARTIFACTFILENAME" ]; then
  inputfilename=$BUILDARTIFACTFILENAME
fi

filebody=${inputfilename%.*}

if [ -d ../rzv/fsp/src/bsp/mcu/all/cm ]; then
  arm-none-eabi-objcopy -O binary -R .resource_table ${filebody}.elf ${filebody}.bin
  arm-none-eabi-objcopy -I binary -O srec --adjust-vma 0x60000000 --srec-forceS3 ${filebody}.bin ${filebody}.srec
else
  arm-none-eabi-objcopy -O binary -j .header -j .header_reserved ${inputfilename} ${filebody}_header.bin
  arm-none-eabi-objcopy -O binary -j .itcm_load_section0 -j .itcm_load_section1 ${inputfilename} ${filebody}_itcm.bin
  arm-none-eabi-objcopy -O binary -j .sram_load_section0 -j .sram_load_section1 ${inputfilename} ${filebody}_sram.bin
  arm-none-eabi-objcopy -O binary -j .sdram_text_section -j .sdram_data_section ${inputfilename} ${filebody}_sdram.bin

  cat ${filebody}_header.bin ${filebody}_itcm.bin ${filebody}_sram.bin ${filebody}_sdram.bin > ${filebody}.bin 2> /dev/null || true

  header_file="../rzv_cfg/fsp_cfg/bsp/bsp_mcu_family_cfg.h"
  core_definition=$(grep "BSP_CR8_CORE_NUM" "$header_file")
  core_num=$(echo "$core_definition" | cut -d ' ' -f 3)

  if [ "$core_num" = "0" ] || [ "$core_num" = "(0)" ]; then
    arm-none-eabi-objcopy -I binary -O srec --adjust-vma 0x60200000 --srec-forceS3 ${filebody}.bin ${filebody}.srec
  else
    arm-none-eabi-objcopy -I binary -O srec --adjust-vma 0x61300000 --srec-forceS3 ${filebody}.bin ${filebody}.srec
  fi
fi

echo "Post build script complete"
