/*
 * Copyright (C) 2021 Icecream95
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

#ifndef __PAN_EMU_H
#define __PAN_EMU_H

#include <stdio.h>
#include "util/u_dynarray.h"

static inline void
hexdump(FILE *fp, const uint8_t *hex, size_t cnt)
{
        uint8_t *shadow = malloc(cnt);
        memcpy(shadow, hex, cnt);
        hex = shadow;

        for (unsigned i = 0; i < cnt; ++i) {
                if ((i & 0xF) == 0)
                        fprintf(fp, "%06X  ", i);

                uint8_t v = hex[i];

                if (v == 0 && (i & 0xF) == 0) {
                        /* Check if we're starting an aligned run of zeroes */
                        unsigned zero_count = 0;

                        for (unsigned j = i; j < cnt; ++j) {
                                if (hex[j] == 0)
                                        zero_count++;
                                else
                                        break;
                        }

                        if (zero_count >= 32) {
                                fprintf(fp, "*\n");
                                i += (zero_count & ~0xF) - 1;
                                continue;
                        }
                }

                fprintf(fp, "%02X ", hex[i]);

                if ((i & 0xF) == 0xF)
                        fprintf(fp, "\n");
        }

        fprintf(fp, "\n");

        free(shadow);
}

void panfrost_emulate_tiler_v6(struct util_dynarray *tiler_jobs, unsigned gpu_id);

static inline void
panfrost_emulate_tiler(struct util_dynarray *tiler_jobs, unsigned gpu_id)
{
        switch (pan_arch(gpu_id)) {
        case 6: panfrost_emulate_tiler_v6(tiler_jobs, gpu_id); return;
        default: unreachable("Unsupported architecture");
        }
}

#endif
