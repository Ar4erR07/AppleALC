//
//  kern_mach.cpp
//  AppleALC
//
//  Certain parts of code are the subject of
//   copyright © 2011, 2012, 2013, 2014 fG!, reverser@put.as - http://reverse.put.as
//  Copyright © 2016 vit9696. All rights reserved.
//

#include "kern_mach.hpp"
#include "kern_compression.hpp"
#include "kern_util.hpp"

#include <sys/malloc.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/disk.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/vm_param.h>
#include <i386/proc_reg.h>
#include <kern/thread.h>

extern proc_t kernproc;

kern_return_t MachInfo::init(const char * const paths[], size_t num) {
    kern_return_t error = KERN_FAILURE;
  
    // Check if we have a proper credential, prevents a race-condition panic on 10.11.4 Beta
    // When calling kauth_cred_get() for the current_thread.
    // This probably wants a better solution...
    if (!kernproc || !current_thread() || !vfs_context_current() || !vfs_context_ucred(vfs_context_current())) {
        SYSLOG("mach @ current context has no credential, it's too early");
        return error;
    }
	
	// lookup vnode for /mach_kernel
	auto machHeader = Buffer::create<uint8_t>(HeaderSize);
	if (!machHeader) {
		SYSLOG("mach @ can't allocate header memory.");
		return error;
	}
	
	vnode_t vnode = NULLVP;
	vfs_context_t ctxt = nullptr;
	bool found = false;

	for(size_t i = 0; i < num; i++) {
		vnode = NULLVP;
		ctxt = vfs_context_create(nullptr);
		
		errno_t err = vnode_lookup(paths[i], 0, &vnode, ctxt);
		if(!err) {
			kern_return_t readError = readMachHeader(machHeader, vnode, ctxt);
			if(readError == KERN_SUCCESS) {
				if(isKernel && !isCurrentKernel(machHeader)) {
					vnode_put(vnode);
				} else {
					DBGLOG("mach @ Found executable at path: %s", paths[i]);
					found = true;
					break;
				}
			}
		}
		
		vfs_context_rele(ctxt);
	}
	
	if(!found) {
		DBGLOG("mach @ couldn't find a suitable executable");
		Buffer::deleter(machHeader);
		return error;
	}
	
	processMachHeader(machHeader);
	if (linkedit_fileoff && symboltable_fileoff) {
		// read linkedit from filesystem
		error = readLinkedit(vnode, ctxt);
		if (error != KERN_SUCCESS) {
			SYSLOG("mach @ could not read the linkedit segment");
		}
	} else {
		SYSLOG("mach @ couldn't find the necessary mach segments or sections (linkedit %llX, sym %X)",
			   linkedit_fileoff, symboltable_fileoff);
	}

	vfs_context_rele(ctxt);
	// drop the iocount due to vnode_lookup()
	// we must do this or the machine gets stuck on shutdown/reboot
	vnode_put(vnode);
	
	// We do not need the whole file buffer anymore
	if (file_buf) {
		Buffer::deleter(file_buf);
		file_buf = nullptr;
	}
	
	Buffer::deleter(machHeader);
	
	return error;
}

void MachInfo::deinit() {
	if (linkedit_buf) {
		Buffer::deleter(linkedit_buf);
		linkedit_buf = nullptr;
	}
}

mach_vm_address_t MachInfo::findKernelBase() {
	// calculate the address of the int80 handler
	mach_vm_address_t tmp = calculateInt80Address();
	
	// search backwards for the kernel base address (mach-o header)
	segment_command_64 *segmentCommand;
	while (tmp > 0) {
		if (*(uint32_t*)(tmp) == MH_MAGIC_64) {
			// make sure it's the header and not some reference to the MAGIC number
			segmentCommand = reinterpret_cast<segment_command_64 *>(tmp + sizeof(mach_header_64));
			if (strncmp(segmentCommand->segname, "__TEXT", 16) == 0) {
				DBGLOG("mach @ Found kernel mach-o header address at %p", (void*)(tmp));
				return tmp;
			}
		}
		// overflow check
		if (tmp - 1 > tmp) break;
		tmp--;
	}
	return 0;
}

kern_return_t MachInfo::setKernelWriting(bool enable) {
	kern_return_t res = KERN_SUCCESS;
	if (enable) __asm__ volatile("cli"); // disable interrupts
	if (setWPBit(!enable) != KERN_SUCCESS) {
		SYSLOG("mach @ failed to set kernel protection");
		enable = false;
		res = KERN_FAILURE;
	}
	if (!enable) __asm__ volatile("sti"); // enable interrupts
	return res;
}

mach_vm_address_t MachInfo::solveSymbol(const char *symbol) {
	if (!linkedit_buf) {
		SYSLOG("mach @ no loaded linkedit buffer found");
		return 0;
	}
	
	if (!symboltable_fileoff) {
		SYSLOG("mach @ no symtable offsets found");
		return 0;
	}
	
	if (!kaslr_slide_set) {
		SYSLOG("mach @ no slide is present");
		return 0;
	}
	
	// symbols and strings offsets into LINKEDIT
	// we just read the __LINKEDIT but fileoff values are relative to the full /mach_kernel
	// subtract the base of LINKEDIT to fix the value into our buffer
	uint64_t symbolOff = symboltable_fileoff - (linkedit_fileoff);
	if (symbolOff > symboltable_fileoff) return 0;
	uint64_t stringOff = stringtable_fileoff - (linkedit_fileoff);
	if (stringOff > stringtable_fileoff) return 0;
	
	nlist_64 *nlist64 = NULL;
	// search for the symbol and get its location if found
	for (uint32_t i = 0; i < symboltable_nr_symbols; i++) {
		// get the pointer to the symbol entry and extract its symbol string
		nlist64 = (nlist_64*)(linkedit_buf + symbolOff + i * sizeof(nlist_64));
		char *symbolStr = reinterpret_cast<char *>(linkedit_buf + stringOff + nlist64->n_un.n_strx);
		// find if symbol matches
		if (strncmp(symbol, symbolStr, strlen(symbol)+1) == 0) {
			DBGLOG("mach @ Found symbol %s at 0x%llx (non-aslr 0x%llx)", symbol, nlist64->n_value + kaslr_slide, nlist64->n_value);
			// the symbol values are without kernel ASLR so we need to add it
			return nlist64->n_value + kaslr_slide;
		}
	}
	// failure
	return 0;
}

int MachInfo::readFileData(void *buffer, off_t off, size_t sz, vnode_t vnode, vfs_context_t ctxt) {
	int error = 0;

	uio_t uio = uio_create(1, off, UIO_SYSSPACE, UIO_READ);
	if (!uio) {
		SYSLOG("mach @ uio_create returned null!");
		return error;
	}
	
	// imitate the kernel and read a single page from the file
	error = uio_addiov(uio, CAST_USER_ADDR_T(buffer), sz);
	if (error) {
		SYSLOG("mach @ uio_addiov returned error %d!", error);
		return error;
	}
	
	// read mach vnode into the buffer
	error = VNOP_READ(vnode, uio, 0, ctxt);
	if (error) {
		SYSLOG("mach @ VNOP_READ failed %d!", error);
		return error;
	}
	
	if (uio_resid(uio)) {
		SYSLOG("mach @ uio_resid returned non-null!");
		return EINVAL;
	}
	
	return error;
}

size_t MachInfo::readFileSize(vnode_t vnode, vfs_context_t ctxt) {
	// Taken from XNU vnode_size
	vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	return vnode_getattr(vnode, &va, ctxt) ? 0 : va.va_data_size;
}

kern_return_t MachInfo::readMachHeader(uint8_t *buffer, vnode_t vnode, vfs_context_t ctxt, off_t off) {
	int error = readFileData(buffer, off, HeaderSize, vnode, ctxt);
	if (error) {
		SYSLOG("mach @ mach header read failed with %d error", error);
		return KERN_FAILURE;
	}
	
	while (1) {
		auto magic = *reinterpret_cast<uint32_t *>(buffer);
		switch (magic) {
			case MH_MAGIC_64:
				fat_offset = off;
				return KERN_SUCCESS;
			case FAT_MAGIC: {
				uint32_t num = _OSSwapInt32(reinterpret_cast<fat_header *>(buffer)->nfat_arch);
				for (uint32_t i = 0; i < num; i++) {
					auto arch = reinterpret_cast<fat_arch *>(buffer + i*sizeof(fat_arch) + sizeof(fat_header));
					if (_OSSwapInt32(arch->cputype) == CPU_TYPE_X86_64)
						return readMachHeader(buffer, vnode, ctxt, _OSSwapInt32(arch->offset));
				}
				SYSLOG("mach @ failed to find a x86_64 mach");
				return KERN_FAILURE;
			}
			case CompressedMagic: { // comp
				auto header = reinterpret_cast<CompressedHeader *>(buffer);
				auto compressedBuf = Buffer::create<uint8_t>(_OSSwapInt32(header->compressed));
				if (!compressedBuf) {
					SYSLOG("mach @ failed to allocate memory for reading mach binary");
				} else if (readFileData(compressedBuf, off+sizeof(CompressedHeader), _OSSwapInt32(header->compressed),
										vnode, ctxt) != KERN_SUCCESS) {
					SYSLOG("mach @ failed to read compressed binary");
				} else {
					DBGLOG("mach @ decompressing %d bytes (estimated %d bytes) with %X compression mode",
						   _OSSwapInt32(header->compressed), _OSSwapInt32(header->decompressed), header->compression);
					file_buf = decompressData(header->compression, _OSSwapInt32(header->decompressed),
											  compressedBuf, _OSSwapInt32(header->compressed));
					
					// Try again
					if (file_buf) {
						memcpy(buffer, file_buf, HeaderSize);
						Buffer::deleter(compressedBuf);
						continue;
					}
				}
				
				Buffer::deleter(compressedBuf);
				return KERN_FAILURE;
			}
			
			default:
				SYSLOG("mach @ read mach has unsupported %X magic", magic);
				return KERN_FAILURE;
		}
	}
	
	return KERN_FAILURE;
}

kern_return_t MachInfo::readLinkedit(vnode_t vnode, vfs_context_t ctxt) {
	// we know the location of linkedit and offsets into symbols and their strings
	// now we need to read linkedit into a buffer so we can process it later
	// __LINKEDIT total size is around 1MB
	// we should free this buffer later when we don't need anymore to solve symbols
	linkedit_buf = Buffer::create<uint8_t>(linkedit_size);
	if (!linkedit_buf) {
		SYSLOG("mach @ Could not allocate enough memory (%lld) for __LINKEDIT segment", linkedit_size);
		return KERN_FAILURE;
	}
	
	if (!file_buf) {
		int error = readFileData(linkedit_buf, fat_offset+linkedit_fileoff, linkedit_size, vnode, ctxt);
		if (error) {
			SYSLOG("mach @ linkedit read failed with %d error", error);
			return KERN_FAILURE;
		}
	} else {
		memcpy(linkedit_buf, file_buf+linkedit_fileoff, linkedit_size);
	}

	return KERN_SUCCESS;
}

//FIXME: Guard pointer access by HeaderSize
void MachInfo::processMachHeader(void *header) {
	mach_header_64 *mh = static_cast<mach_header_64 *>(header);
	size_t headerSize = sizeof(mach_header_64);
	
	// point to the first load command
	char *addr = static_cast<char *>(header) + headerSize;
	// iterate over all load cmds and retrieve required info to solve symbols
	// __LINKEDIT location and symbol/string table location
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		load_command *loadCmd = reinterpret_cast<load_command *>(addr);
		if (loadCmd->cmd == LC_SEGMENT_64) {
			segment_command_64 *segCmd = reinterpret_cast<segment_command_64 *>(loadCmd);
			// use this one to retrieve the original vm address of __TEXT so we can compute kernel aslr slide
			if (strncmp(segCmd->segname, "__TEXT", 16) == 0) {
				DBGLOG("mach @ header processing found TEXT");
				disk_text_addr = segCmd->vmaddr;
			} else if (strncmp(segCmd->segname, "__LINKEDIT", 16) == 0) {
				DBGLOG("mach @ header processing found LINKEDIT");
				linkedit_fileoff = segCmd->fileoff;
				linkedit_size    = segCmd->filesize;
			}
		}
		// table information available at LC_SYMTAB command
		else if (loadCmd->cmd == LC_SYMTAB) {
			DBGLOG("mach @ header processing found SYMTAB");
			symtab_command *symtab_cmd = (symtab_command*)loadCmd;
			symboltable_fileoff    = symtab_cmd->symoff;
			symboltable_nr_symbols = symtab_cmd->nsyms;
			stringtable_fileoff    = symtab_cmd->stroff;
		}
		addr += loadCmd->cmdsize;
	}
}

//FIXME: Guard pointer access by HeaderSize
kern_return_t MachInfo::getRunningAddresses(mach_vm_address_t slide, size_t size) {
	if (kaslr_slide_set) return KERN_SUCCESS;
	
	if (size > 0)
		memory_size = size;
	
	// We are meant to know the base address of kexts
	mach_vm_address_t base = slide ? slide : findKernelBase();
	if (base != 0) {
		// get the vm address of __TEXT segment
		mach_header_64 *mh = reinterpret_cast<mach_header_64 *>(base);
		size_t headerSize = sizeof(mach_header_64);
		
		load_command *loadCmd;
		char *addr = reinterpret_cast<char *>(base) + headerSize;
		for (uint32_t i = 0; i < mh->ncmds; i++) {
			loadCmd = reinterpret_cast<load_command *>(addr);
			if (loadCmd->cmd == LC_SEGMENT_64) {
				segment_command_64 *segCmd = reinterpret_cast<segment_command_64 *>(loadCmd);
				if (strncmp(segCmd->segname, "__TEXT", 16) == 0) {
					running_text_addr = segCmd->vmaddr;
					running_mh = mh;
					break;
				}
			}
			addr += loadCmd->cmdsize;
		}
	}
	
	// compute kaslr slide
	if (running_text_addr && running_mh) {
		if (!slide) {
			kaslr_slide = running_text_addr - disk_text_addr;
		} else {
			kaslr_slide = slide;
		}
		kaslr_slide_set = true;
		
		DBGLOG("mach @ aslr/load slide is 0x%llx", kaslr_slide);
	} else {
		SYSLOG("mach @ Couldn't find the running addresses");
		return KERN_FAILURE;
	}
	
	return KERN_SUCCESS;
}

void MachInfo::getRunningPosition(uint8_t * &header, size_t &size) {
	header = reinterpret_cast<uint8_t *>(running_mh);
	size = memory_size > 0 ? memory_size : HeaderSize;
	DBGLOG("mach @ getRunningPosition %p of memory %zu size", header, size);
}

//FIXME: Guard pointer access by HeaderSize
uint64_t *MachInfo::getUUID(void *header) {
	if (!header) return nullptr;
	
	mach_header_64 *mh = static_cast<mach_header_64 *>(header);
	size_t size = sizeof(struct mach_header_64);
	
	uint8_t *addr = static_cast<uint8_t *>(header) + size;
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		load_command *loadCmd = reinterpret_cast<load_command *>(addr);
		if (loadCmd->cmd == LC_UUID) {
			return reinterpret_cast<uint64_t *>((reinterpret_cast<uuid_command *>(loadCmd))->uuid);
		}
		
		addr += loadCmd->cmdsize;
	}
	
	return nullptr;
}

bool MachInfo::isCurrentKernel(void *kernelHeader) {
	mach_vm_address_t kernelBase = findKernelBase();
	
	uint64_t *uuid1 = getUUID(kernelHeader);
	uint64_t *uuid2 = getUUID(reinterpret_cast<void *>(kernelBase));

	return uuid1 && uuid2 && uuid1[0] == uuid2[0] && uuid1[1] == uuid2[1];
}

mach_vm_address_t MachInfo::getIDTAddress() {
	uint8_t idtr[10];
	__asm__ volatile ("sidt %0": "=m" (idtr));
	return *(mach_vm_address_t *)(idtr+2);
}

mach_vm_address_t MachInfo::calculateInt80Address() {
	// retrieve the address of the IDT
	mach_vm_address_t idtAddr = getIDTAddress();
	
	// find the address of interrupt 0x80 - EXCEP64_SPC_USR(0x80,hi64_unix_scall) @ osfmk/i386/idt64.s
	descriptor_idt *int80Descr = nullptr;
	mach_vm_address_t int80Addr = 0;
	// we need to compute the address, it's not direct
	// extract the stub address
	
	// retrieve the descriptor for interrupt 0x80
	// the IDT is an array of descriptors
	int80Descr = reinterpret_cast<descriptor_idt *>(idtAddr+sizeof(descriptor_idt)*0x80);
	uint64_t high = static_cast<uint64_t>(int80Descr->offset_high) << 32;
	uint32_t middle = static_cast<uint32_t>(int80Descr->offset_middle) << 16;
	int80Addr = static_cast<mach_vm_address_t>(high + middle + int80Descr->offset_low);
	DBGLOG("mach @ Address of interrupt 80 stub is 0x%llx", int80Addr);
	
	return int80Addr;
}

kern_return_t MachInfo::setWPBit(bool enable) {
	uintptr_t cr0;
	// retrieve current value
	cr0 = get_cr0();
	if (enable) {
		// add the WP bit
		cr0 = cr0 | CR0_WP;
	} else {
		// remove the WP bit
		cr0 = cr0 & ~CR0_WP;
	}
	// and write it back
	set_cr0(cr0);
	// verify if we were successful
	if (((get_cr0() & CR0_WP) != 0 && enable) ||
		((get_cr0() & CR0_WP) == 0 && !enable)) {
		return KERN_SUCCESS;
	} else {
		return KERN_FAILURE;
	}
}
