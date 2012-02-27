/* -----------------------------------------------------------------------
 *
 *   Copyright 2012 Intel Corporation; author Matt Fleming
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2 or (at your
 *   option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

pe_header:
	.ascii	"PE"
	.word 	0

coff_header:
#ifdef CONFIG_X86_32
	.word	0x14c				# i386
#else
	.word	0x8664				# x86-64
#endif
	.word	2				# nr_sections
	.long	0 				# TimeDateStamp
	.long	0				# PointerToSymbolTable
	.long	1				# NumberOfSymbols
	.word	section_table - optional_header	# SizeOfOptionalHeader
#ifdef CONFIG_X86_32
	.word	0x306				# Characteristics.
						# IMAGE_FILE_32BIT_MACHINE |
						# IMAGE_FILE_DEBUG_STRIPPED |
						# IMAGE_FILE_EXECUTABLE_IMAGE |
						# IMAGE_FILE_LINE_NUMS_STRIPPED
#else
	.word	0x206				# Characteristics
						# IMAGE_FILE_DEBUG_STRIPPED |
						# IMAGE_FILE_EXECUTABLE_IMAGE |
						# IMAGE_FILE_LINE_NUMS_STRIPPED
#endif

optional_header:
#ifdef CONFIG_X86_32
	.word	0x10b				# PE32 format
#else
	.word	0x20b 				# PE32+ format
#endif
	.byte	0x02				# MajorLinkerVersion
	.byte	0x14				# MinorLinkerVersion

	# Filled in by build.c
	.long	0				# SizeOfCode

	.long	0				# SizeOfInitializedData
	.long	0				# SizeOfUninitializedData

	# Filled in by build.c
	.long	0x0000				# AddressOfEntryPoint

	.long	0x0200				# BaseOfCode
#ifdef CONFIG_X86_32
	.long	0				# data
#endif

extra_header_fields:
#ifdef CONFIG_X86_32
	.long	0				# ImageBase
#else
	.quad	0				# ImageBase
#endif
	.long	0x1000				# SectionAlignment
	.long	0x200				# FileAlignment
	.word	0				# MajorOperatingSystemVersion
	.word	0				# MinorOperatingSystemVersion
	.word	0				# MajorImageVersion
	.word	0				# MinorImageVersion
	.word	0				# MajorSubsystemVersion
	.word	0				# MinorSubsystemVersion
	.long	0				# Win32VersionValue

	#
	# The size of the bzImage is written in tools/build.c
	#
	.long	0				# SizeOfImage

	.long	0x200				# SizeOfHeaders
	.long	0				# CheckSum
	.word	0xa				# Subsystem (EFI application)
	.word	0				# DllCharacteristics
#ifdef CONFIG_X86_32
	.long	0				# SizeOfStackReserve
	.long	0				# SizeOfStackCommit
	.long	0				# SizeOfHeapReserve
	.long	0				# SizeOfHeapCommit
#else
	.quad	0				# SizeOfStackReserve
	.quad	0				# SizeOfStackCommit
	.quad	0				# SizeOfHeapReserve
	.quad	0				# SizeOfHeapCommit
#endif
	.long	0				# LoaderFlags
	.long	0x6				# NumberOfRvaAndSizes

	.quad	0				# ExportTable
	.quad	0				# ImportTable
	.quad	0				# ResourceTable
	.quad	0				# ExceptionTable
	.quad	0				# CertificationTable
	.quad 	0				# BaseRelocationTable

	# Section table
section_table:
	.ascii	".text"
	.byte	0
	.byte	0
	.byte	0
	.long	0
	.long	0x0				# startup_{32,64}
	.long	0				# Size of initialized data
						# on disk
	.long	0x0				# startup_{32,64}
	.long	0				# PointerToRelocations
	.long	0				# PointerToLineNumbers
	.word	0				# NumberOfRelocations
	.word	0				# NumberOfLineNumbers
	.long	0x60500020			# Characteristics (section flags)

	#
	# The EFI application loader requires a relocation section
	# because EFI applications are relocatable and not having
	# this section seems to confuse it. But since we don't need
	# the loader to fixup any relocs for us just fill it with a
	# single dummy reloc.
	#
	.ascii	".reloc"
	.byte	0
	.byte	0
	.long	reloc_end - reloc_start
	.long	reloc_start
	.long	reloc_end - reloc_start		# SizeOfRawData
	.long	reloc_start			# PointerToRawData
	.long	0				# PointerToRelocations
	.long	0				# PointerToLineNumbers
	.word	0				# NumberOfRelocations
	.word	0				# NumberOfLineNumbers
	.long	0x42100040			# Characteristics (section flags)

	.section .reloc
reloc_start:
	.long	dummy - reloc_start
	.long	10
	.word	0
reloc_end:
