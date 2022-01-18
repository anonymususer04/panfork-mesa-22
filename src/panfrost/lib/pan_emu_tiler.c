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
        signed offset      : 8;
        unsigned layer     : 4;
        unsigned op        : 4;
        unsigned zero      : 2;
};

__attribute__((packed))
struct tiler_instr_set_offset {
        signed c           : 7;
        signed b           : 7;
        signed offset      : 7;
        unsigned layer     : 7;
        unsigned op        : 4;
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

enum foo { INVALID, END, REL, ABS };

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
                {2, REL, -2, REL, -1},
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

struct tiler_header {
        uint32_t last;
        uint32_t start;
};

struct tiler_ext_header {
        mali_ptr job;
        unsigned pos;
};

struct tiler_context {
        unsigned width;
        unsigned height;
        float widthf;
        float heightf;

        unsigned tx;
        unsigned ty;

        uint8_t *blocks;
        unsigned block_pos;

        unsigned op;

        unsigned heap_offsets[13];
        unsigned heap_strides[13];
        struct tiler_header *heap_levels[13];
        struct tiler_ext_header *heap_ext[13];
};

typedef struct {
        uint16_t l;
        uint16_t x;
        uint16_t y;
} coord_t;

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
        c.tx = DIV_ROUND_UP(c.width, 16);
        c.ty = DIV_ROUND_UP(c.height, 16);

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
heap_jump(struct tiler_context *c, uint32_t *ins, unsigned target)
{
        *ins = target + 3;
}

static struct tiler_header *
heap_get_header(struct tiler_context *c, coord_t loc)
{
        struct tiler_header *level = c->heap_levels[loc.l];
        unsigned stride = c->heap_strides[loc.l];
        return level + loc.y * stride + loc.x;
}

static struct tiler_ext_header *
heap_get_ext(struct tiler_context *c, coord_t loc)
{
        struct tiler_ext_header *level = c->heap_ext[loc.l];
        unsigned stride = c->heap_strides[loc.l];
        return level + loc.y * stride + loc.x;
}

static unsigned
heap_alloc_block(struct tiler_context *c)
{
        unsigned ret = c->block_pos;
        c->block_pos += 512;
        // todo; store size..
        assert(c->block_pos < 63 * 1024 * 1024);
//        printf("alloc block %x\n", ret + 0x8000);
        return ret;
}

static uint8_t *
heap_get_space(struct tiler_context *c, coord_t loc)
{
        struct tiler_header *header = heap_get_header(c, loc);

        if (!header->start) {
                header->last = header->start = heap_alloc_block(c);
        } else if ((header->last - header->start) == 0x78) {
                unsigned block = heap_alloc_block(c);
                heap_jump(c, (uint32_t *)(c->blocks + header->last + 4), block);
                header->last = block;
        } else if ((header->last & 0x1fc) == 0x1f8) {
                unsigned block = heap_alloc_block(c);
                heap_jump(c, (uint32_t *)(c->blocks + header->last + 4), block);
                header->last = block;
        } else {
                header->last += 4;
        }

        return c->blocks + header->last;
}

static void
heap_add(struct tiler_context *c, coord_t loc, void *ptr)
{
//        printf("heap_add: %i %i %i\n", loc.l, loc.x, loc.y);
        uint8_t *space = heap_get_space(c, loc);
        memcpy(space, ptr, 4);
}

static void
heap_pad(struct tiler_context *c, coord_t loc)
{
        uint8_t *space = heap_get_space(c, loc);
        memset(space, 0, 4);
}

static void
set_draw(struct tiler_context *c, coord_t loc, mali_ptr job_addr, enum mali_draw_mode mode)
{
        // TODO: genxml packing for instructions?
        struct tiler_instr_draw_struct ins = {
                .addr = (job_addr + 128) >> 6,
                .draw_type = tiler_draw_type(mode),
                .reset = true,
                .op = 4,
        };
        heap_add(c, loc, &ins);
}

#define DIV_ROUND_DOWN(A, B) ( ((A) > 0) ? ((A) / (B)) : (-DIV_ROUND_UP(-(A), (B))) )

static void
do_draw(struct tiler_context *tc, coord_t loc, int op, int layer, int a, int b, int c)
{
        if (CLAMP(a, -128, 127) != a || CLAMP(b, -64, 63) != b || CLAMP(c, -64, 63) != c) {

                if (CLAMP(a, -16384, 16383) != a || CLAMP(b, -8192, 8191) != b || CLAMP(c, -8192, 8191) != c) {

                        struct tiler_instr_set_offset ins = {
                                .op = 4,
                                .layer = 0,
                                .offset = DIV_ROUND_DOWN(a, 32768),
                                .b = DIV_ROUND_DOWN(b, 16384),
                                .c = DIV_ROUND_DOWN(c, 16384),
                        };
                        heap_add(tc, loc, &ins);

                        assert(ins.offset == DIV_ROUND_DOWN(a, 32768));
                        assert(ins.b == DIV_ROUND_DOWN(b, 16384));
                        assert(ins.c == DIV_ROUND_DOWN(c, 16384));
                }

                struct tiler_instr_set_offset ins = {
                        .op = 4,
                        .layer = 0,
                        .offset = DIV_ROUND_DOWN(a, 256),
                        .b = DIV_ROUND_DOWN(b, 128),
                        .c = DIV_ROUND_DOWN(c, 128),
                };
                heap_add(tc, loc, &ins);
        }

        struct tiler_instr_do_draw ins = {
                .op = op,
                .layer = 0,
                .offset = a,
                .b = b,
                .c = c,
        };
        heap_add(tc, loc, &ins);
}

struct position {
        float x;
        float y;
        float z;
        float w;
};

static void
do_tiler_job(struct tiler_context *c, mali_ptr job)
{
        struct mali_tiler_job_packed *PANDECODE_PTR_VAR(p, NULL, job);
        pan_section_unpack(p, TILER_JOB, DRAW, draw);
        pan_section_unpack(p, TILER_JOB, PRIMITIVE, primitive);
        pan_section_unpack(p, TILER_JOB, INVOCATION, invocation);

        struct position *PANDECODE_PTR_VAR(position, NULL, draw.position);

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

        int aa, bb, cc;
        while (generate_triangle_indexed(&tris, &aa, &bb, &cc)) {

                if (aa == bb || aa == cc || bb == cc)
                        continue;

                struct position vp = position[aa];
                float minx = vp.x;
                float maxx = vp.x;

                float miny = vp.y;
                float maxy = vp.y;

                vp = position[bb];
                minx = MIN2(minx, vp.x);
                maxx = MAX2(maxx, vp.x);
                miny = MIN2(miny, vp.y);
                maxy = MAX2(maxy, vp.y);

                vp = position[cc];
                minx = MIN2(minx, vp.x);
                maxx = MAX2(maxx, vp.x);
                miny = MIN2(miny, vp.y);
                maxy = MAX2(maxy, vp.y);

                int minx_tile = MAX2((int)minx / 16, 0);
                int miny_tile = MAX2((int)miny / 16, 0);
                int maxx_tile = MIN2(DIV_ROUND_UP((int)ceilf(maxx), 16), c->tx);
                int maxy_tile = MIN2(DIV_ROUND_UP((int)ceilf(maxy), 16), c->ty);

                coord_t l = {0};
                for (l.x = minx_tile; l.x < maxx_tile; ++l.x) {
                        for (l.y = miny_tile; l.y < maxy_tile; ++l.y) {
                                struct tiler_ext_header *ext
                                        = heap_get_ext(c, l);

                                if (ext->job != job) {
                                        set_draw(c, l, job, primitive.draw_mode);
                                        ext->job = job;
                                        ext->pos = 0;
                                }

                                do_draw(c, l, c->op, 0, aa - ext->pos, bb - aa, cc - aa);
                                ext->pos = aa;

                        }
                }
        }
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

        uint32_t *heap = pandecode_fetch_gpu_mem(NULL, tiler_heap[1] + 0x8000, 1);
        tiler_context[0] = (0xffULL << 48) | (tiler_heap[1] + 0x8000);

        unsigned hierarchy_mask = tiler_context_16[4] & ((1 << 13) - 1);

        /* Don't ask me what this space is used for */
        unsigned header_size = 0;
        u_foreach_bit(b, hierarchy_mask) {
                unsigned tile_size = (1 << b) * 16;
                unsigned tile_count = pan_tile_count(c.width, c.height, tile_size, tile_size);

                c.heap_offsets[b] = header_size;
                c.heap_strides[b] = DIV_ROUND_UP(c.width, tile_size);

                header_size += tile_count;

//                if (tile_count == 1)
//                        break;
        }
        header_size = ALIGN_POT(header_size, 128);// / 8);

        memset(heap, 0, 1024*1024);//header_size * 8);

        struct tiler_header *heap_shadow = calloc(header_size, sizeof(*heap_shadow));
        struct tiler_ext_header *heap_ext = calloc(header_size, sizeof(*heap_ext));

        u_foreach_bit(b, hierarchy_mask) {
                c.heap_levels[b] = heap_shadow + c.heap_offsets[b];
                c.heap_ext[b] = heap_ext + c.heap_offsets[b];
        }

        c.blocks = (uint8_t *)heap;
        c.block_pos = header_size * 8;

        c.op = 15;

        util_dynarray_foreach(tiler_jobs, mali_ptr, job) {
                do_tiler_job(&c, *job);
        }

        memcpy(heap, heap_shadow, header_size * 8);
        free(heap_shadow);
        free(heap_ext);

        UNUSED unsigned heap_size = (*tiler_heap) >> 32;

        if (getenv("TILER_DUMP")) {
                fflush(stdout);
                FILE *stream = popen("tiler-hex-read", "w");
                fprintf(stream, "width %i\nheight %i\n", c.width, c.height);
                hexdump(stream, (uint8_t *)heap - 0x8000, heap_size);
                pclose(stream);
        }

        pandecode_no_mprotect = old_do_mprotect;
}
