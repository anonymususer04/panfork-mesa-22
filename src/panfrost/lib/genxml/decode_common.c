/*
 * Copyright (C) 2019 Alyssa Rosenzweig
 * Copyright (C) 2017-2018 Lyude Paul
 * Copyright (C) 2019 Collabora, Ltd.
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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "decode.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"

FILE *pandecode_dump_stream;

bool pandecode_no_mprotect;

/* Memory handling */

static struct rb_tree mmap_tree;

static struct util_dynarray ro_mappings;

#define to_mapped_memory(x) \
	rb_node_data(struct pandecode_mapped_memory, x, node)

/*
 * Compare a GPU VA to a node, considering a GPU VA to be equal to a node if it
 * is contained in the interval the node represents. This lets us store
 * intervals in our tree.
 */
static int
pandecode_cmp_key(const struct rb_node *lhs, const void *key)
{
        struct pandecode_mapped_memory *mem = to_mapped_memory(lhs);
        uint64_t *gpu_va = (uint64_t *) key;

        if (mem->gpu_va <= *gpu_va && *gpu_va < (mem->gpu_va + mem->length))
                return 0;
        else
                return mem->gpu_va - *gpu_va;
}

static int
pandecode_cmp(const struct rb_node *lhs, const struct rb_node *rhs)
{
        return to_mapped_memory(lhs)->gpu_va - to_mapped_memory(rhs)->gpu_va;
}

static struct pandecode_mapped_memory *
pandecode_find_mapped_gpu_mem_containing_rw(uint64_t addr)
{
        struct rb_node *node = rb_tree_search(&mmap_tree, &addr, pandecode_cmp_key);

        return to_mapped_memory(node);
}

struct pandecode_mapped_memory *
pandecode_find_mapped_gpu_mem_containing(uint64_t addr)
{
        struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing_rw(addr);

        if (!pandecode_no_mprotect && mem && mem->addr && !mem->ro) {
                mprotect(mem->addr, mem->length, PROT_READ);
                mem->ro = true;
                util_dynarray_append(&ro_mappings, struct pandecode_mapped_memory *, mem);
        }

        return mem;
}

void
pandecode_map_read_write(void)
{
        util_dynarray_foreach(&ro_mappings, struct pandecode_mapped_memory *, mem) {
                (*mem)->ro = false;
                mprotect((*mem)->addr, (*mem)->length, PROT_READ | PROT_WRITE);
        }
        util_dynarray_clear(&ro_mappings);
}

static void
pandecode_add_name(struct pandecode_mapped_memory *mem, uint64_t gpu_va, const char *name)
{
        if (!name) {
                /* If we don't have a name, assign one */

                snprintf(mem->name, sizeof(mem->name) - 1,
                         "memory_%" PRIx64, gpu_va);
        } else {
                assert((strlen(name) + 1) < sizeof(mem->name));
                memcpy(mem->name, name, strlen(name) + 1);
        }
}

void
pandecode_inject_mmap(uint64_t gpu_va, void *cpu, unsigned sz, const char *name)
{
        /* First, search if we already mapped this and are just updating an address */

        struct pandecode_mapped_memory *existing =
                pandecode_find_mapped_gpu_mem_containing_rw(gpu_va);

        if (existing && existing->gpu_va == gpu_va) {
                existing->length = sz;
                existing->addr = cpu;
                pandecode_add_name(existing, gpu_va, name);
                return;
        }

        /* Otherwise, add a fresh mapping */
        struct pandecode_mapped_memory *mapped_mem = NULL;

        mapped_mem = calloc(1, sizeof(*mapped_mem));
        mapped_mem->gpu_va = gpu_va;
        mapped_mem->length = sz;
        mapped_mem->addr = cpu;
        pandecode_add_name(mapped_mem, gpu_va, name);
        rb_tree_init(&mapped_mem->annotations);

        /* Add it to the tree */
        rb_tree_insert(&mmap_tree, &mapped_mem->node, pandecode_cmp);
}

static void
pandecode_free_mem(struct pandecode_mapped_memory *mem)
{
        rb_tree_foreach_safe(struct pandecode_annotation, it, &mem->annotations, node) {
                rb_tree_remove(&mem->annotations, &it->node);
                free(it->text);
                free(it);
        }

        rb_tree_remove(&mmap_tree, &mem->node);
        free(mem);
}

void
pandecode_inject_free(uint64_t gpu_va, unsigned sz)
{
        struct pandecode_mapped_memory *mem =
                pandecode_find_mapped_gpu_mem_containing_rw(gpu_va);

        if (!mem)
                return;

        assert(mem->gpu_va == gpu_va);
        assert(mem->length == sz);

        pandecode_free_mem(mem);
}

char *
pointer_as_memory_reference(uint64_t ptr)
{
        struct pandecode_mapped_memory *mapped;
        char *out = malloc(128);

        /* Try to find the corresponding mapped zone */

        mapped = pandecode_find_mapped_gpu_mem_containing_rw(ptr);

        if (mapped) {
                snprintf(out, 128, "%s + 0x%x", mapped->name, (int) (ptr - mapped->gpu_va));
                return out;
        }

        /* Just use the raw address if other options are exhausted */

        snprintf(out, 128, "0x%" PRIx64, ptr);
        return out;

}

void *
panblob_cpu_addr(struct panfrost_device *dev, mali_ptr gpu)
{
        struct pandecode_mapped_memory *mem =
                pandecode_find_mapped_gpu_mem_containing_rw(gpu);

        if (mem && mem->addr) {
                // TODO: Include pan_device.h for gating the print on a PAN_DBG_
                fprintf(stderr, "found address: %"PRIx64" -> %p\n", gpu, mem->addr);
                return mem->addr;
        }

        return NULL;
}

static int pandecode_dump_frame_count = 0;

static bool force_stderr = false;

void
pandecode_dump_file_open(void)
{
        if (pandecode_dump_stream)
                return;

        /* This does a getenv every frame, so it is possible to use
         * setenv to change the base at runtime.
         */
        const char *dump_file_base = debug_get_option("PANDECODE_DUMP_FILE", "pandecode.dump");
        if (force_stderr || !strcmp(dump_file_base, "stderr"))
                pandecode_dump_stream = stderr;
        else {
                char buffer[1024];
                snprintf(buffer, sizeof(buffer), "%s.%04d", dump_file_base, pandecode_dump_frame_count);
                printf("pandecode: dump command stream to file %s\n", buffer);
                pandecode_dump_stream = fopen(buffer, "w");
                if (!pandecode_dump_stream)
                        fprintf(stderr,
                                "pandecode: failed to open command stream log file %s\n",
                                buffer);
        }
}

static void
pandecode_dump_file_close(void)
{
        if (pandecode_dump_stream && pandecode_dump_stream != stderr) {
                if (fclose(pandecode_dump_stream))
                        perror("pandecode: dump file");

                pandecode_dump_stream = NULL;
        }
}

void
pandecode_initialize(bool to_stderr)
{
        force_stderr = to_stderr;
        rb_tree_init(&mmap_tree);
        util_dynarray_init(&ro_mappings, NULL);
}

void
pandecode_next_frame(void)
{
        pandecode_dump_file_close();
        pandecode_dump_frame_count++;
}

void
pandecode_close(void)
{
        rb_tree_foreach_safe(struct pandecode_mapped_memory, it, &mmap_tree, node) {
                pandecode_free_mem(it);
        }

        util_dynarray_fini(&ro_mappings);
        pandecode_dump_file_close();
}

void
pandecode_dump_mappings(void)
{
        pandecode_dump_file_open();

        rb_tree_foreach(struct pandecode_mapped_memory, it, &mmap_tree, node) {
                if (!it->addr || !it->length)
                        continue;

                fprintf(pandecode_dump_stream, "Buffer: %s gpu %" PRIx64 "\n\n",
                        it->name, it->gpu_va);

                pan_hexdump(pandecode_dump_stream, it->addr, it->length, false);
                fprintf(pandecode_dump_stream, "\n");
        }
}

void pandecode_abort_on_fault_v4(mali_ptr jc_gpu_va);
void pandecode_abort_on_fault_v5(mali_ptr jc_gpu_va);
void pandecode_abort_on_fault_v6(mali_ptr jc_gpu_va);
void pandecode_abort_on_fault_v7(mali_ptr jc_gpu_va);
void pandecode_abort_on_fault_v9(mali_ptr jc_gpu_va);

void
pandecode_abort_on_fault(mali_ptr jc_gpu_va, unsigned gpu_id)
{
        switch (pan_arch(gpu_id)) {
        case 4: pandecode_abort_on_fault_v4(jc_gpu_va); return;
        case 5: pandecode_abort_on_fault_v5(jc_gpu_va); return;
        case 6: pandecode_abort_on_fault_v6(jc_gpu_va); return;
        case 7: pandecode_abort_on_fault_v7(jc_gpu_va); return;
        case 9: pandecode_abort_on_fault_v9(jc_gpu_va); return;
        default: unreachable("Unsupported architecture");
        }
}

void pandecode_jc_v4(mali_ptr jc_gpu_va, unsigned gpu_id);
void pandecode_jc_v5(mali_ptr jc_gpu_va, unsigned gpu_id);
void pandecode_jc_v6(mali_ptr jc_gpu_va, unsigned gpu_id);
void pandecode_jc_v7(mali_ptr jc_gpu_va, unsigned gpu_id);
void pandecode_jc_v9(mali_ptr jc_gpu_va, unsigned gpu_id);

void
pandecode_jc(mali_ptr jc_gpu_va, unsigned gpu_id)
{
        switch (pan_arch(gpu_id)) {
        case 4: pandecode_jc_v4(jc_gpu_va, gpu_id); return;
        case 5: pandecode_jc_v5(jc_gpu_va, gpu_id); return;
        case 6: pandecode_jc_v6(jc_gpu_va, gpu_id); return;
        case 7: pandecode_jc_v7(jc_gpu_va, gpu_id); return;
        case 9: pandecode_jc_v9(jc_gpu_va, gpu_id); return;
        default: unreachable("Unsupported architecture");
        }
}

#define to_annotation(x) \
	rb_node_data(struct pandecode_annotation, x, node)

static int
pandecode_annotation_cmp_key(const struct rb_node *lhs, const void *key)
{
        struct pandecode_annotation *ann = to_annotation(lhs);
        uint64_t *gpu_va = (uint64_t *) key;

        /* TODO: What should this be? */
        if (ann->gpu_va <= *gpu_va && *gpu_va < (ann->gpu_va + 4))
                return 0;
        else
                return CLAMP((int64_t)(ann->gpu_va - *gpu_va), -1, 1);
}

static int
pandecode_annotation_compare(const struct rb_node *a, const struct rb_node *b)
{
        struct pandecode_annotation *aa = to_annotation(a);
        struct pandecode_annotation *bb = to_annotation(b);
        int64_t diff = aa->gpu_va - bb->gpu_va;
        if (diff)
                return CLAMP(diff, -1, 1);
        return strcmp(aa->text, bb->text);
}

void
pandecode_annotate_memory(mali_ptr gpu_va, const char *format, ...)
{
        struct pandecode_mapped_memory *mem =
                pandecode_find_mapped_gpu_mem_containing_rw(gpu_va);

        if (!mem)
                return;

        char *str;

        va_list ap;

        va_start(ap, format);
        int ret = vasprintf(&str, format, ap);
        va_end(ap);

        if (ret == -1)
                return;

        /* TODO: Check for duplicate entries? */
        struct pandecode_annotation *ann = NULL;

        /* TODO: Store the BO offset rather than the VA? */
        ann = calloc(1, sizeof(*ann));
        ann->gpu_va = gpu_va;
        ann->text = str;

        rb_tree_insert(&mem->annotations, &ann->node,
                       pandecode_annotation_compare);
}

const char *
pandecode_get_annotation(struct pandecode_mapped_memory *mem, mali_ptr gpu_va)
{
        if (!mem)
                mem = pandecode_find_mapped_gpu_mem_containing_rw(gpu_va);

        if (!mem)
                return NULL;

        struct rb_node *node = rb_tree_search(&mem->annotations, &gpu_va,
                                              pandecode_annotation_cmp_key);

        if (!node)
                return NULL;

        return to_annotation(node)->text;
}
