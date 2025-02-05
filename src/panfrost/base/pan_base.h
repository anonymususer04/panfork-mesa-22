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

/* Library for interfacing with kbase */
#ifndef PAN_BASE_H
#define PAN_BASE_H

#include "util/u_dynarray.h"

typedef uint64_t base_va;
struct base_ptr {
        void *cpu;
        base_va gpu;
};

struct kbase_syncobj;

/* The job is done when the queue seqnum > seqnum */
struct kbase_sync_link {
        struct kbase_sync_link *next; /* must be first */
        struct kbase_syncobj *o;
        uint64_t seqnum;
};

struct kbase_event_slot {
        struct kbase_sync_link *syncobjs;
        struct kbase_sync_link **back;
        uint64_t last;
};

struct kbase_context {
        uint8_t csg_handle;
        uint32_t csg_uid;
        unsigned num_csi;

        unsigned tiler_heap_chunk_size;
        base_va tiler_heap_va;
        base_va tiler_heap_header;
};

struct kbase_cs {
        struct kbase_context *ctx;
        void *user_io;
        base_va va;
        unsigned size;
        unsigned event_mem_offset;

        unsigned last_insert;
        unsigned last_extract;
};

struct kbase;
typedef struct kbase *kbase;

#define KBASE_SLOT_COUNT 2

typedef struct {
        base_va va;
        int fd;
        uint8_t use_count;
        /* For emulating implicit sync. TODO make this work on v10 */
        uint8_t last_access[KBASE_SLOT_COUNT];
} kbase_handle;

struct kbase {
        unsigned setup_state;
        bool verbose;

        int fd;
        unsigned api;
        unsigned page_size;
        // TODO: Actually we may want to try to pack multiple contexts / queue
        // "sets" into a single group...
        unsigned cs_queue_count;

        unsigned gpuprops_size;
        void *gpuprops;

        void *tracking_region;
        void *csf_user_reg;
        struct base_ptr event_mem;
        // TODO: dynamically size
        struct kbase_event_slot event_slots[256];
        // TODO: USe a bitset?
        unsigned event_slot_usage;

        uint8_t atom_number;

        pthread_mutex_t handle_lock;

        struct util_dynarray gem_handles;
        struct util_dynarray atom_bos[256];


        void (*close)(kbase k);

        bool (*get_pan_gpuprop)(kbase k, unsigned name, uint64_t *value);
        bool (*get_mali_gpuprop)(kbase k, unsigned name, uint64_t *value);

        struct base_ptr (*alloc)(kbase k, size_t size,
                                 unsigned pan_flags,
                                 unsigned mali_flags);
        void (*free)(kbase k, base_va va);

        int (*import_dmabuf)(kbase k, int fd);

        void (*cache_clean)(void *ptr, size_t size);
        void (*cache_invalidate)(void *ptr, size_t size);

        void (*poll_event)(kbase k, int64_t timeout_ns);
        void (*handle_events)(kbase k);

        /* <= v9 GPUs */
        int (*submit)(kbase k, uint64_t va, unsigned req,
                      struct kbase_syncobj *o,
                      int32_t *handles, unsigned num_handles);

        /* >= v10 GPUs */
        struct kbase_context *(*context_create)(kbase k);
        void (*context_destroy)(kbase k, struct kbase_context *ctx);
        // TODO: Pass in a priority?
        struct kbase_cs (*cs_bind)(kbase k, struct kbase_context *ctx,
                                   base_va va, unsigned size);
        void (*cs_term)(kbase k, struct kbase_cs *cs, base_va va);

        bool (*cs_submit)(kbase k, struct kbase_cs *cs, unsigned insert_offset,
                          struct kbase_syncobj *o, uint64_t seqnum);
        bool (*cs_wait)(kbase k, struct kbase_cs *cs, unsigned extract_offset);
        void (*cs_wait_idle)(kbase k, struct kbase_cs *cs);

        /* syncobj functions */
        struct kbase_syncobj *(*syncobj_create)(kbase k);
        void (*syncobj_destroy)(kbase k, struct kbase_syncobj *o);
        struct kbase_syncobj *(*syncobj_dup)(kbase k, struct kbase_syncobj *o);
        /* TODO: timeout? (and for cs_wait) */
        bool (*syncobj_wait)(kbase k, struct kbase_syncobj *o);

        void (*ctr_open)(kbase k);
        void (*ctr_set_enabled)(kbase k, bool enable);
        void (*ctr_dump)(kbase k);

        void (*mem_sync)(kbase k, base_va gpu, void *cpu, unsigned size,
                         bool invalidate);
};

bool kbase_open(kbase k, int fd, unsigned cs_queue_count, bool verbose);

/* Called from kbase_open */
bool kbase_open_old(kbase k);
bool kbase_open_new(kbase k);
bool kbase_open_csf(kbase k);

/* BO management */
int kbase_alloc_gem_handle(kbase k, base_va va, int fd);
int kbase_alloc_gem_handle_locked(kbase k, base_va va, int fd);
void kbase_free_gem_handle(kbase k, int handle);
kbase_handle kbase_gem_handle_get(kbase k, int handle);
int kbase_wait_bo(kbase k, int handle, int64_t timeout_ns, bool wait_readers);

#endif
