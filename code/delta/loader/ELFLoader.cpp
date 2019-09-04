
// Copyright (C) Force67 2019

#include "loader/ELFLoader.h"
#include "kernel/Process.h"

namespace loaders
{
	// based on: https://github.com/idc/uplift/blob/master/uplift/src/program_info.cpp

	ELF_Loader::ELF_Loader(std::unique_ptr<uint8_t[]> d) :
		AppLoader(std::move(d))
	{}

	FileType ELF_Loader::IdentifyType(utl::File& file)
	{
		ELFHeader elf{};
		file.Read(elf);

		if (elf.magic == ELF_MAGIC && elf.machine == ELF_MACHINE_X86_64) {
			std::puts("[+] Identified file as plain ELF");
			return FileType::ELF;
		}

		return FileType::UNKNOWN;
	}

	LoadErrorCode ELF_Loader::Load(krnl::Process& proc)
	{
		elf = GetOffset<ELFHeader>(0);
		segments = GetOffset<ELFPgHeader>(elf->phoff);

		for (uint16_t i = 0; i < elf->phnum; i++) {
			auto s = &segments[i];
			std::printf("SEGMENTTYPE %x\n", s->type);
			switch (s->type) {
			case PT_LOAD:
			{
				// test alignment
				if (s->align & 0x3FFFull || s->vaddr & 0x3FFFull || s->offset & 0x3FFFull)
					return LoadErrorCode::BADALIGN;

				if (s->filesz > s->memsz)
					return LoadErrorCode::BADSEG;
				break;
			}
			case PT_TLS:
			{
				if (s->filesz > s->memsz)
					return LoadErrorCode::BADSEG;

				if (s->align > 32)
					return LoadErrorCode::BADALIGN;

				break;
			}
			case PT_SCE_DYNLIBDATA:
			{
				if (s->filesz == 0) {
					//__debugbreak();
					return LoadErrorCode::BADSEG;
				}

				dynld.ptr = GetOffset<char>(s->offset);
				dynld.size = s->filesz;

				std::printf("DYNLD OFS %llx\n", s->offset);
				break;
			}
			case PT_DYNAMIC:
			{
				if (s->filesz > s->memsz)
					return LoadErrorCode::BADSEG;
				break;
			}
			case PT_SCE_MODULEPARAM:
			{
				std::printf("FOUND PROCPARAM %llx (%llx byts big)\n", s->offset, s->filesz);

			} break;

			// like the PDB path on windows
			case PT_SCE_COMMENT:
			{
				auto* comment = GetOffset<SCEComment>(s->offset);

				std::string name;
				name.resize(comment->pathLength);
				memcpy(name.data(), GetOffset<void>(s->offset + sizeof(SCEComment)), comment->pathLength);

				std::printf("Comment %s, %d\n", name.c_str(), comment->unk);
				break;
			}
			case PT_SCE_LIBVERSION:
			{
				uint8_t* sec = GetOffset<uint8_t>(s->offset);

				// count entries
				int32_t index = 0;
				while (index <= s->filesz) {

					int8_t cb = sec[index];

					// skip control byte
					index++;

					for (int i = index; i < (index + cb); i++)
					{
						if (sec[i] == 0x3A) {

							size_t length = i - index;

							std::string name;
							name.resize(length);
							memcpy(name.data(), &sec[index], length);

							uint32_t version = *(uint32_t*)& sec[i + 1];
							uint8_t* vptr = (uint8_t*)& version;

							std::printf("lib <%s>, version %x.%x.%x.%x\n", name.c_str(), vptr[0], vptr[1], vptr[2], vptr[3]);
							break;
						}
					}

					// skip forward
					index += cb;
				}
				break;
			}
			}
		}

		DoDynamics();

		if (!MapImage(proc)) {
			return LoadErrorCode::BADMAP;
		}

		if (!ResolveImports()) {
			return LoadErrorCode::BADIMP;
		}

		return LoadErrorCode::SUCCESS;
	}

	void ELF_Loader::DoDynamics()
	{
		auto* s = GetSegment(ElfSegType::PT_DYNAMIC);
		ELFDyn* dynamics = GetOffset<ELFDyn>(s->offset);
		for (int32_t i = 0; i < (s->filesz / sizeof(ELFDyn)); i++) {
			auto* d = &dynamics[i];

			switch (d->tag) {
			case DT_SCE_JMPREL:
			{
				if (d->un.ptr > dynld.size) {
					std::printf("invalid JMPREL offset: %llx\n", d->un.ptr);
					continue;
				}

				jmpslots = (ElfRel*)(dynld.ptr + d->un.ptr);
				std::printf("the jumpslots are at %llx\n", jmpslots);
				break;
			}
			case DT_SCE_PLTRELSZ:
			{
				numJmpSlots = d->un.value / sizeof(ElfRel);
				break;
			}
			case DT_SCE_STRTAB:
			{
				if (d->un.ptr > dynld.size) {
					std::printf("invalid STRTAB offset: %llx", d->un.ptr);
					continue;
				}

				strtab.ptr = (char*)(dynld.ptr + d->un.ptr);
				break;
			}
			case DT_STRSZ:
			case DT_SCE_STRSIZE:
			{
				strtab.size = d->un.value;
				break;
			}
			case DT_SCE_EXPLIB:
			case DT_SCE_IMPLIB:
			{

				break;
			}
			case DT_SCE_SYMTAB:
			{
				if (d->un.ptr > dynld.size) {
					std::printf("invalid SYMTAB offset: %llx", d->un.ptr);
					continue;
				}

				symbols = (ElfSym*)(dynld.ptr + d->un.ptr);
				break;
			}
			case DT_SCE_SYMTABSZ:
			{
				numSymbols = d->un.value / sizeof(ElfSym);
				break;
			}
			}
		}

		for (int32_t i = 0; i < (s->filesz / sizeof(ELFDyn)); i++) {
			auto* d = &dynamics[i];
			switch (d->tag) {
			/*case DT_NEEDED:
			{
				std::printf("%i: DT_NEEDED %s\n", i, (char*)(strtab.ptr + d->un.value));
				break;
			}*/
			case DT_SCE_NEEDED_MODULE:
				std::printf("NEEDED MODULE %s\n", (char*)(strtab.ptr + (d->un.value & 0xFFFFFFFF)));
				break;
			}
		}
	}

	bool ELF_Loader::MapImage(krnl::Process& proc)
	{
		// the stuff we actually care about
		uint32_t totalimage = 0;
		for (uint16_t i = 0; i < elf->phnum; ++i) {
			const auto* p = &segments[i];
			if (p->type == PT_LOAD || PT_SCE_RELRO) {
				totalimage += (p->memsz + 0xFFF) & ~0xFFF;
			}
		}

		// could also check if INTERP exists
		if (totalimage == 0)
			return false;

		// reserve segment
		char* mem = (char*)proc.GetVirtualMemory().AllocateSeg(totalimage);

		// and move it!
		for (uint16_t i = 0; i < elf->phnum; i++) {
			auto s = &segments[i];
			if (s->type == PT_LOAD || PT_SCE_RELRO) {
				uint32_t perm = s->flags & (PF_R | PF_W | PF_X);
				switch (perm) {

				// todo: module segment collection :D
				case (PF_R | PF_X):
				{
					// its a codepage
					std::memcpy(mem + s->vaddr, GetOffset<void>(s->offset), s->filesz);
					break;
				}
				case (PF_R):
				{
					//Reserve a rdata segment
					std::memcpy(mem + s->vaddr, GetOffset<void>(s->offset), s->filesz);
					break;
				}
				case (PF_R | PF_W):
				{
					// reserve a read write data seg
					std::memcpy(mem + s->vaddr, GetOffset<void>(s->offset), s->filesz);
					break;
				}
				default:
				{
					std::puts("UNKNOWN param!");

				}
				}
			}
		}

		// register it to module chain
		auto entry = std::make_shared<krnl::Module>();
		entry->base = reinterpret_cast<uintptr_t*>(mem);
		entry->entry = reinterpret_cast<uintptr_t*>(mem + elf->entry);
		entry->size = totalimage;
		entry->name = "#MAIN#";
		proc.RegisterModule(std::move(entry));

		return true;
	}

	bool ELF_Loader::ResolveImports()
	{
		return true;

		for (size_t i = 0; i < numJmpSlots; i++) {
			auto* r = &jmpslots[i];

			int32_t type = ELF64_R_TYPE(r->info);
			int32_t isym = ELF64_R_SYM(r->info);

			if (type != R_X86_64_JUMP_SLOT) {
				std::printf("Unexpected reloc type %i for jump slot %i", type, i);
				continue;
			}

			if (isym >= numSymbols) {
				std::printf("Invalid symbol index %i for relocation %i", isym, i);
				continue;
			}

			if (symbols[isym].st_name >= strtab.size) {
				std::printf("Invalid symbol string offset %x of symbol %i for relocation %i", symbols[isym].st_name, isym, i);
				continue;
			}

			const char* name = &strtab.ptr[symbols[isym].st_name];
			std::printf("IMPORT NAME %s\n", name);
		}

		return true;
	}

	LoadErrorCode ELF_Loader::Unload()
	{
		return LoadErrorCode::UNKNOWN;
	}
}