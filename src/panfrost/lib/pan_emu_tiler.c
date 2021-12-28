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

enum foo { END, REL, ABS, LOOP };

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
                {0, LOOP},
                {1, REL, 2, REL, 1},
                {1, REL, 1, REL, 2},
                {0, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_STRIP + PROVOKE_LAST] = {
                {2, REL, -2, REL, -1},
                {0, LOOP},
                {1, REL, -1, REL, -2},
                {1, REL, -2, REL, -1},
                {0, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_FAN] = {
                {1, REL, 1, ABS, 0},
                {0, END},
        },
        [MALI_DRAW_MODE_TRIANGLE_FAN + PROVOKE_LAST] = {
                {2, ABS, 0, REL, -1},
                {0, LOOP},
                {1, ABS, 0, REL, -1},
                {0, END},
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
};

struct trigen_context {
        unsigned type;
        int pos;
        int size;

        unsigned loop_pt;
        unsigned state;
};

static bool
generate_triangle(struct trigen_context *t, int *a, int *b, int *c)
{
        if (t->pos >= t->size)
                return false;
        struct draw_state_data d = states[t->type][t->state];
        ++t->state;

        t->pos += d.offset;
        if (t->pos >= t->size)
                return false;

        *a = t->pos;

        switch (d.typb) {
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

        if (*b >= t->size || *c >= t->size)
                return false;

        return true;
}

struct tiler_context {
        unsigned width;
        unsigned height;
        float widthf;
        float heightf;

        /* TODO: Multiple "bins", then hierarchy.. */
        uint32_t *heap;
        unsigned pos;
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
do_tiler_job(struct tiler_context *c, mali_ptr job)
{
        mali_ptr draw_ptr = job + 128;

        struct mali_tiler_job_packed *PANDECODE_PTR_VAR(p, NULL, job);
        pan_section_unpack(p, TILER_JOB, DRAW, draw);
        pan_section_unpack(p, TILER_JOB, PRIMITIVE, primitive);

        printf("draw: %"PRIx64", pos: %"PRIx64"\n", draw_ptr, draw.position);

        // TODO: genxml packing for instructions?
        struct tiler_instr_draw_struct set_draw = {
                .addr = draw_ptr >> 6,
                .draw_type = tiler_draw_type(primitive.draw_mode),
                .reset = true,
                .op = 4,
        }; memcpy(c->heap + (c->pos++), &set_draw, 4);

        struct trigen_context tris = {
                .type = primitive.draw_mode + PROVOKE_LAST,
                .size = 4, // todo: read..
        };

        int pos = 0;
        int aa, bb, cc;
        while (generate_triangle(&tris, &aa, &bb, &cc)) {

                struct tiler_instr_do_draw do_draw = {
                        .op = 15, // what does this mean?
                        .layer = 0,
                        .offset = aa - pos,
                        .b = bb - aa,
                        .c = cc - aa,
                }; memcpy(c->heap + (c->pos++), &do_draw, 4);
                pos = aa;
        }

        memset(c->heap + c->pos, 0, 4);
}

void
GENX(panfrost_emulate_tiler)(struct util_dynarray *tiler_jobs, unsigned gpu_id)
{
        bool old_do_mprotect = pandecode_no_mprotect;
        pandecode_no_mprotect = true;

        if (!tiler_jobs->size) {
                printf("no tiler jobs\n");
                return;
        }

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

        printf("fb size %i %i\n", c.width, c.height);

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

                printf("Setting offset for level %i to %x\n", b, level_offsets[b] * 4);
        }
        header_size = ALIGN_POT(header_size, 0x40 / 4);

        unsigned level = util_logbase2_ceil(MAX2(c.width, c.height)) - 4;
        unsigned tile_offset = 0;

        c.pos = header_size;

        tiler_context[0] = (0xffULL << 48) | (tiler_heap[1] + 0x8000);

        heap[level_offsets[level] + 1 + tile_offset] = c.pos * 4 - 0x8000;

        util_dynarray_foreach(tiler_jobs, mali_ptr, job)
                do_tiler_job(&c, *job);

        // todo; subtract?
        heap[level_offsets[level] + tile_offset] = c.pos * 4 - 0x8000 - 4;

        hexdump(stdout, (uint8_t *)heap, heap_size);

        pandecode_no_mprotect = old_do_mprotect;
}


