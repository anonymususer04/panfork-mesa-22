/*
 * Copyright © 2015 Intel Corporation
 * Copyright © 2022 Collabora, LTD
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_nir.h"

#include "compiler/spirv/nir_spirv.h"
#include "vk_log.h"
#include "vk_util.h"

#define SPIR_V_MAGIC_NUMBER 0x07230203

uint32_t
vk_spirv_version(uint32_t *spirv_data, size_t spirv_size_B)
{
   assert(spirv_size_B >= 8);
   assert(spirv_data[0] == SPIR_V_MAGIC_NUMBER);
   return spirv_data[1];
}

static void
spirv_nir_debug(void *private_data,
                enum nir_spirv_debug_level level,
                size_t spirv_offset,
                const char *message)
{
   const struct vk_object_base *log_obj = private_data;

   switch (level) {
   case NIR_SPIRV_DEBUG_LEVEL_INFO:
      //vk_logi(VK_LOG_OBJS(log_obj), "SPIR-V offset %lu: %s",
      //        (unsigned long) spirv_offset, message);
      break;
   case NIR_SPIRV_DEBUG_LEVEL_WARNING:
      vk_logw(VK_LOG_OBJS(log_obj), "SPIR-V offset %lu: %s",
              (unsigned long) spirv_offset, message);
      break;
   case NIR_SPIRV_DEBUG_LEVEL_ERROR:
      vk_loge(VK_LOG_OBJS(log_obj), "SPIR-V offset %lu: %s",
              (unsigned long) spirv_offset, message);
      break;
   default:
      break;
   }
}

nir_shader *
vk_spirv_to_nir(struct vk_device *device,
                const uint32_t *spirv_data, size_t spirv_size_B,
                gl_shader_stage stage, const char *entrypoint_name,
                const VkSpecializationInfo *spec_info,
                const struct spirv_to_nir_options *spirv_options,
                const struct nir_shader_compiler_options *nir_options,
                void *mem_ctx)
{
   assert(spirv_size_B >= 4 && spirv_size_B % 4 == 0);
   assert(spirv_data[0] == SPIR_V_MAGIC_NUMBER);

   struct spirv_to_nir_options spirv_options_local = *spirv_options;
   spirv_options_local.debug.func = spirv_nir_debug;
   spirv_options_local.debug.private_data = (void *)device;

   uint32_t num_spec_entries = 0;
   struct nir_spirv_specialization *spec_entries =
      vk_spec_info_to_nir_spirv(spec_info, &num_spec_entries);

   nir_shader *nir = spirv_to_nir(spirv_data, spirv_size_B / 4,
                                  spec_entries, num_spec_entries,
                                  stage, entrypoint_name,
                                  &spirv_options_local, nir_options);
   free(spec_entries);

   if (nir == NULL)
      return NULL;

   assert(nir->info.stage == stage);
   nir_validate_shader(nir, "after spirv_to_nir");
   nir_validate_ssa_dominance(nir, "after spirv_to_nir");
   if (mem_ctx != NULL)
      ralloc_steal(mem_ctx, nir);

   /* We have to lower away local constant initializers right before we
    * inline functions.  That way they get properly initialized at the top
    * of the function and not at the top of its caller.
    */
   NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS_V(nir, nir_lower_returns);
   NIR_PASS_V(nir, nir_inline_functions);
   NIR_PASS_V(nir, nir_copy_prop);
   NIR_PASS_V(nir, nir_opt_deref);

   /* Pick off the single entrypoint that we want */
   nir_remove_non_entrypoints(nir);

   /* Now that we've deleted all but the main function, we can go ahead and
    * lower the rest of the constant initializers.  We do this here so that
    * nir_remove_dead_variables and split_per_member_structs below see the
    * corresponding stores.
    */
   NIR_PASS_V(nir, nir_lower_variable_initializers, ~0);

   /* Split member structs.  We do this before lower_io_to_temporaries so that
    * it doesn't lower system values to temporaries by accident.
    */
   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_split_per_member_structs);

   NIR_PASS_V(nir, nir_remove_dead_variables,
              nir_var_shader_in | nir_var_shader_out | nir_var_system_value |
              nir_var_shader_call_data | nir_var_ray_hit_attrib,
              NULL);

   NIR_PASS_V(nir, nir_propagate_invariant, false);

   return nir;
}
