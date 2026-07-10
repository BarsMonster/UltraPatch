/* Measurement-only Cortex-M0+ layout: no vectors, CRT, startup, or board code. */
ENTRY(rcv3_run)

MEMORY
{
	FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 256K
	RAM (rw)   : ORIGIN = 0x20000000, LENGTH = 64K
}

SECTIONS
{
	.text :
	{
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.glue_7 .glue_7t)
		*(.vfp11_veneer)
		*(.v4_bx)
	} > FLASH

	.data :
	{
		KEEP(*(.data .data.*))
	} > RAM AT > FLASH

	.bss (NOLOAD) :
	{
		KEEP(*(.bss .bss.*))
		KEEP(*(COMMON))
	} > RAM

	/DISCARD/ :
	{
		*(.iplt .rel.iplt .igot.plt)
		*(.comment .ARM.attributes)
		*(.debug*)
	}
}
