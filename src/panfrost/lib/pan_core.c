/*
 * Copyright (c) 2021 Icecream95
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifdef PAN_CORE

#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

#include <libelf.h>
#include <elfutils/libdwelf.h>

#include "util/u_dynarray.h"
#include "util/u_memory.h"

#include "pan_core.h"

struct pan_section_info {
        uint64_t va;
        size_t size;
        void *ptr;
        const char *label;
        uint32_t flags;
        Elf64_Shdr *shdr;
        Dwelf_Strent *str;
};

struct pan_core {
        Elf *elf;
        unsigned num_phdr;
        struct util_dynarray sections;
};

struct pan_core *
panfrost_core_create(int fd)
{
        elf_version(EV_CURRENT);

        Elf *elf = elf_begin(fd, ELF_C_WRITE, NULL);
        if (!elf) {
                printf("error creating ELF descriptor: %s\n", elf_errmsg(-1));
                return NULL;
        }

        Elf64_Ehdr *ehdr = elf64_newehdr(elf);
        if (!ehdr) {
                printf("error creating ELF header: %s\n", elf_errmsg(-1));
                return NULL;
        }
        *ehdr = (Elf64_Ehdr) {
                .e_ident[EI_CLASS] = ELFCLASS64,
                .e_ident[EI_DATA] = ELFDATA2LSB,
                .e_ident[EI_OSABI] = ELFOSABI_STANDALONE,
                .e_type = ET_CORE,
                .e_machine = EM_NONE,
                .e_version = EV_CURRENT,
        };
        elf_flagehdr(elf, ELF_C_SET, ELF_F_DIRTY);

        struct pan_core *core = CALLOC_STRUCT(pan_core);
        core->elf = elf;
        util_dynarray_init(&core->sections, NULL);

        return core;
}

void
panfrost_core_add(struct pan_core *core, uint64_t va, size_t size,
                  void *ptr, const char *label, uint32_t flags)
{
        struct pan_section_info info = {
                .va = va,
                .size = size,
                .ptr = ptr,
                .label = label,
                .flags = flags,
        };

        util_dynarray_append(&core->sections, struct pan_section_info, info);

        if (ptr)
                ++core->num_phdr;
}

void
panfrost_core_finish(struct pan_core *core)
{
        Elf *elf = core->elf;

        unsigned num_phdr = core->num_phdr;

        Elf64_Phdr *phdr = elf64_newphdr(elf, num_phdr);
        if (num_phdr && !phdr) {
                printf("error creating program headers: %s\n", elf_errmsg(-1));
                return;
        }

        for (unsigned i = 0; i < num_phdr; ++i)
                phdr[i].p_type = PT_LOAD;
        elf_flagphdr(elf, ELF_C_SET, ELF_F_DIRTY);

        Dwelf_Strtab *shst = dwelf_strtab_init(true);

        util_dynarray_foreach(&core->sections, struct pan_section_info, info) {
                Elf_Scn *section = elf_newscn(elf);
                if (!section) {
                        printf("error creating PROGBITS section: %s\n", elf_errmsg(-1));
                        return;
                }
                Elf64_Shdr *shdr = elf64_getshdr(section);
                if (!shdr) {
                        printf("error getting header for PROGBITS section: %s\n", elf_errmsg(-1));
                        return;
                }

                Dwelf_Strent *str = dwelf_strtab_add(shst, info->label ?: "Unknown BO");

                info->shdr = shdr;
                info->str = str;

                *shdr = (Elf64_Shdr) {
                        .sh_type = info->ptr ? SHT_PROGBITS : SHT_NOBITS,
                        .sh_flags = SHF_ALLOC | (info->flags & 1 ? SHF_EXECINSTR : SHF_WRITE),
                        .sh_addr = info->va,
                        .sh_size = info->size,
                        .sh_addralign = 1,
                };

                Elf_Data *data = elf_newdata(section);
                if (!data) {
                        printf("error creating data for PROGBITS section: %s\n", elf_errmsg(-1));
                        return;
                }
                *data = (Elf_Data) {
                        .d_buf = info->ptr,
                        .d_type = ELF_T_BYTE,
                        .d_version = EV_CURRENT,
                        .d_size = info->size,
                        .d_align = 1,
                };
        }

        Elf_Scn *str_scn = elf_newscn(elf);
        if (!str_scn) {
                printf("error creating STRTAB section: %s\n", elf_errmsg(-1));
                return;
        }
        Elf64_Shdr *str_shdr = elf64_getshdr(str_scn);
        if (!str_shdr) {
                printf("error getting STRTAB section header: %s\n", elf_errmsg(-1));
                return;
        }
        Dwelf_Strent *shstrtabse = dwelf_strtab_add(shst, ".shstrtab");

        *str_shdr = (Elf64_Shdr) {
                .sh_type = SHT_STRTAB,
                .sh_entsize = 1,
        };
        elf64_getehdr(elf)->e_shstrndx = elf_ndxscn(str_scn);

        dwelf_strtab_finalize(shst, elf_newdata(str_scn));

        util_dynarray_foreach(&core->sections, struct pan_section_info, info) {
                info->shdr->sh_name = dwelf_strent_off(info->str);
        }
        str_shdr->sh_name = dwelf_strent_off(shstrtabse);

        if (elf_update(elf, ELF_C_NULL) < 0)
        {
                printf("failure in elf_update(NULL): %s\n", elf_errmsg(-1));
                return;
        }

        util_dynarray_foreach(&core->sections, struct pan_section_info, info) {
                if (!info->ptr)
                        continue;

                *(phdr++) = (Elf64_Phdr) {
                        .p_type = PT_LOAD,
                        .p_offset = info->shdr->sh_offset,
                        .p_vaddr = info->va,
                        .p_paddr = 0,
                        .p_filesz = info->size,
                        .p_memsz = info->size,
                        .p_flags = PF_R | (info->flags & 1 ? PF_X : PF_W),
                        .p_align = 1,
                };
        }

        if (elf_update(elf, ELF_C_WRITE) < 0)
        {
                printf("failure in elf_update(WRITE): %s\n", elf_errmsg(-1));
                return;
        }
        elf_end(elf);

        util_dynarray_fini(&core->sections);
        free(core);
}

#endif
