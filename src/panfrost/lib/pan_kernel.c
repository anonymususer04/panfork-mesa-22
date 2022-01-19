/*
 * Copyright (C) 2022 Icecream95
 * Copyright © 2020 Intel Corporation
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

#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"

#include "pan_kernel.h"

bool
pan_kernel_from_spirv(struct pan_kernel *kernel,
                      const struct pan_kernel_template *tmpl,
                      const nir_shader_compiler_options *nir_options,
                      bool enable_printf)
{
        struct spirv_to_nir_options spirv_options = {
                .environment = NIR_SPIRV_OPENCL,
                .caps = {
                        .address = true,
                        .float16 = true,
                        .int8 = true, /* Though int8 is buggy on Midgard */
                        .int16 = true,
                        .int64 = true,
                        .float64 = true,
                        .kernel = true,
                        .generic_pointers = true,
                        .storage_8bit = true,
                        .storage_16bit = true,
                        .printf = enable_printf,
                },
                .shared_addr_format = nir_address_format_32bit_offset_as_64bit,
                .temp_addr_format = nir_address_format_32bit_offset_as_64bit,
                .global_addr_format = nir_address_format_64bit_global,
                .constant_addr_format = nir_address_format_64bit_global,
        };

        assert(tmpl->spirv_size % 4 == 0);
        nir_shader *nir =
                spirv_to_nir(tmpl->spirv, tmpl->spirv_size / 4, NULL, 0, MESA_SHADER_KERNEL,
                             tmpl->entrypoint, &spirv_options, nir_options);
        nir_validate_shader(nir, "after spirv_to_nir");
        nir_validate_ssa_dominance(nir, "after spirv_to_nir");
        nir->info.name = ralloc_strdup(nir, tmpl->entrypoint);

        /* We have to lower away local constant initializers right before we
         * inline functions.  That way they get properly initialized at the top
         * of the function and not at the top of its caller.
         */
        NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_function_temp);
        NIR_PASS_V(nir, nir_lower_returns);

        struct nir_lower_printf_options printf_options = {
                .treat_doubles_as_floats = true,
                .max_buffer_size = 1024 * 1024,
        };
        NIR_PASS_V(nir, nir_lower_printf, &printf_options);

        /* TODO: Try turning off this pass :) */
        NIR_PASS_V(nir, nir_inline_functions);

        NIR_PASS_V(nir, nir_copy_prop);
        NIR_PASS_V(nir, nir_opt_deref);

        NIR_PASS_V(nir, nir_lower_system_values);
        NIR_PASS_V(nir, nir_lower_compute_system_values, NULL);

        /* Pick off the single entrypoint that we want */
        foreach_list_typed_safe(nir_function, func, node, &nir->functions) {
                if (!func->is_entrypoint)
                        exec_node_remove(&func->node);
        }
        assert(exec_list_length(&nir->functions) == 1);

        /* Now that we've deleted all but the main function, we can go ahead and
         * lower the rest of the constant initializers.  We do this here so that
         * nir_remove_dead_variables and split_per_member_structs below see the
         * corresponding stores.
         */
        NIR_PASS_V(nir, nir_lower_variable_initializers, ~0);

        /* LLVM loves take advantage of the fact that vec3s in OpenCL are 16B
         * aligned and so it can just read/write them as vec4s.  This results in a
         * LOT of vec4->vec3 casts on loads and stores.  One solution to this
         * problem is to get rid of all vec3 variables.
         */
        NIR_PASS_V(nir, nir_lower_vec3_to_vec4,
                   nir_var_shader_temp | nir_var_function_temp |
                   nir_var_mem_shared | nir_var_mem_global|
                   nir_var_mem_constant);

        /* We assign explicit types early so that the optimizer can take advantage
         * of that information and hopefully get rid of some of our memcpys.
         */
        NIR_PASS_V(nir, nir_lower_vars_to_explicit_types,
                   nir_var_uniform |
                   nir_var_shader_temp | nir_var_function_temp |
                   nir_var_mem_shared | nir_var_mem_global,
                   glsl_get_cl_type_size_align);

        int max_arg_idx = -1;
        nir_foreach_uniform_variable(var, nir) {
                assert(var->data.location < 256);
                max_arg_idx = MAX2(max_arg_idx, var->data.location);
        }

        kernel->args_size = nir->num_uniforms;
        kernel->arg_count = max_arg_idx + 1;

        struct pan_kernel_arg_desc *args =
                calloc(kernel->arg_count, sizeof(struct pan_kernel_arg_desc));

        nir_foreach_uniform_variable(var, nir) {
                struct pan_kernel_arg_desc arg_desc = {
                        .offset = var->data.driver_location,
                        .size = glsl_get_explicit_size(var->type, false),
                };
                assert(arg_desc.offset + arg_desc.size <= nir->num_uniforms);

                assert(var->data.location >= 0);
                args[var->data.location] = arg_desc;
        }

        kernel->args = args;

        NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_all, NULL);


        /* Lower again, this time after dead-variables to get more compact variable
         * layouts.
         */
        nir->scratch_size = 0;
        NIR_PASS_V(nir, nir_lower_vars_to_explicit_types,
                   nir_var_shader_temp | nir_var_function_temp |
                   nir_var_mem_shared | nir_var_mem_global | nir_var_mem_constant,
                   glsl_get_cl_type_size_align);
        if (nir->constant_data_size > 0) {
                assert(nir->constant_data == NULL);
                nir->constant_data = rzalloc_size(nir, nir->constant_data_size);
                nir_gather_explicit_io_initializers(nir, nir->constant_data,
                                                    nir->constant_data_size,
                                                    nir_var_mem_constant);
        }


        NIR_PASS_V(nir, nir_lower_memcpy);

        /* Some of these lower_io passes might be wrong.. */
        NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_constant,
                   nir_address_format_64bit_global);

        NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_uniform,
                   nir_address_format_32bit_offset_as_64bit);

        NIR_PASS_V(nir, nir_lower_vars_to_ssa);

        NIR_PASS_V(nir, nir_split_var_copies);
        NIR_PASS_V(nir, nir_split_per_member_structs);
        NIR_PASS_V(nir, nir_split_struct_vars, nir_var_function_temp);

        NIR_PASS_V(nir, nir_lower_explicit_io,
                   nir_var_shader_temp | nir_var_function_temp |
                   nir_var_mem_shared | nir_var_mem_global,
                   nir_address_format_64bit_global);

        NIR_PASS_V(nir, nir_lower_convert_alu_types, NULL);

        /* So that we have a chance to convert idiv to imod before
         * the backend compiler calls nir_lower_idiv */
        NIR_PASS_V(nir, nir_opt_algebraic);

        kernel->nir = nir;

        kernel->printf_info_count = nir->printf_info_count;
        kernel->printf_info = rzalloc_array(kernel, nir_printf_info, nir->printf_info_count);
        memcpy(kernel->printf_info, nir->printf_info, sizeof(nir_printf_info) * nir->printf_info_count);
        for (unsigned i = 0; i < kernel->printf_info_count; ++i) {
                nir_printf_info *info = &kernel->printf_info[i];
                nir_printf_info *nir_info = &nir->printf_info[i];

                if (nir_info->num_args) {
                        info->arg_sizes = ralloc_array(kernel, unsigned, nir_info->num_args);
                        memcpy(info->arg_sizes, nir_info->arg_sizes, sizeof(unsigned) * nir_info->num_args);
                }
                if (nir_info->string_size) {
                        info->strings = ralloc_array(kernel, char, nir_info->string_size);
                        memcpy(info->strings, nir_info->strings, nir_info->string_size);
                }
        }

        return true;
}

bool
pan_kernel_from_spirv_file(struct pan_kernel *kernel,
                           const char *path,
                           const char *entrypoint,
                           const nir_shader_compiler_options *nir_options,
                           bool enable_printf)
{
        FILE *f = fopen(path, "r");

        fseek(f, 0, SEEK_END);
        unsigned spirv_size = ftell(f);
        assert(spirv_size % 4 == 0);
        fseek(f, 0, SEEK_SET);

        uint32_t *spirv = malloc(spirv_size);
        fread(spirv, spirv_size, 1, f);
        fclose(f);

        struct pan_kernel_template tmpl = {
                .name = "",
                .entrypoint = entrypoint,
                .spirv = spirv,
                .spirv_size = spirv_size,
        };

        bool ret = pan_kernel_from_spirv(kernel, &tmpl,
                                         nir_options, enable_printf);

        free(spirv);
        return ret;
}
