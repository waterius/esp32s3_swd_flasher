/* Раскладка нейтральной прошивки: flash с 0x08000000, ОЗУ 3 КБ с 0x20000000.
   Flash здесь 32 КБ, а не 24: так линкер принимает секцию .tail на 0x08005F00 (сборка с
   -DWITH_TAIL), которая не помещается в 24 КБ PY32F002B. Без WITH_TAIL секция пуста и в ELF
   не попадает. */
MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 32K
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 3K
}
ENTRY(reset_handler)
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) } > FLASH
  .rodata : { *(.rodata*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT > FLASH
  .bss : { *(.bss*) *(COMMON) } > RAM
  .tail 0x08005F00 : { KEEP(*(.tail)) } > FLASH
}
