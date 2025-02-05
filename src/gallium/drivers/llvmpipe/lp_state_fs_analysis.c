/**************************************************************************
 *
 * Copyright 2010-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/


#include "util/u_memory.h"
#include "util/u_math.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_util.h"
#include "tgsi/tgsi_dump.h"
#include "lp_debug.h"
#include "lp_state.h"
#include "nir.h"

/*
 * Detect Aero minification shaders.
 *
 * Aero does not use texture mimaps when a window gets animated and its shaped
 * bended. Instead it uses the average of 4 nearby texels. This is the simplest
 * of such shader, but there are several variations:
 *
 *   FRAG
 *   DCL IN[0], GENERIC[1], PERSPECTIVE
 *   DCL IN[1], GENERIC[2], PERSPECTIVE
 *   DCL IN[2], GENERIC[3], PERSPECTIVE
 *   DCL OUT[0], COLOR
 *   DCL SAMP[0]
 *   DCL TEMP[0..3]
 *   IMM FLT32 {     0.2500,     0.0000,     0.0000,     0.0000 }
 *   MOV TEMP[0].x, IN[0].zzzz
 *   MOV TEMP[0].y, IN[0].wwww
 *   MOV TEMP[1].x, IN[1].zzzz
 *   MOV TEMP[1].y, IN[1].wwww
 *   TEX TEMP[0], TEMP[0], SAMP[0], 2D
 *   TEX TEMP[2], IN[0], SAMP[0], 2D
 *   TEX TEMP[3], IN[1], SAMP[0], 2D
 *   TEX TEMP[1], TEMP[1], SAMP[0], 2D
 *   ADD TEMP[0], TEMP[0], TEMP[2]
 *   ADD TEMP[0], TEMP[3], TEMP[0]
 *   ADD TEMP[0], TEMP[1], TEMP[0]
 *   MUL TEMP[0], TEMP[0], IN[2]
 *   MUL TEMP[0], TEMP[0], IMM[0].xxxx
 *   MOV OUT[0], TEMP[0]
 *   END
 *
 * Texture coordinates are interleaved like the Gaussian blur shaders, but
 * unlike the later there isn't structure in the sub-pixel positioning of the
 * texels, other than being disposed in a diamond-like shape. For example,
 * these are the relative offsets of the texels relative to the average:
 *
 *    x offset   y offset
 *   --------------------
 *    0.691834   -0.21360
 *   -0.230230   -0.64160
 *   -0.692406    0.21356
 *    0.230802    0.64160
 *
 *  These shaders are typically used with linear min/mag filtering, but the
 *  linear filtering provides very little visual improvement compared to the
 *  performance impact it has. The ultimate purpose of detecting these shaders
 *  is to override with nearest texture filtering.
 */
static inline boolean
match_aero_minification_shader(const struct tgsi_token *tokens,
                               const struct lp_tgsi_info *info)
{
   struct tgsi_parse_context parse;
   unsigned coord_mask;
   boolean has_quarter_imm;
   unsigned index, chan;

   if ((info->base.opcode_count[TGSI_OPCODE_TEX] != 4 &&
        info->base.opcode_count[TGSI_OPCODE_SAMPLE] != 4) ||
       info->num_texs != 4) {
      return FALSE;
   }

   /*
    * Ensure the texture coordinates are interleaved as in the example above.
    */

   coord_mask = 0;
   for (index = 0; index < 4; ++index) {
      const struct lp_tgsi_texture_info *tex = &info->tex[index];
      if (tex->sampler_unit != 0 ||
          tex->texture_unit != 0 ||
          tex->coord[0].file != TGSI_FILE_INPUT ||
          tex->coord[1].file != TGSI_FILE_INPUT ||
          tex->coord[0].u.index != tex->coord[1].u.index ||
          (tex->coord[0].swizzle % 2) != 0 ||
          tex->coord[1].swizzle != tex->coord[0].swizzle + 1) {
         return FALSE;
      }

      coord_mask |= 1 << (tex->coord[0].u.index*2 + tex->coord[0].swizzle/2);
   }
   if (coord_mask != 0xf) {
      return FALSE;
   }

   /*
    * Ensure it has the 0.25 immediate.
    */

   has_quarter_imm = FALSE;

   tgsi_parse_init(&parse, tokens);

   while (!tgsi_parse_end_of_tokens(&parse)) {
      tgsi_parse_token(&parse);

      switch (parse.FullToken.Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         break;

      case TGSI_TOKEN_TYPE_INSTRUCTION:
         goto finished;

      case TGSI_TOKEN_TYPE_IMMEDIATE:
         {
            const unsigned size =
                  parse.FullToken.FullImmediate.Immediate.NrTokens - 1;
            assert(size <= 4);
            for (chan = 0; chan < size; ++chan) {
               if (parse.FullToken.FullImmediate.u[chan].Float == 0.25f) {
                  has_quarter_imm = TRUE;
                  goto finished;
               }
            }
         }
         break;

      case TGSI_TOKEN_TYPE_PROPERTY:
         break;

      default:
         assert(0);
         goto finished;
      }
   }
finished:

   tgsi_parse_free(&parse);

   if (!has_quarter_imm) {
      return FALSE;
   }

   return TRUE;
}


/*
 * Examine the NIR shader to determine if it's "linear".
 */
static bool
llvmpipe_nir_fn_is_linear_compat(const struct nir_shader *shader,
                                 nir_function_impl *impl,
                                 struct lp_tgsi_info *info)
{
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_deref:
         case nir_instr_type_load_const:
            break;
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            if (intrin->intrinsic != nir_intrinsic_load_deref &&
                intrin->intrinsic != nir_intrinsic_store_deref &&
                intrin->intrinsic != nir_intrinsic_load_ubo)
               return false;

            if (intrin->intrinsic == nir_intrinsic_load_ubo) {
               if (!nir_src_is_const(intrin->src[0]))
                  return false;
               nir_load_const_instr *load =
                  nir_instr_as_load_const(intrin->src[0].ssa->parent_instr);
               if (load->value[0].u32 != 0)
                  return false;
            }
            break;
         }
         case nir_instr_type_tex: {
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            struct lp_tgsi_texture_info *tex_info = &info->tex[info->num_texs];

            for (unsigned i = 0; i < tex->num_srcs; i++) {
               switch (tex->src[i].src_type) {
               case nir_tex_src_coord: {
                  nir_ssa_scalar scalar = nir_ssa_scalar_resolved(tex->src[i].src.ssa, 0);
                  if (scalar.def->parent_instr->type != nir_instr_type_intrinsic)
                     return false;
                  nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(scalar.def->parent_instr);
                  if (intrin->intrinsic != nir_intrinsic_load_deref)
                     return false;
                  nir_deref_instr *deref = nir_instr_as_deref(intrin->src[0].ssa->parent_instr);
                  nir_variable *var = nir_deref_instr_get_variable(deref);
                  if (var->data.mode != nir_var_shader_in)
                     return false;
                  break;
               }
               default:
                  continue;
               }
            }

            switch (tex->op) {
            case nir_texop_tex:
               tex_info->modifier = LP_BLD_TEX_MODIFIER_NONE;
               break;
            default:
               /* inaccurate but sufficient. */
               tex_info->modifier = LP_BLD_TEX_MODIFIER_EXPLICIT_LOD;
               return false;
            }
            switch (tex->sampler_dim) {
            case GLSL_SAMPLER_DIM_2D:
               tex_info->target = TGSI_TEXTURE_2D;
               break;
            default:
               /* inaccurate but sufficient. */
               tex_info->target = TGSI_TEXTURE_1D;
               return false;
            }

            tex_info->sampler_unit = tex->sampler_index;

            /* this is enforced in the scanner previously. */
            tex_info->coord[0].file = TGSI_FILE_INPUT;
            tex_info->coord[1].file = TGSI_FILE_INPUT;
            tex_info->coord[1].swizzle = 1;
            info->num_texs++;
            break;
         }
         case nir_instr_type_alu: {
            const nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op != nir_op_mov &&
                alu->op != nir_op_vec2 &&
                alu->op != nir_op_vec4 &&
                alu->op != nir_op_fmul)
               return false;

            if (alu->op == nir_op_fmul) {
               unsigned num_src = nir_op_infos[alu->op].num_inputs;;
               for (unsigned s = 0; s < num_src; s++) {
                  /* If the MUL uses immediate values, the values must
                   * be 32-bit floats in the range [0,1].
                   */
                  if (nir_src_is_const(alu->src[s].src)) {
                     nir_load_const_instr *load =
                        nir_instr_as_load_const(alu->src[s].src.ssa->parent_instr);

                     if (load->def.bit_size != 32)
                        return false;
                     for (unsigned c = 0; c < load->def.num_components; c++) {
                        if (load->value[c].f32 < 0.0 || load->value[c].f32 > 1.0) {
                           info->unclamped_immediates = true;
                           return false;
                        }
                     }
                  }
               }
            }
            break;
         }
         default:
            return false;
         }
      }
   }
   return true;
}


static bool
llvmpipe_nir_is_linear_compat(struct nir_shader *shader,
                              struct lp_tgsi_info *info)
{
   nir_foreach_function(function, shader) {
      if (function->impl) {
         if (!llvmpipe_nir_fn_is_linear_compat(shader, function->impl, info))
            return false;
      }
   }
   return true;
}


void
llvmpipe_fs_analyse_nir(struct lp_fragment_shader *shader)
{
   shader->kind = LP_FS_KIND_GENERAL;

   if (shader->info.base.num_inputs <= LP_MAX_LINEAR_INPUTS &&
       shader->info.base.num_outputs == 1 &&
       !shader->info.indirect_textures &&
       !shader->info.sampler_texture_units_different &&
       !shader->info.unclamped_immediates &&
       shader->info.num_texs <= LP_MAX_LINEAR_TEXTURES &&
       llvmpipe_nir_is_linear_compat(shader->base.ir.nir, &shader->info)) {
      shader->kind = LP_FS_KIND_LLVM_LINEAR;
   }
}


void
llvmpipe_fs_analyse(struct lp_fragment_shader *shader,
                    const struct tgsi_token *tokens)
{
   shader->kind = LP_FS_KIND_GENERAL;

   if (shader->kind == LP_FS_KIND_GENERAL &&
       shader->info.base.num_inputs <= LP_MAX_LINEAR_INPUTS &&
       shader->info.base.num_outputs == 1 &&
       !shader->info.indirect_textures &&
       !shader->info.sampler_texture_units_different &&
       !shader->info.unclamped_immediates &&
       shader->info.num_texs <= LP_MAX_LINEAR_TEXTURES &&
       (shader->info.base.opcode_count[TGSI_OPCODE_TEX] +
        shader->info.base.opcode_count[TGSI_OPCODE_SAMPLE] +
        shader->info.base.opcode_count[TGSI_OPCODE_MOV] +
        shader->info.base.opcode_count[TGSI_OPCODE_MUL] +
        shader->info.base.opcode_count[TGSI_OPCODE_RET] +
        shader->info.base.opcode_count[TGSI_OPCODE_END] ==
        shader->info.base.num_instructions)) {
      shader->kind = LP_FS_KIND_LLVM_LINEAR;
   }

   if (shader->kind == LP_FS_KIND_GENERAL &&
       match_aero_minification_shader(tokens, &shader->info)) {
      shader->kind = LP_FS_KIND_AERO_MINIFICATION;
   }
}
