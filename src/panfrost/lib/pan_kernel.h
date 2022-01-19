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

#ifndef PAN_KERNEL_H
#define PAN_KERNEL_H

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pan_kernel_template {
        const char *name;
        const char *entrypoint;
        const uint32_t *spirv;
        unsigned spirv_size;
};

struct pan_kernel_arg_desc {
        uint16_t offset;
        uint16_t size;
};

struct pan_kernel {
        uint16_t args_size;
        uint16_t arg_count;
        const struct pan_kernel_arg_desc *args;

        unsigned printf_info_count;
        nir_printf_info *printf_info;

        nir_shader *nir;
};

bool
pan_kernel_from_spirv(struct pan_kernel *kernel,
                      const struct pan_kernel_template *tmpl,
                      const nir_shader_compiler_options *nir_options,
                      bool enable_printf);

bool
pan_kernel_from_spirv_file(struct pan_kernel *kernel,
                           const char *path,
                           const char *entrypoint,
                           const nir_shader_compiler_options *nir_options,
                           bool enable_printf);

#ifdef __cplusplus
}
#endif

#endif
