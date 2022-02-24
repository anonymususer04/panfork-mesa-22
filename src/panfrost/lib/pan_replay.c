/*
 * Copyright (C) 2022 Icecream95
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
 */

#include <fcntl.h>
#include <stdio.h>
#include <inttypes.h>

#include <libelf.h>

#include "drm-uapi/panfrost_drm.h"

#include "pan_bo.h"
#include "pan_device.h"

// TODO: Move this function to Mesa's util/?

/* Used to fill a "hole" with POT-sized self-aligned objects from `lower` to
 * `upper` (but also useful for hierarchical tiling) */
static uint64_t
pan_pot_fill(uint64_t lower, uint64_t upper)
{
        if (lower == upper)
                return 0;

        assert(lower < upper);

        uint64_t xor = lower ^ upper;
        /* lower < upper, so this bit will always be a transition from 0 to 1.
         * Think about how string comparisons are implemented.. */
        unsigned bits = util_last_bit64(xor);

        uint64_t midpoint = 1ULL << bits;
        lower &= midpoint - 1;
        upper &= midpoint - 1;

        if (lower) {
                /* We have not reached the midpoint, so start small and
                 * increase in size with each call. */
                uint64_t size = midpoint - lower;
                return 1ULL << (ffsll(size) - 1);
        } else {
                /* We are at the midpoint, so start as large as possible. */
                assert(upper);
                return 1ULL << (util_last_bit64(upper) - 1);
        }
}

int main(int argc, char *argv[]) {
        int fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
        if (fd < 0) {
                fprintf(stderr, "No panfrost device\n");
                exit(1);
        }

        struct panfrost_device _dev;
        struct panfrost_device *dev = &_dev;
        panfrost_open_device_struct(NULL, fd, dev);

        if (!argv || !argv[1]) {
                printf("Usage: %s <PANFROST ELF CORE DUMP>.core\n", argv[0]);
                exit(1);
        }

        int dump_fd = open(argv[1], O_RDONLY);
        if (dump_fd == -1) {
                perror("open");
                exit(1);
        }

        elf_version(EV_CURRENT);

        Elf *e = elf_begin(dump_fd, ELF_C_READ, NULL);
        if (!e) {
                printf("Unable to open Elf file: %s\n", elf_errmsg(-1));
                exit(1);
        }

        struct util_dynarray temp_bos;
        util_dynarray_init(&temp_bos, NULL);

        struct util_dynarray created_bos;
        util_dynarray_init(&created_bos, NULL);

        size_t num_phdr = 0;
        elf_getphdrnum(e, &num_phdr);

        unsigned gpu_cmd = ~0;

        mali_ptr last_addr = 0;

        Elf64_Phdr *phdrs = elf64_getphdr(e);
        for (unsigned i = 0; i < num_phdr; ++i) {
                Elf64_Phdr *phdr = &phdrs[i];

                if (phdr->p_type != PT_LOAD)
                        continue;

                /* TODO: Search for the corresponding section? */

                mali_ptr gpu_addr = phdr->p_vaddr;

                /* TODO: Use one of the PT_ enum values to encode this case */
                if (!gpu_addr) {
                        gpu_cmd = i;
                        continue;
                }

                if (!last_addr)
                        last_addr = gpu_addr;

                uint64_t size;
                while ((size = pan_pot_fill(last_addr, gpu_addr))) {

                        struct panfrost_bo *pad_bo = panfrost_bo_create(
                                dev, size, PAN_BO_SHARED | PAN_BO_DELAY_MMAP,
                                "GPU address padding");

                        util_dynarray_append(&temp_bos, struct panfrost_bo *,
                                             pad_bo);

                        assert(last_addr == pad_bo->ptr.gpu);
                        last_addr += size;
                }

                uint32_t flags = (phdr->p_flags & PF_X) ? PAN_BO_EXECUTE : 0;

                /* TODO: Use actual section name */
                struct panfrost_bo *bo = panfrost_bo_create(
                        dev, phdr->p_memsz, flags, "Core dump replay");

                assert(bo->ptr.gpu == gpu_addr);

                last_addr = bo->ptr.gpu + bo->size;
                util_dynarray_append(&created_bos, struct panfrost_bo *, bo);

                if (phdr->p_filesz)
                        pread(dump_fd, bo->ptr.cpu, phdr->p_filesz,
                              phdr->p_offset);
        }

        if (gpu_cmd == ~0)
                return 0;

        uint32_t syncobj = 0;
        drmSyncobjCreate(dev->fd, DRM_SYNCOBJ_CREATE_SIGNALED, &syncobj);

        Elf64_Phdr *phdr = &phdrs[gpu_cmd];

        uint64_t *data = calloc(1, phdr->p_memsz);
        if (phdr->p_filesz)
                pread(dump_fd, data, phdr->p_filesz,
                      phdr->p_offset);

        unsigned num_instr = phdr->p_memsz / 8;

        unsigned i = 0;
        while (i < num_instr) {
                uint64_t instr = data[i];
                unsigned instr_size = (instr >> 16) & 0xffff;

                /* Execute a GPU job */
                /* TODO: Move to another function? */
                if ((instr & 0xffff) == 1) {
                        mali_ptr vertex_job = data[i + 1];
                        mali_ptr fragment_job = data[i + 2];
                        unsigned num_bos = data[i + 3];

                        assert(4 + num_bos == instr_size);

                        uint32_t *bo_handles =
                                calloc(num_bos, sizeof(*bo_handles));

                        for (unsigned j = 0; j < num_bos; ++j) {
                                mali_ptr bo_va = data[i + 4 + j];

                                unsigned gem_handle = ~0;
                                util_dynarray_foreach(&created_bos,
                                                  struct panfrost_bo *, bo) {

                                        if ((*bo)->ptr.gpu != bo_va)
                                                continue;

                                        gem_handle = (*bo)->gem_handle;
                                        break;
                                }

                                assert(gem_handle != ~0);

                                bo_handles[j] = gem_handle;
                        }

                        struct drm_panfrost_submit vertex_submit = {
                                .jc = vertex_job,
                                .bo_handles = (u64) (uintptr_t) bo_handles,
                                .bo_handle_count = num_bos,
                                .out_sync = fragment_job ? 0 : syncobj,
                        };

                        struct drm_panfrost_submit fragment_submit = {
                                .jc = fragment_job,
                                .bo_handles = (u64) (uintptr_t) bo_handles,
                                .bo_handle_count = num_bos,
                                .out_sync = syncobj,
                                .requirements = PANFROST_JD_REQ_FS,
                        };

                        int ret = 0;
                        if (vertex_job)
                                ret = drmIoctl(dev->fd,
                                               DRM_IOCTL_PANFROST_SUBMIT,
                                               &vertex_submit);
                        if (ret)
                                perror("submit vertex job");

                        ret = 0;
                        if (fragment_job)
                                ret = drmIoctl(dev->fd,
                                               DRM_IOCTL_PANFROST_SUBMIT,
                                               &fragment_submit);
                        if (ret)
                                perror("submit fragment job");

                        drmSyncobjWait(dev->fd, &syncobj, 1,
                                       INT64_MAX, 0, NULL);

                        /* TODO: Now what? Should we dump the framebuffer to a
                         * file? */

                        printf("job done\n");
                }

                i += instr_size;
        }
}

