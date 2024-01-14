/*
 *  Copyright (C) 2003-2005 Pontus Fuchs, Giridhar Pemmasani
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */

#ifdef TEST_LOADER

#include "usr_linker.h"

#else

#include <linux/types.h>
#include <asm/errno.h>

//#define DEBUGLINKER 2

#include "ntoskernel.h"

#endif

#include "nvmalloc.h"

struct pe_exports {
	char *dll;
	char *name;
	generic_func addr;
};

static struct pe_exports pe_exports[40];
static int num_pe_exports;

#define RVA2VA(image, rva, type) (type)(ULONG_PTR)((void *)image + rva)
#define CHECK_SZ(a,b) { if (sizeof(a) != b) {				\
			ERROR("%s is bad, got %zd, expected %d",	\
			      #a , sizeof(a), (b)); return -EINVAL; } }

#if defined(DEBUGLINKER) && DEBUGLINKER > 0
#define DBGLINKER(fmt, ...) printk(KERN_INFO "%s (%s:%d): " fmt "\n",	\
				   DRIVER_NAME, __func__,		\
				   __LINE__ , ## __VA_ARGS__);
static const char *image_directory_name[] = {
	"EXPORT",
	"IMPORT",
	"RESOURCE",
	"EXCEPTION",
	"SECURITY",
	"BASERELOC",
	"DEBUG",
	"COPYRIGHT",
	"GLOBALPTR",
	"TLS",
	"LOAD_CONFIG",
	"BOUND_IMPORT",
	"IAT",
	"DELAY_IMPORT",
	"COM_DESCRIPTOR" };
#else
#define DBGLINKER(fmt, ...) do { } while (0)
#endif

#ifndef TEST_LOADER
extern struct wrap_export ntoskernel_exports[], ntoskernel_io_exports[],
	ndis_exports[], crt_exports[], hal_exports[], rtl_exports[];
#ifdef ENABLE_USB
extern struct wrap_export usb_exports[];
#endif

static int get_export(char *name, generic_func *func)
{
	int i, j;

	struct wrap_export *exports[] = {
		ntoskernel_exports,
		ntoskernel_io_exports,
		ndis_exports,
		crt_exports,
		hal_exports,
		rtl_exports,
#ifdef ENABLE_USB
		usb_exports,
#endif
	};

	for (j = 0; j < ARRAY_SIZE(exports); j++)
		for (i = 0; exports[j][i].name != NULL; i++)
			if (strcmp(exports[j][i].name, name) == 0) {
				*func = exports[j][i].func;
				return 0;
			}

	for (i = 0; i < num_pe_exports; i++)
		if (strcmp(pe_exports[i].name, name) == 0) {
			*func = pe_exports[i].addr;
			return 0;
		}

	return -1;
}
#endif // TEST_LOADER

static void *get_dll_init(char *name)
{
	int i;
	for (i = 0; i < num_pe_exports; i++)
		if ((strcmp(pe_exports[i].dll, name) == 0) &&
		    (strcmp(pe_exports[i].name, "DllInitialize") == 0))
			return (void *)pe_exports[i].addr;
	return NULL;
}

/*
 * Find and validate the coff header
 *
 */
static int check_nt_hdr(IMAGE_NT_HEADERS *nt_hdr)
{
	int i;
	WORD attr;
	PIMAGE_OPTIONAL_HEADER opt_hdr;

	/* Validate the "PE\0\0" signature */
	if (nt_hdr->Signature != IMAGE_NT_SIGNATURE) {
		ERROR("is this driver file? bad signature %08x",
		      nt_hdr->Signature);
		return -EINVAL;
	}

	opt_hdr = &nt_hdr->OptionalHeader;
	/* Make sure Image is PE32 or PE32+ */
#ifdef CONFIG_X86_64
	if (opt_hdr->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		ERROR("kernel is 64-bit, but Windows driver is not 64-bit;"
		      "bad magic: %04X", opt_hdr->Magic);
		return -EINVAL;
	}
#else
	if (opt_hdr->Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		ERROR("kernel is 32-bit, but Windows driver is not 32-bit;"
		      "bad magic: %04X", opt_hdr->Magic);
		return -EINVAL;
	}
#endif

	/* Validate the image for the current architecture. */
#ifdef CONFIG_X86_64
	if (nt_hdr->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
		ERROR("kernel is 64-bit, but Windows driver is not 64-bit;"
		      " (PE signature is %04X)", nt_hdr->FileHeader.Machine);
		return -EINVAL;
	}
#else
	if (nt_hdr->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
		ERROR("kernel is 32-bit, but Windows driver is not 32-bit;"
		      " (PE signature is %04X)", nt_hdr->FileHeader.Machine);
		return -EINVAL;
	}
#endif

	/* Must have attributes */
#ifdef CONFIG_X86_64
	attr = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
#else
	attr = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
#endif
	if ((nt_hdr->FileHeader.Characteristics & attr) != attr)
		return -EINVAL;

	/* Must be relocatable */
	attr = IMAGE_FILE_RELOCS_STRIPPED;
	if ((nt_hdr->FileHeader.Characteristics & attr))
		return -EINVAL;

	/* Make sure we have at least one section */
	if (nt_hdr->FileHeader.NumberOfSections == 0)
		return -EINVAL;

	if (opt_hdr->SectionAlignment < opt_hdr->FileAlignment) {
		ERROR("alignment mismatch: section: 0x%x, file: 0x%x",
		      opt_hdr->SectionAlignment, opt_hdr->FileAlignment);
		return -EINVAL;
	}

	DBGLINKER("number of datadictionary entries %d",
		  opt_hdr->NumberOfRvaAndSizes);
	for (i = 0; i < opt_hdr->NumberOfRvaAndSizes; i++) {
		DBGLINKER("datadirectory %s RVA:%X Size:%d",
			  (i <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR) ?
			  image_directory_name[i] : "unknown",
			  opt_hdr->DataDirectory[i].VirtualAddress,
			  opt_hdr->DataDirectory[i].Size);
	}

	if ((nt_hdr->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
		return IMAGE_FILE_EXECUTABLE_IMAGE;
	if ((nt_hdr->FileHeader.Characteristics & IMAGE_FILE_DLL))
		return IMAGE_FILE_DLL;
	return -EINVAL;
}

static int import(void *image, IMAGE_IMPORT_DESCRIPTOR *dirent, char *dll)
{
	ULONG_PTR *lookup_tbl, *address_tbl;
	char *symname = NULL;
	int i;
	int ret = 0;
	generic_func adr;

	lookup_tbl = RVA2VA(image, dirent->u.OriginalFirstThunk, ULONG_PTR *);
	address_tbl = RVA2VA(image, dirent->FirstThunk, ULONG_PTR *);

	for (i = 0; lookup_tbl[i]; i++) {
		if (IMAGE_SNAP_BY_ORDINAL(lookup_tbl[i])) {
			ERROR("ordinal import not supported: %llu",
			      (uint64_t)lookup_tbl[i]);
			return -1;
		}
		else {
			symname = RVA2VA(image,
					 ((lookup_tbl[i] &
					   ~IMAGE_ORDINAL_FLAG) + 2), char *);
		}

		if (get_export(symname, &adr) < 0) {
			ERROR("unknown symbol: %s:'%s'", dll, symname);
			ret = -1;
		} else {
			DBGLINKER("found symbol: %s:%s: addr: %p, rva = %llu",
				  dll, symname, adr, (uint64_t)address_tbl[i]);
			address_tbl[i] = (ULONG_PTR)adr;
		}
	}
	return ret;
}

static int read_exports(struct pe_image *pe)
{
	IMAGE_EXPORT_DIRECTORY *export_dir_table;
	uint32_t *export_addr_table;
	int i;
	uint32_t *name_table;
	PIMAGE_OPTIONAL_HEADER opt_hdr;
	IMAGE_DATA_DIRECTORY *export_data_dir;

	opt_hdr = &pe->nt_hdr->OptionalHeader;
	export_data_dir =
		&opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

	if (export_data_dir->Size == 0) {
		DBGLINKER("no exports");
		return 0;
	}

	export_dir_table =
		RVA2VA(pe->image, export_data_dir->VirtualAddress,
		       IMAGE_EXPORT_DIRECTORY *);

	name_table = (unsigned int *)(pe->image +
				      export_dir_table->AddressOfNames);
	export_addr_table = (uint32_t *)
		(pe->image + export_dir_table->AddressOfFunctions);

	for (i = 0; i < export_dir_table->NumberOfNames; i++) {

		if (export_data_dir->VirtualAddress <= *export_addr_table ||
		    *export_addr_table >= (export_data_dir->VirtualAddress +
					   export_data_dir->Size))
			DBGLINKER("forwarder rva");

		DBGLINKER("export symbol: %s, at %p",
			  (char *)(pe->image + *name_table),
			  pe->image + *export_addr_table);

		pe_exports[num_pe_exports].dll = pe->name;
		pe_exports[num_pe_exports].name = pe->image + *name_table;
		pe_exports[num_pe_exports].addr =
			pe->image + *export_addr_table;

		num_pe_exports++;
		name_table++;
		export_addr_table++;
	}
	return 0;
}

static int fixup_imports(void *image, IMAGE_NT_HEADERS *nt_hdr)
{
	int i;
	char *name;
	int ret = 0;
	IMAGE_IMPORT_DESCRIPTOR *dirent;
	IMAGE_DATA_DIRECTORY *import_data_dir;
	PIMAGE_OPTIONAL_HEADER opt_hdr;

	opt_hdr = &nt_hdr->OptionalHeader;
	import_data_dir =
		&opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	dirent = RVA2VA(image, import_data_dir->VirtualAddress,
			IMAGE_IMPORT_DESCRIPTOR *);

	for (i = 0; dirent[i].Name; i++) {
		name = RVA2VA(image, dirent[i].Name, char*);

		DBGLINKER("imports from dll: %s", name);
		ret += import(image, &dirent[i], name);
	}
	return ret;
}

static int fixup_reloc(void *image, IMAGE_NT_HEADERS *nt_hdr)
{
	ULONG_PTR base;
	ULONG_PTR size;
	IMAGE_BASE_RELOCATION *fixup_block;
	IMAGE_DATA_DIRECTORY *base_reloc_data_dir;
	PIMAGE_OPTIONAL_HEADER opt_hdr;

	opt_hdr = &nt_hdr->OptionalHeader;
	base = opt_hdr->ImageBase;
	base_reloc_data_dir =
		&opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	if (base_reloc_data_dir->Size == 0)
		return 0;

	fixup_block = RVA2VA(image, base_reloc_data_dir->VirtualAddress,
			     IMAGE_BASE_RELOCATION *);
	DBGLINKER("fixup_block=%p, image=%p", fixup_block, image);
	DBGLINKER("fixup_block info: %x %d",
		  fixup_block->VirtualAddress, fixup_block->SizeOfBlock);

	while (fixup_block->SizeOfBlock) {
		int i;
		WORD fixup, offset;

		size = (fixup_block->SizeOfBlock -
			sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
		DBGLINKER("found %llu relocations in this block",
			  (uint64_t)size);

		for (i = 0; i < size; i++) {
			fixup = fixup_block->TypeOffset[i];
			offset = fixup & 0xfff;
			switch ((fixup >> 12) & 0x0f) {
			case IMAGE_REL_BASED_ABSOLUTE:
				break;

			case IMAGE_REL_BASED_HIGHLOW: {
				uint32_t addr;
				uint32_t *loc =
					RVA2VA(image,
					       fixup_block->VirtualAddress +
					       offset, uint32_t *);
				addr = RVA2VA(image, (*loc - base), uint32_t);
				DBGLINKER("relocation: *%p (Val:%X)= %X",
					  loc, *loc, addr);
				*loc = addr;
			}
				break;

			case IMAGE_REL_BASED_DIR64: {
				uint64_t addr;
				uint64_t *loc =
					RVA2VA(image,
					       fixup_block->VirtualAddress +
					       offset, uint64_t *);
				addr = RVA2VA(image, (*loc - base), uint64_t);
				DBGLINKER("relocation: *%p (Val:%llX)= %llx",
					  loc, *loc, addr);
				*loc = addr;
			}
				break;

			default:
				ERROR("unknown fixup: %08X",
				      (fixup >> 12) & 0x0f);
				return -EOPNOTSUPP;
				break;
			}
		}
		DBGLINKER("finished relocating block");

		fixup_block = (IMAGE_BASE_RELOCATION *)
			((void *)fixup_block + fixup_block->SizeOfBlock);
	};
	DBGLINKER("done relocating all");

	return 0;
}

/* Expand the image in memory if necessary. The image on disk does not
 * necessarily maps the image of the driver in memory, so we have to
 * re-write it in order to fulfill the sections alignments. The
 * advantage to do that is that rva_to_va becomes a simple
 * addition. */
static int fix_pe_image(struct pe_image *pe)
{
	void *image;
	IMAGE_SECTION_HEADER *sect_hdr;
	int i, sections;
	int image_size;

	if (pe->size == pe->opt_hdr->SizeOfImage) {
		/* Nothing to do */
		return 0;
	}

	image_size = pe->opt_hdr->SizeOfImage;
#ifdef CONFIG_X86_64
#ifdef PAGE_KERNEL_EXECUTABLE
	image = nvmalloc(image_size, GFP_KERNEL | __GFP_HIGHMEM,
			  PAGE_KERNEL_EXECUTABLE);
#elif defined PAGE_KERNEL_EXEC
	image = nvmalloc(image_size, GFP_KERNEL | __GFP_HIGHMEM,
			  PAGE_KERNEL_EXEC);
#else
#error x86_64 should have either PAGE_KERNEL_EXECUTABLE or PAGE_KERNEL_EXEC
#endif
#else
#ifdef cpu_has_nx
	/* hate to play with kernel macros, but PAGE_KERNEL_EXEC is
	 * not available to modules! */
	if (cpu_has_nx)
		image = nvmalloc(image_size, GFP_KERNEL | __GFP_HIGHMEM,
				  __pgprot(__PAGE_KERNEL & ~_PAGE_NX));
	else
		image = vmalloc(image_size);
#else
		image = vmalloc(image_size);
#endif
#endif
	if (image == NULL) {
		ERROR("failed to allocate enough space for new image:"
		      " %d bytes", image_size);
		return -ENOMEM;
	}
	memset(image, 0, image_size);

	/* Copy all the headers, ie everything before the first section. */

	sections = pe->nt_hdr->FileHeader.NumberOfSections;
	sect_hdr = IMAGE_FIRST_SECTION(pe->nt_hdr);

	DBGLINKER("copying headers: %u bytes", sect_hdr->PointerToRawData);

	memcpy(image, pe->image, sect_hdr->PointerToRawData);

	/* Copy all the sections */
	for (i = 0; i < sections; i++) {
		DBGLINKER("Copy section %s from %x to %x",
			  sect_hdr->Name, sect_hdr->PointerToRawData,
			  sect_hdr->VirtualAddress);
		if (sect_hdr->VirtualAddress+sect_hdr->SizeOfRawData >
		    image_size) {
			ERROR("Invalid section %s in driver", sect_hdr->Name);
			vfree(image);
			return -EINVAL;
		}

		memcpy(image+sect_hdr->VirtualAddress,
		       pe->image + sect_hdr->PointerToRawData,
		       sect_hdr->SizeOfRawData);
		sect_hdr++;
	}

	vfree(pe->image);
	pe->image = image;
	pe->size = image_size;

	/* Update our internal pointers */
	pe->nt_hdr = (IMAGE_NT_HEADERS *)
		(pe->image + ((IMAGE_DOS_HEADER *)pe->image)->e_lfanew);
	pe->opt_hdr = &pe->nt_hdr->OptionalHeader;

	DBGLINKER("set nt headers: nt_hdr=%p, opt_hdr=%p, image=%p",
		  pe->nt_hdr, pe->opt_hdr, pe->image);

	return 0;
}

#if defined(CONFIG_X86_64)
static void fix_user_shared_data_addr(char *driver, unsigned long length)
{
	unsigned long i, n, max_addr, *addr;

	TRACE1("fixing KI_USER_SHARED_DATA address in the driver");
	n = length - sizeof(unsigned long);
	max_addr = KI_USER_SHARED_DATA + sizeof(kuser_shared_data);
	for (i = 0; i < n; i++) {
		addr = (unsigned long *)(driver + i);
		if (*addr >= KI_USER_SHARED_DATA && *addr < max_addr) {
			*addr -= KI_USER_SHARED_DATA;
			*addr += (unsigned long)&kuser_shared_data;
			kuser_shared_data.reserved1 = 1;
		}
	}
}
#endif

int link_pe_images(struct pe_image *pe_image, unsigned short n)
{
	int i;
	struct pe_image *pe;

#if DEBUG >= 1
	/* Sanity checks */
	CHECK_SZ(IMAGE_SECTION_HEADER, IMAGE_SIZEOF_SECTION_HEADER);
	CHECK_SZ(IMAGE_FILE_HEADER, IMAGE_SIZEOF_FILE_HEADER);
	CHECK_SZ(IMAGE_OPTIONAL_HEADER, IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
	CHECK_SZ(IMAGE_NT_HEADERS, 4 + IMAGE_SIZEOF_FILE_HEADER +
		 IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
	CHECK_SZ(IMAGE_DOS_HEADER, 0x40);
	CHECK_SZ(IMAGE_EXPORT_DIRECTORY, 40);
	CHECK_SZ(IMAGE_BASE_RELOCATION, 8);
	CHECK_SZ(IMAGE_IMPORT_DESCRIPTOR, 20);
#endif

	for (i = 0; i < n; i++) {
		IMAGE_DOS_HEADER *dos_hdr;
		pe = &pe_image[i];
		dos_hdr = pe->image;

		if (pe->size < sizeof(IMAGE_DOS_HEADER)) {
			TRACE1("image too small: %d", pe->size);
			return -EINVAL;
		}

		pe->nt_hdr =
			(IMAGE_NT_HEADERS *)(pe->image + dos_hdr->e_lfanew);
		pe->opt_hdr = &pe->nt_hdr->OptionalHeader;

		pe->type = check_nt_hdr(pe->nt_hdr);
		if (pe->type <= 0) {
			TRACE1("type <= 0");
			return -EINVAL;
		}

		if (fix_pe_image(pe)) {
			TRACE1("bad PE image");
			return -EINVAL;
		}

		if (read_exports(pe)) {
			TRACE1("read exports failed");
			return -EINVAL;
		}
	}

	for (i = 0; i < n; i++) {
		pe = &pe_image[i];

		if (fixup_reloc(pe->image, pe->nt_hdr)) {
			TRACE1("fixup reloc failed");
			return -EINVAL;
		}
		if (fixup_imports(pe->image, pe->nt_hdr)) {
			TRACE1("fixup imports failed");
			return -EINVAL;
		}
#if defined(CONFIG_X86_64)
		fix_user_shared_data_addr(pe_image[i].image, pe_image[i].size);
#endif
		flush_icache_range((unsigned long)pe->image, pe->size);

		pe->entry =
			RVA2VA(pe->image,
			       pe->opt_hdr->AddressOfEntryPoint, void *);
		TRACE1("entry is at %p, rva at %08X", pe->entry,
		       pe->opt_hdr->AddressOfEntryPoint);
	}

	for (i = 0; i < n; i++) {
		pe = &pe_image[i];

		if (pe->type == IMAGE_FILE_DLL) {
			struct unicode_string ustring;
			char *buf = "0/0t0m0p00";
			int (*dll_entry)(struct unicode_string *ustring)
				wstdcall;

			memset(&ustring, 0, sizeof(ustring));
			ustring.buf = (wchar_t *)buf;
			dll_entry = (void *)get_dll_init(pe->name);

			TRACE1("calling dll_init at %p", dll_entry);
			if (!dll_entry || dll_entry(&ustring))
				ERROR("DLL initialize failed for %s",
				      pe->name);
		}
		else if (pe->type != IMAGE_FILE_EXECUTABLE_IMAGE)
			ERROR("illegal image type: %d", pe->type);
	}
	return 0;
}
