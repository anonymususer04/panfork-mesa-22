#include <stdio.h>

#include "gen_macros.h"
#include "decode.h"
#include "util/u_dynarray.h"

#include "pan_emu.h"

extern bool pandecode_no_mprotect;

static unsigned
pan_tile_count(unsigned width, unsigned height, unsigned tile_width, unsigned tile_height)
{
        unsigned aligned_width = ALIGN_POT(width, tile_width);
        unsigned aligned_height = ALIGN_POT(height, tile_height);

        unsigned tile_count_x = aligned_width / tile_width;
        unsigned tile_count_y = aligned_height / tile_height;

        return tile_count_x * tile_count_y;
}

__attribute__((packed))
struct tiler_instr_draw_struct {
        unsigned addr      : 26;
        unsigned draw_type : 2;
        unsigned reset     : 1;
        unsigned op        : 3;
};

__attribute__((packed))
struct tiler_instr_do_draw {
        signed c           : 7;
        signed b           : 7;
        signed offset      : 7;
        unsigned layer     : 5;
        unsigned op        : 4;
        unsigned zero      : 2;
};

enum tiler_draw_mode {
        TILER_DRAW_MODE_POINTS = 1,
        TILER_DRAW_MODE_LINES = 2,
        TILER_DRAW_MODE_TRIS = 3,
};

static unsigned
tiler_draw_type(enum mali_draw_mode mode)
{
        switch (mode) {
        case MALI_DRAW_MODE_POINTS:
                return TILER_DRAW_MODE_POINTS;
        case MALI_DRAW_MODE_LINES:
        case MALI_DRAW_MODE_LINE_STRIP:
        case MALI_DRAW_MODE_LINE_LOOP:
                return TILER_DRAW_MODE_LINES;
        default:
                return TILER_DRAW_MODE_TRIS;
        }
}

enum foo { INVALID, END, REL, ABS, LOOP };

#define PROVOKE_LAST 16

struct draw_state_data {
        int offset;
        enum foo typb;
        int b;
        enum foo typc;
        int c;
};

const static struct draw_state_data states[][10] = {
        [MALI_DRAW_MODE_TRIANGLES] = {
                {0, REL, 1, REL, 2},
                {3, END},
        },
        [MALI_DRAW_MODE_TRIANGLES + PROVOKE_LAST] = {
                {2, REL, -2, REL, -1},
                {1, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_STRIP] = {
                {0, REL, 1, REL, 2},
                {1, REL, 2, REL, 1},
                {1, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_STRIP + PROVOKE_LAST] = {
                {1, REL, -2, REL, -1},
                {1, REL, -1, REL, -2},
                {-1, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_FAN] = {
                {1, REL, 1, ABS, 0},
                {0, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_FAN + PROVOKE_LAST] = {
                {2, ABS, 0, REL, -1},
                {-1, END},
        },
        [MALI_DRAW_MODE_QUADS] = {
                {0, REL, 1, REL, 2},
                {0, REL, 2, REL, 3},
                {4, END},
        },
        [MALI_DRAW_MODE_QUADS + PROVOKE_LAST] = {
                {3, REL, -3, REL, -2},
                {0, REL, -2, REL, -1},
                {1, END},
        },
        [MALI_DRAW_MODE_QUAD_STRIP] = {
                {0, REL, 1, REL, 3},
                {0, REL, 3, REL, 2},
                {2, END},
        },
        [MALI_DRAW_MODE_QUAD_STRIP + PROVOKE_LAST] = {
                {3, REL, -3, REL, -2},
                {0, REL, -1, REL, -3},
                {-1, END},
        },
};

struct trigen_context {
        unsigned type;
        int pos;
        unsigned index_count;

        unsigned loop_pt;
        unsigned state;

        enum mali_index_type index_type;
        void *indices;
        int invoc;

        int base_vertex_offset;
};

static bool
check_pos(struct trigen_context *t, int *a, int *b, int *c)
{
        return *a < t->index_count &&
                *b < t->index_count &&
                *c < t->index_count;
}

static bool
generate_triangle(struct trigen_context *t, int *a, int *b, int *c)
{
        if ((t->type & 0xf) == MALI_DRAW_MODE_POLYGON) {
                ++t->state;
                *a = 0;
                *b = t->state;
                *c = t->state + 1;
                return check_pos(t, a, b, c);
        }

        struct draw_state_data d = states[t->type][t->state];
        ++t->state;

        t->pos += d.offset;

        *a = t->pos;

        switch (d.typb) {
        case INVALID:
                assert(0);
        case END:
                t->state = t->loop_pt;
                return generate_triangle(t, a, b, c);
        case LOOP:
                t->loop_pt = t->state - 1;
                return generate_triangle(t, a, b, c);
        case REL:
                *b = *a + d.b;
                break;
        case ABS:
                *b = d.b;
                break;
        }

        switch (d.typc) {
        case REL:
                *c = *a + d.c;
                break;
        case ABS:
                *c = d.c;
                break;
        default:
                assert(0);
        }

        return check_pos(t, a, b, c);
}

static int
index_transform_u16(struct trigen_context *t, int *val)
{
        uint16_t *indices = t->indices;
        assert(indices);
        assert(*val < t->index_count);

        // todo: only when primrestart enabled
        uint16_t v = indices[*val];
        if (v == 0xffff)
                return *val + 1;

        v += t->base_vertex_offset;

        assert(v < t->invoc);

        *val = v;
        return 0;
}

static int
index_transform_u32(struct trigen_context *t, int *val)
{
        uint32_t *indices = t->indices;
        assert(indices);
        assert(*val < t->index_count);

        uint32_t v = indices[*val];
        if (v == 0xffffffff)
                return *val + 1;

        v += t->base_vertex_offset;

        assert(v < t->invoc);

        *val = v;
        return 0;
}

static bool
generate_triangle_indexed(struct trigen_context *t, int *a, int *b, int *c)
{
        if (!generate_triangle(t, a, b, c))
                return false;

        switch (t->index_type) {
        case MALI_INDEX_TYPE_NONE:
                return true;
        case MALI_INDEX_TYPE_UINT16: {
                int r = index_transform_u16(t, a);
                if (!r) r = index_transform_u16(t, b);
                if (!r) r = index_transform_u16(t, c);
                if (r) {
                        /* We hit a primitive restart index */
                        t->pos = r;
                        t->state = 0;
                        return generate_triangle_indexed(t, a, b, c);
                }
                return true;
        }
        case MALI_INDEX_TYPE_UINT32: {
                /* TODO: reduce duplication -- move switch to a new func? */
                int r = index_transform_u32(t, a);
                if (!r) r = index_transform_u32(t, b);
                if (!r) r = index_transform_u32(t, c);
                if (r) {
                        /* We hit a primitive restart index */
                        t->pos = r;
                        t->state = 0;
                        return generate_triangle_indexed(t, a, b, c);
                }
                return true;
        }
        default:
                assert(0);
        }
}

struct tiler_context {
        unsigned width;
        unsigned height;
        float widthf;
        float heightf;

        /* TODO: Multiple "bins", then hierarchy.. */
        uint32_t *heap;
        unsigned pos;

        unsigned start;
};

static struct tiler_context
decode_tiler_job(mali_ptr job)
{
        struct tiler_context c = {0};

        struct mali_tiler_job_packed *PANDECODE_PTR_VAR(p, NULL, job);
        pan_section_unpack(p, TILER_JOB, TILER, tiler);

        struct mali_tiler_context_packed *PANDECODE_PTR_VAR(tp, NULL, tiler.address);
        pan_unpack(tp, TILER_CONTEXT, t);

        c.width = t.fb_width;
        c.height = t.fb_height;
        c.widthf = c.width;
        c.heightf = c.height;

//        pan_section_unpack(p, TILER_JOB, DRAW, draw);
//        pandecode_dcd(&draw, job_no, h->type, "", gpu_id);

//        pandecode_log("Tiler Job Payload:\n");
//        pandecode_indent++;
//        pandecode_invocation(pan_section_ptr(p, TILER_JOB, INVOCATION));
//        pandecode_primitive(pan_section_ptr(p, TILER_JOB, PRIMITIVE));
//        DUMP_UNPACKED(DRAW, draw, "Draw:\n");

//        pan_section_unpack(p, TILER_JOB, PRIMITIVE, primitive);
//        pandecode_primitive_size(pan_section_ptr(p, TILER_JOB, PRIMITIVE_SIZE),
//                                 primitive.point_size_array_format == MALI_POINT_SIZE_ARRAY_FORMAT_NONE);
//        pandecode_indent--;
//        pandecode_log("\n");

        return c;
}

static void
heap_jump(struct tiler_context *c)
{
        unsigned target = (c->pos + 1) * 4;

        uint32_t jump = target - 0x8000 + 3;
        memcpy(c->heap + (c->pos++), &jump, 4);
}

static void
heap_add(struct tiler_context *c, void *ptr)
{
        if (((c->pos - c->start) & 0x7f) == 0x1f)
                heap_jump(c);
        memcpy(c->heap + (c->pos++), ptr, 4);
}

static void
heap_pad(struct tiler_context *c, unsigned size)
{
        memset(c->heap + c->pos, 0, size);
}

static void
set_draw(struct tiler_context *c, mali_ptr job_addr, enum mali_draw_mode mode)
{
        // TODO: genxml packing for instructions?
        struct tiler_instr_draw_struct ins = {
                .addr = (job_addr + 128) >> 6,
                .draw_type = tiler_draw_type(mode),
                .reset = true,
                .op = 4,
        };
        heap_add(c, &ins);
}

static void
do_tiler_job(struct tiler_context *c, mali_ptr job)
{
        struct mali_tiler_job_packed *PANDECODE_PTR_VAR(p, NULL, job);
        pan_section_unpack(p, TILER_JOB, DRAW, draw);
        pan_section_unpack(p, TILER_JOB, PRIMITIVE, primitive);
        pan_section_unpack(p, TILER_JOB, INVOCATION, invocation);

        bool done_set_draw = false;

//        printf("draw: %"PRIx64", pos: %"PRIx64"\n", job + 128, draw.position);

        struct trigen_context tris = {
                .type = primitive.draw_mode + PROVOKE_LAST,
                // TODO: decode properly
                .invoc = invocation.invocations + 1,

                .index_type = primitive.index_type,
                .index_count = primitive.index_count,
                .indices = primitive.indices ? pandecode_fetch_gpu_mem(NULL, primitive.indices, 1) : NULL,
                .base_vertex_offset = primitive.base_vertex_offset,
        };

        int pos = 0;
        int aa, bb, cc;
        while (generate_triangle_indexed(&tris, &aa, &bb, &cc)) {

                if (aa == bb || aa == cc || bb == cc)
                        continue;

                if (!done_set_draw) {
                        set_draw(c, job, primitive.draw_mode);
                        done_set_draw = true;
                }

                struct tiler_instr_do_draw do_draw = {
                        .op = 1, // what does this mean?
                        .layer = 0,
                        .offset = aa - pos,
                        .b = bb - aa,
                        .c = cc - aa,
                }; heap_add(c, &do_draw);
                pos = aa;
        }

        heap_pad(c, 4);
}

void
GENX(panfrost_emulate_tiler)(struct util_dynarray *tiler_jobs, unsigned gpu_id)
{
        bool old_do_mprotect = pandecode_no_mprotect;
        pandecode_no_mprotect = true;

        if (!tiler_jobs->size)
                return;

        mali_ptr last_job = util_dynarray_top(tiler_jobs, mali_ptr);

        struct tiler_context c = decode_tiler_job(last_job);

        uint64_t *job_mem = pandecode_fetch_gpu_mem(NULL, last_job, 256);

        mali_ptr tiler_ptr = job_mem[9];
        uint64_t *tiler_context = pandecode_fetch_gpu_mem(NULL, tiler_ptr, 32);
        uint16_t *tiler_context_16 = (uint16_t *)tiler_context;

        uint64_t *tiler_heap = pandecode_fetch_gpu_mem(NULL, tiler_context[3], 32);

        unsigned heap_size = (*tiler_heap) >> 32;

        uint32_t *heap = pandecode_fetch_gpu_mem(NULL, tiler_heap[1], 1);
        c.heap = heap;

        unsigned hierarchy_mask = tiler_context_16[4] & ((1 << 13) - 1);

        /* Size is in words */
        /* See pan_tiler.c */
        unsigned level_offsets[13] = {0};
        /* Don't ask me what this space is used for */
        unsigned header_size = 0x8000 / 4;
        u_foreach_bit(b, hierarchy_mask) {
                unsigned tile_size = (1 << b) * 16;
                unsigned tile_count = pan_tile_count(c.width, c.height, tile_size, tile_size);
                unsigned level_count = tile_count * 2;
                level_offsets[b] = header_size;
                header_size += level_count;

//                printf("Setting offset for level %i to %x\n", b, level_offsets[b] * 4);
        }
        header_size = ALIGN_POT(header_size, 0x40 / 4);

        unsigned level = util_logbase2_ceil(MAX3(c.width, c.height, 16)) - 4;
        unsigned tile_offset = 0;

        c.start = header_size;
        c.pos = header_size;

        tiler_context[0] = (0xffULL << 48) | (tiler_heap[1] + 0x8000);

        // TODO: Remove the need for all these offsetings..
        memset(heap + 0x8000/4, 0, c.pos * 4 - 0x8000);

        heap[level_offsets[level] + 1 + tile_offset] = c.pos * 4 - 0x8000;

        util_dynarray_foreach(tiler_jobs, mali_ptr, job) {
                do_tiler_job(&c, *job);
        }

        // todo; subtract?
        heap[level_offsets[level] + tile_offset] = c.pos * 4 - 0x8000 - 4;

//        printf("width %i\nheight %i\n", c.width, c.height);
//        hexdump(stdout, (uint8_t *)heap, heap_size);

        pandecode_no_mprotect = old_do_mprotect;
}
