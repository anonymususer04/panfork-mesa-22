/*
 * Copyright (C) 2021 Collabora, Ltd.
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

#include "compiler.h"
#include "bi_builder.h"

/* This optimization pass, intended to run once after code emission but before
 * copy propagation, analyzes direct word-aligned UBO reads and promotes a
 * subset to moves from FAU. It is the sole populator of the UBO push data
 * structure returned back to the command stream. */

static bool
bi_is_ubo(bi_instr *ins)
{
        return (bi_opcode_props[ins->op].message == BIFROST_MESSAGE_LOAD) &&
                (ins->seg == BI_SEG_UBO);
}

static bool
bi_is_direct_aligned_ubo(bi_instr *ins)
{
        return bi_is_ubo(ins) &&
                (ins->src[0].type == BI_INDEX_CONSTANT) &&
                (ins->src[1].type == BI_INDEX_CONSTANT) &&
                ((ins->src[0].value & 0x3) == 0);
}

/* Represents use data for a single UBO */

#define MAX_UBO_QWORDS (65536 / 16)

struct bi_ubo_block {
        BITSET_DECLARE(pushed, MAX_UBO_QWORDS);
        uint8_t used[MAX_UBO_QWORDS];
};

struct bi_ubo_analysis {
        /* Per block analysis */
        unsigned refcnt;
        unsigned nr_blocks;
        bool done_pick;
        struct bi_ubo_block *blocks;
};

static void
bi_analyze_ranges(bi_context *ctx)
{
        if (!*(ctx->analysis))
                *(ctx->analysis) = calloc(1, sizeof(struct bi_ubo_analysis));

        struct bi_ubo_analysis *res = *ctx->analysis;

        if (res->nr_blocks) {
                assert(res->nr_blocks == ctx->nir->info.num_ubos + 1);
        } else {
                res->nr_blocks = ctx->nir->info.num_ubos + 1;
                res->blocks = calloc(res->nr_blocks, sizeof(struct bi_ubo_block));
        }

        ++res->refcnt;

        bi_foreach_instr_global(ctx, ins) {
                if (!bi_is_direct_aligned_ubo(ins)) continue;

                unsigned ubo = ins->src[1].value;
                unsigned qword = ins->src[0].value / 16;
                unsigned offset = (ins->src[0].value / 4) & 3;
                unsigned channels = bi_opcode_props[ins->op].sr_count;

                assert(ubo < res->nr_blocks);
                assert(channels > 0 && channels <= 4);

                unsigned used = BITSET_MASK(channels) << offset;

                if (qword < MAX_UBO_QWORDS)
                        res->blocks[ubo].used[qword] |= used & 0xf;
                if (used > 0xf && qword + 1 < MAX_UBO_QWORDS)
                        res->blocks[ubo].used[qword + 1] |= used >> 4;
        }
}

static void
bi_set_sysval_push(struct bi_shader_info *info, unsigned sysval_ubo)
{
        if (info->push->num_ranges &&
            info->push->ranges[0].ubo == sysval_ubo) {

                unsigned pushed_sysvals = info->push->ranges[0].size / 4;

                info->sysvals->ubo_count -= pushed_sysvals;
                info->sysvals->push_count += pushed_sysvals;

                /* The sysval upload code can only handle a single range */
                assert(!(info->push->num_ranges > 1 &&
                         info->push->ranges[1].ubo == sysval_ubo));
        }
}

/* Select UBO words to push. A sophisticated implementation would consider the
 * number of uses and perhaps the control flow to estimate benefit. This is not
 * sophisticated. Select from the last UBO first to prioritize sysvals. */

static void
bi_pick_ubo(struct panfrost_ubo_push *push, struct bi_ubo_analysis *analysis,
            unsigned sysval_ubo, struct bi_shader_info *info)
{
        if (analysis->done_pick)
                return;

        /* The sysval push range must be first */
        assert(sysval_ubo == analysis->nr_blocks - 1);

        for (signed ubo = analysis->nr_blocks - 1; ubo >= 0; --ubo) {
                struct bi_ubo_block *block = &analysis->blocks[ubo];

                for (unsigned r = 0; r < MAX_UBO_QWORDS; ++r) {
                        unsigned used = block->used[r];

                        /* Don't push something we don't access */
                        if (used == 0) continue;

                        /* We want a single push range for sysvals, pretend
                         * there are no holes between sysvals */
                        if (ubo == sysval_ubo)
                                used = 0xf;

                        /* Don't push more than possible */
                        if (push->count > PAN_MAX_PUSH - util_bitcount(used))
                                continue;

                        u_foreach_bit(offs, used)
                                pan_add_pushed_ubo(push, ubo, r * 16 + offs * 4);

                        /* Mark it as pushed so we can rewrite */
                        BITSET_SET(block->pushed, r);
                }

                /* Stop if we aren't likely to be able to fit another entry */
                if (push->count > PAN_MAX_PUSH - 4)
                        break;
        }

        bi_set_sysval_push(info, sysval_ubo);

        analysis->done_pick = true;
}

void
bi_opt_push_ubo_analyze(bi_context *ctx)
{
        struct bi_ubo_analysis *analysis = *ctx->analysis;
        if (analysis)
                assert(!analysis->done_pick);

        bi_analyze_ranges(ctx);
}

void
bi_opt_push_ubo(bi_context *ctx)
{
        unsigned sysval_ubo =
                MAX2(ctx->inputs->sysval_ubo, ctx->nir->info.num_ubos);

        struct bi_ubo_analysis *analysis = *ctx->analysis;
        bi_pick_ubo(ctx->info.push, analysis, sysval_ubo, &ctx->info);

        ctx->ubo_mask = 0;

        bi_foreach_instr_global_safe(ctx, ins) {
                if (!bi_is_ubo(ins)) continue;

                unsigned ubo = ins->src[1].value;
                unsigned offset = ins->src[0].value;

                if (!bi_is_direct_aligned_ubo(ins)) {
                        /* The load can't be pushed, so this UBO needs to be
                         * uploaded conventionally */
                        if (ins->src[1].type == BI_INDEX_CONSTANT)
                                ctx->ubo_mask |= BITSET_BIT(ubo);
                        else
                                ctx->ubo_mask = ~0;

                        continue;
                }

                unsigned channels = bi_opcode_props[ins->op].sr_count;

                /* Check if we decided to push this */
                assert(ubo < analysis->nr_blocks);
                if ((!BITSET_TEST(analysis->blocks[ubo].pushed, offset / 16) ||
                     !BITSET_TEST(analysis->blocks[ubo].pushed,
                                  (offset + (channels - 1) * 4) / 16))) {
                        ctx->ubo_mask |= BITSET_BIT(ubo);
                        continue;
                }

                /* Replace the UBO load with moves from FAU */
                bi_builder b = bi_init_builder(ctx, bi_after_instr(ins));

                for (unsigned w = 0; w < channels; ++w) {
                        /* FAU is grouped in pairs (2 x 4-byte) */
                        unsigned base =
                                pan_lookup_pushed_ubo(ctx->info.push, ubo,
                                                      (offset + 4 * w));

                        unsigned fau_idx = (base >> 1);
                        unsigned fau_hi = (base & 1);

                        bi_mov_i32_to(&b,
                                bi_word(ins->dest[0], w),
                                bi_fau(BIR_FAU_UNIFORM | fau_idx, fau_hi));
                }

                bi_remove_instruction(ins);
        }

        if (--analysis->refcnt == 0) {
                free(analysis->blocks);
                free(analysis);
        }
}
