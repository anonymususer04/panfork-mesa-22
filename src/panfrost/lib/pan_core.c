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
        size_t file_size;
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

        uint64_t *cmdlist_instrs;
        struct pan_core_cmdlist *c;
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
                .e_machine = 24884, /* Randomly chosen unofficial value */
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
        uint64_t *data_64 = ptr + (size & 7);
        size_t file_size = ptr ? size : 0;

        while (file_size >= 8 && !data_64[(file_size / 8) - 1])
                file_size -= 8;

        struct pan_section_info info = {
                .va = va,
                .size = size,
                .file_size = file_size,
                .ptr = ptr,
                .label = label,
                .flags = flags,
        };

        util_dynarray_append(&core->sections, struct pan_section_info, info);

        ++core->num_phdr;
}

void
panfrost_core_add_cmdlist(struct pan_core *core, struct pan_core_cmdlist *c)
{
        unsigned size = sizeof(uint64_t) * c->num_instr;
        uint64_t *instrs = malloc(size);

        struct pan_section_info info = {
                .va = 0,
                .size = size,
                .file_size = size,
                .ptr = instrs,
                .label = ".gpu_jobs",
                .flags = 0,
        };

        util_dynarray_append(&core->sections, struct pan_section_info, info);

        ++core->num_phdr;

        uint64_t *ins = instrs;
        util_dynarray_foreach(&c->cmds, struct pan_core_cmd, instr) {
                if (instr->type != PAN_CORE_INSTR)
                        continue;
                *(ins++) = instr->instr;
        }
        assert(ins == instrs + c->num_instr);

        core->cmdlist_instrs = instrs;
        core->c = c;
}

void
panfrost_core_finish(struct pan_core *core)
{
        Elf *elf = core->elf;

        unsigned num_phdr = core->num_phdr;

        Elf64_Phdr *phdr = elf64_newphdr(elf, num_phdr);
        if (num_phdr && !phdr) {
                fprintf(stderr, "pan_core: error creating program headers"
                        ": %s\n", elf_errmsg(-1));
                return;
        }

        for (unsigned i = 0; i < num_phdr; ++i)
                phdr[i].p_type = PT_LOAD;
        elf_flagphdr(elf, ELF_C_SET, ELF_F_DIRTY);

        unsigned cmdlist_index = 0;

        Dwelf_Strtab *shst = dwelf_strtab_init(true);

        util_dynarray_foreach(&core->sections, struct pan_section_info, info) {
                Elf_Scn *section = elf_newscn(elf);
                if (!section) {
                        fprintf(stderr, "pan_core: error creating PROGBITS"
                                " section: %s\n", elf_errmsg(-1));
                        return;
                }
                Elf64_Shdr *shdr = elf64_getshdr(section);
                if (!shdr) {
                        fprintf(stderr, "pan_core: error getting header for"
                                " PROGBITS section: %s\n", elf_errmsg(-1));
                        return;
                }

                Dwelf_Strent *str = dwelf_strtab_add(shst,
                                                  info->label ?: "Unknown BO");

                if (!info->va)
                        cmdlist_index = elf_ndxscn(section);

                info->str = str;

                *shdr = (Elf64_Shdr) {
                        .sh_type = SHT_PROGBITS,
                        .sh_flags = SHF_ALLOC | (info->flags & 1 ?
                                                 SHF_EXECINSTR : SHF_WRITE),
                        .sh_addr = info->va,
                };

                Elf_Data *data = elf_newdata(section);
                if (!data) {
                        fprintf(stderr, "pan_core: error creating data for"
                               " PROGBITS section: %s\n", elf_errmsg(-1));
                        return;
                }
                *data = (Elf_Data) {
                        .d_buf = info->ptr,
                        .d_type = ELF_T_BYTE,
                        .d_version = EV_CURRENT,
                        .d_size = info->file_size,
                        .d_align = 1,
                };

                /* We get the data from the program headers, so compression
                 * wouldn't be automatically handled on the replay side... */
#if 0
                shdr->sh_flags &= ~((uint64_t)SHF_ALLOC);
                if (elf_compress(section, ELFCOMPRESS_ZLIB, 0) < 0)
                        fprintf(stderr, "pan_core: failure in elf_compress():"
                                " %s\n", elf_errmsg(-1));
                elf_flagscn(section, ELF_C_SET, ELF_F_DIRTY);
#endif

                info->shdr = elf64_getshdr(section);
        }

        struct pan_core_cmdlist c = {0};
        if (core->c)
                c = *(core->c);

        Elf64_Sym *sym_tab = calloc(sizeof(*sym_tab), c.num_sym);
        if (c.num_sym) {
                unsigned sym_tab_idx = 0;
                unsigned num_instr = 0;

                util_dynarray_foreach(&c.cmds, struct pan_core_cmd, instr) {
                        if (instr->type == PAN_CORE_INSTR)
                                ++num_instr;

                        if (instr->type != PAN_CORE_SYM)
                                continue;

                        uint64_t value = num_instr * 8;

                        if (sym_tab_idx)
                                sym_tab[sym_tab_idx - 1].st_size =
                                     value - sym_tab[sym_tab_idx - 1].st_value;

                        sym_tab[sym_tab_idx++] = (Elf64_Sym) {
                                .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                                .st_other = ELF64_ST_VISIBILITY(STV_DEFAULT),
                                .st_shndx = cmdlist_index,
                                .st_value = value,
                        };

                        instr->str = dwelf_strtab_add(shst, instr->sym);
                }

                sym_tab[sym_tab_idx - 1].st_size =
                        num_instr * 8 - sym_tab[sym_tab_idx - 1].st_value;
        }

        Elf_Scn *sym_scn = elf_newscn(elf);
        if (!sym_scn) {
                fprintf(stderr, "pan_core: error creating SYMTAB section"
                        ": %s\n", elf_errmsg(-1));
                return;
        }
        Elf64_Shdr *sym_shdr = elf64_getshdr(sym_scn);
        if (!sym_shdr) {
                fprintf(stderr, "error getting SYMTAB section header"
                        ": %s\n", elf_errmsg(-1));
                return;
        }
        *sym_shdr = (Elf64_Shdr) {
                .sh_type = SHT_SYMTAB,
                .sh_entsize = sizeof(Elf64_Sym),
        };

        Elf_Scn *str_scn = elf_newscn(elf);
        if (!str_scn) {
                fprintf(stderr, "pan_core: error creating STRTAB section"
                        ": %s\n", elf_errmsg(-1));
                return;
        }
        Elf64_Shdr *str_shdr = elf64_getshdr(str_scn);
        if (!str_shdr) {
                fprintf(stderr, "pan_core: error getting STRTAB section header"
                        ": %s\n", elf_errmsg(-1));
                return;
        }
        Dwelf_Strent *systrtabse = dwelf_strtab_add(shst, ".symtab");
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
        if (c.num_sym) {
                unsigned sym_tab_idx = 0;
                util_dynarray_foreach(&c.cmds, struct pan_core_cmd, instr) {
                        if (instr->type != PAN_CORE_SYM)
                                continue;
                        sym_tab[sym_tab_idx++].st_name =
                                dwelf_strent_off(instr->str);
                }
        }
        str_shdr->sh_name = dwelf_strent_off(shstrtabse);
        sym_shdr->sh_name = dwelf_strent_off(systrtabse);
        sym_shdr->sh_link = elf_ndxscn(str_scn);

        Elf_Data *data = elf_newdata(sym_scn);
        if (!data) {
                fprintf(stderr, "pan_core: error creating data for SYMTAB"
                        " section: %s\n", elf_errmsg(-1));
                return;
        }
        *data = (Elf_Data) {
                .d_buf = sym_tab,
                .d_type = ELF_T_SYM,
                .d_version = EV_CURRENT,
                .d_size = sizeof(*sym_tab) * c.num_sym,
                .d_align = 8,
        };

        if (elf_update(elf, ELF_C_NULL) < 0) {
                fprintf(stderr, "pan_core: failure in elf_update(NULL)"
                        ": %s\n", elf_errmsg(-1));
                return;
        }

        util_dynarray_foreach(&core->sections, struct pan_section_info, info) {
                *(phdr++) = (Elf64_Phdr) {
                        .p_type = PT_LOAD,
                        .p_offset = info->shdr->sh_offset,
                        .p_vaddr = info->va,
                        .p_paddr = 0,
                        .p_filesz = info->file_size,
                        .p_memsz = info->size,
                        .p_flags = PF_R | (info->flags & 1 ? PF_X : PF_W),
                        .p_align = 1,
                };
        }

        if (elf_update(elf, ELF_C_WRITE) < 0) {
                fprintf(stderr, "pan_core: failure in elf_update(WRITE)"
                        ": %s\n", elf_errmsg(-1));
                return;
        }
        elf_end(elf);

        util_dynarray_fini(&core->sections);
        FREE(core);
}

#else

/* Provide an implementation of the functions which does not use ELF but
 * instead writes hexdumps as a fallback. */

#include "util/u_memory.h"
#include "pan_core.h"

/* For pan_hexdump */
#include "genxml/decode.h"

struct pan_core {
        FILE *dump;
};

struct pan_core *
panfrost_core_create(int fd)
{
        struct pan_core *core = CALLOC_STRUCT(pan_core);
        core->dump = fdopen(dup(fd), "w");
        return core;
}

void
panfrost_core_add(struct pan_core *core, uint64_t va, size_t size,
                  void *ptr, const char *label, uint32_t flags)
{
        fprintf(core->dump,
                "%p: 0x%"PRIx64" - 0x%"PRIx64" (0x%"PRIx64"): %s\n",
                ptr, va, va + size, (uint64_t)size, label);

        if (ptr)
                pan_hexdump(core->dump, ptr, size, false);
}

void
panfrost_core_add_cmdlist(struct pan_core *core, struct pan_core_cmdlist *c)
{
        /* TODO: Do something here? */
        return;
}

void
panfrost_core_finish(struct pan_core *core)
{
        fclose(core->dump);
        FREE(core);
}

#endif
