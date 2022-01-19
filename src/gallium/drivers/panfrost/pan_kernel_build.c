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
 *
 */

#include "pan_screen.h"
#include "pan_context.h"

struct panfrost_kernel *
panfrost_get_kernel(struct panfrost_context *ctx,
                    const struct pan_kernel_template *kernel)
{
        struct pipe_context *pipe = &ctx->base;
        struct pipe_screen *pscreen = ctx->base.screen;
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = pan_device(pscreen);

        pthread_mutex_lock(&screen->compute_kernel_lock);

        struct hash_entry *he =
                _mesa_hash_table_search(screen->compute_kernels,
                                        kernel);

        if (he) {
                pthread_mutex_unlock(&screen->compute_kernel_lock);
                return (struct panfrost_kernel *)he->data;
        }

        static const char *dir = (void *)(uintptr_t)1;
        if ((uintptr_t)dir == 1)
                dir = os_get_option("PAN_KERNEL_DIR");

        const nir_shader_compiler_options *nir_options =
                pscreen->get_compiler_options(pscreen, PIPE_SHADER_IR_NIR,
                                              PIPE_SHADER_COMPUTE);

        struct panfrost_kernel *out = rzalloc(screen->compute_kernels,
                                              struct panfrost_kernel);

        bool enable_printf = (dev->debug & PAN_DBG_PRINTF);

        if (dir) {
                char *path;
                ASSERTED int ret = asprintf(&path, "%s/%s.spv", dir, kernel->name);
                assert(ret != -1);

                pan_kernel_from_spirv_file(&out->base, path,
                                           kernel->entrypoint,
                                           nir_options, enable_printf);

                free(path);
        } else {
                pan_kernel_from_spirv(&out->base, kernel,
                                      nir_options, enable_printf);
        }

        struct pipe_compute_state cso = {
                .ir_type = PIPE_SHADER_IR_NIR,
                .prog = out->base.nir,
        };
        out->cso = pipe->create_compute_state(pipe, &cso);

        _mesa_hash_table_insert(screen->compute_kernels,
                                kernel, out);

        pthread_mutex_unlock(&screen->compute_kernel_lock);

        return out;
}
