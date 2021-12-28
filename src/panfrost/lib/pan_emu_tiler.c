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
        unsigned pos;
        unsigned max;

        unsigned loop_pt;
        unsigned state;
};

static bool
generate_triangle(struct trigen_context *t, unsigned *a, unsigned *b, unsigned *c)
{
        if (t->pos >= t->max)
                return false;
        struct draw_state_data d = states[t->type][t->state];
        ++t->state;

        t->pos += d.offset;
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

        return true;
}

struct tiler_context {
        unsigned width;
        unsigned height;
        float widthf;
        float heightf;

        struct MALI_PRIMITIVE primitive;
};

static struct tiler_context
decode_tiler_job(mali_ptr job)
{
        struct tiler_context c = {0};

        struct mali_tiler_job_packed *PANDECODE_PTR_VAR(p, NULL, job);
        pan_section_unpack(p, TILER_JOB, TILER, tiler);

        pan_section_unpack(p, TILER_JOB, PRIMITIVE, prim);
        c.primitive = prim;

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

        unsigned pos = header_size;

        tiler_context[0] = (0xffULL << 48) | (tiler_heap[1] + 0x8000);

        heap[level_offsets[level] + 1 + tile_offset] = pos * 4 - 0x8000;

        // TODO: genxml packing for instructions?

        util_dynarray_foreach(tiler_jobs, mali_ptr, job) {
                uint64_t *job_mem = pandecode_fetch_gpu_mem(NULL, *job, 256);

                mali_ptr draw_ptr = *job + 128;

                mali_ptr position_ptr = job_mem[16 + 2];

                printf("draw: %"PRIx64", pos: %"PRIx64"\n", draw_ptr, position_ptr);

                struct tiler_instr_draw_struct draw = {
                        .addr = draw_ptr >> 6,
                        .draw_type = tiler_draw_type(c.primitive.draw_mode),
                        .reset = true,
                        .op = 4,
                }; memcpy(heap + (pos++), &draw, 4);

                uint8_t bytes[] = { 0x7F, 0xBF, 0x00, 0x3C, 0xFF, 0x7E, 0x00, 0x3C };
                memcpy(heap + (pos += 2) - 2, &bytes, 8);
        }

        // todo; subtract?
        heap[level_offsets[level] + tile_offset] = pos * 4 - 0x8000 - 4;

        hexdump(stdout, (uint8_t *)heap, heap_size);

        pandecode_no_mprotect = old_do_mprotect;
}


