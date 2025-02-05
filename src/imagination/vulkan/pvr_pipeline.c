/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * based in part on v3dv driver which is:
 * Copyright © 2019 Raspberry Pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "compiler/shader_enums.h"
#include "hwdef/rogue_hw_utils.h"
#include "nir/nir.h"
#include "pvr_bo.h"
#include "pvr_csb.h"
#include "pvr_csb_enum_helpers.h"
#include "pvr_hardcode.h"
#include "pvr_pds.h"
#include "pvr_private.h"
#include "pvr_shader.h"
#include "pvr_winsys.h"
#include "rogue/rogue.h"
#include "rogue/rogue_build_data.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_util.h"

/*****************************************************************************
   PDS functions
*****************************************************************************/

/* If allocator == NULL, the internal one will be used. */
static VkResult pvr_pds_coeff_program_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *allocator,
   const uint32_t *fpu_iterators,
   uint32_t fpu_iterators_count,
   const uint32_t *destinations,
   struct pvr_pds_upload *const pds_upload_out)
{
   struct pvr_pds_coeff_loading_program program = {
      .num_fpu_iterators = fpu_iterators_count,
   };
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   assert(fpu_iterators_count < PVR_MAXIMUM_ITERATIONS);

   /* Get the size of the program and then allocate that much memory. */
   pvr_pds_coefficient_loading(&program, NULL, PDS_GENERATE_SIZES);

   staging_buffer_size =
      (program.code_size + program.data_size) * sizeof(*staging_buffer);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* FIXME: Should we save pointers when we redesign the pds gen api ? */
   typed_memcpy(program.FPU_iterators,
                fpu_iterators,
                program.num_fpu_iterators);

   typed_memcpy(program.destination, destinations, program.num_fpu_iterators);

   /* Generate the program into is the staging_buffer. */
   pvr_pds_coefficient_loading(&program,
                               staging_buffer,
                               PDS_GENERATE_CODEDATA_SEGMENTS);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[0],
                               program.data_size,
                               16,
                               &staging_buffer[program.data_size],
                               program.code_size,
                               16,
                               16,
                               pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return result;
   }

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
}

/* FIXME: move this elsewhere since it's also called in pvr_pass.c? */
/* If allocator == NULL, the internal one will be used. */
VkResult pvr_pds_fragment_program_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *allocator,
   const struct pvr_bo *fragment_shader_bo,
   uint32_t fragment_temp_count,
   enum rogue_msaa_mode msaa_mode,
   bool has_phase_rate_change,
   struct pvr_pds_upload *const pds_upload_out)
{
   const enum PVRX(PDSINST_DOUTU_SAMPLE_RATE)
      sample_rate = pvr_pdsinst_doutu_sample_rate_from_rogue(msaa_mode);
   struct pvr_pds_kickusc_program program = { 0 };
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   /* FIXME: Should it be passing in the USC offset rather than address here?
    */
   /* Note this is not strictly required to be done before calculating the
    * staging_buffer_size in this particular case. It can also be done after
    * allocating the buffer. The size from pvr_pds_kick_usc() is constant.
    */
   pvr_pds_setup_doutu(&program.usc_task_control,
                       fragment_shader_bo->vma->dev_addr.addr,
                       fragment_temp_count,
                       sample_rate,
                       has_phase_rate_change);

   pvr_pds_kick_usc(&program, NULL, 0, false, PDS_GENERATE_SIZES);

   staging_buffer_size =
      (program.code_size + program.data_size) * sizeof(*staging_buffer);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_pds_kick_usc(&program,
                    staging_buffer,
                    0,
                    false,
                    PDS_GENERATE_CODEDATA_SEGMENTS);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[0],
                               program.data_size,
                               16,
                               &staging_buffer[program.data_size],
                               program.code_size,
                               16,
                               16,
                               pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return result;
   }

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
}

static inline size_t pvr_pds_get_max_vertex_program_const_map_size_in_bytes(
   const struct pvr_device_info *dev_info,
   bool robust_buffer_access)
{
   /* FIXME: Use more local variable to improve formatting. */

   /* Maximum memory allocation needed for const map entries in
    * pvr_pds_generate_vertex_primary_program().
    * When robustBufferAccess is disabled, it must be >= 410.
    * When robustBufferAccess is enabled, it must be >= 570.
    *
    * 1. Size of entry for base instance
    *        (pvr_const_map_entry_base_instance)
    *
    * 2. Max. number of vertex inputs (PVR_MAX_VERTEX_INPUT_BINDINGS) * (
    *     if (!robustBufferAccess)
    *         size of vertex attribute entry
    *             (pvr_const_map_entry_vertex_attribute_address) +
    *     else
    *         size of robust vertex attribute entry
    *             (pvr_const_map_entry_robust_vertex_attribute_address) +
    *         size of entry for max attribute index
    *             (pvr_const_map_entry_vertex_attribute_max_index) +
    *     fi
    *     size of Unified Store burst entry
    *         (pvr_const_map_entry_literal32) +
    *     size of entry for vertex stride
    *         (pvr_const_map_entry_literal32) +
    *     size of entries for DDMAD control word
    *         (num_ddmad_literals * pvr_const_map_entry_literal32))
    *
    * 3. Size of entry for DOUTW vertex/instance control word
    *     (pvr_const_map_entry_literal32)
    *
    * 4. Size of DOUTU entry (pvr_const_map_entry_doutu_address)
    */

   const size_t attribute_size =
      (!robust_buffer_access)
         ? sizeof(struct pvr_const_map_entry_vertex_attribute_address)
         : sizeof(struct pvr_const_map_entry_robust_vertex_attribute_address) +
              sizeof(struct pvr_const_map_entry_vertex_attribute_max_index);

   /* If has_pds_ddmadt the DDMAD control word is now a DDMADT control word
    * and is increased by one DWORD to contain the data for the DDMADT's
    * out-of-bounds check.
    */
   const size_t pvr_pds_const_map_vertex_entry_num_ddmad_literals =
      1U + (size_t)PVR_HAS_FEATURE(dev_info, pds_ddmadt);

   return (sizeof(struct pvr_const_map_entry_base_instance) +
           PVR_MAX_VERTEX_INPUT_BINDINGS *
              (attribute_size +
               (2 + pvr_pds_const_map_vertex_entry_num_ddmad_literals) *
                  sizeof(struct pvr_const_map_entry_literal32)) +
           sizeof(struct pvr_const_map_entry_literal32) +
           sizeof(struct pvr_const_map_entry_doutu_address));
}

/* This is a const pointer to an array of pvr_pds_vertex_dma structs.
 * The array being pointed to is of PVR_MAX_VERTEX_ATTRIB_DMAS size.
 */
typedef struct pvr_pds_vertex_dma (
      *const
         pvr_pds_attrib_dma_descriptions_array_ptr)[PVR_MAX_VERTEX_ATTRIB_DMAS];

/* dma_descriptions_out_ptr is a pointer to the array used as output.
 * The whole array might not be filled so dma_count_out indicates how many
 * elements were used.
 */
static void pvr_pds_vertex_attrib_init_dma_descriptions(
   const VkPipelineVertexInputStateCreateInfo *const vertex_input_state,
   const struct rogue_vs_build_data *vs_data,
   pvr_pds_attrib_dma_descriptions_array_ptr dma_descriptions_out_ptr,
   uint32_t *const dma_count_out)
{
   struct pvr_pds_vertex_dma *const dma_descriptions =
      *dma_descriptions_out_ptr;
   uint32_t dma_count = 0;

   if (!vertex_input_state) {
      *dma_count_out = 0;
      return;
   }

   for (uint32_t i = 0; i < vertex_input_state->vertexAttributeDescriptionCount;
        i++) {
      const VkVertexInputAttributeDescription *const attrib_desc =
         &vertex_input_state->pVertexAttributeDescriptions[i];
      const VkVertexInputBindingDescription *binding_desc = NULL;

      /* Finding the matching binding description. */
      for (uint32_t j = 0;
           j < vertex_input_state->vertexBindingDescriptionCount;
           j++) {
         const VkVertexInputBindingDescription *const current_binding_desc =
            &vertex_input_state->pVertexBindingDescriptions[j];

         if (current_binding_desc->binding == attrib_desc->binding) {
            binding_desc = current_binding_desc;
            break;
         }
      }

      /* From the Vulkan 1.2.195 spec for
       * VkPipelineVertexInputStateCreateInfo:
       *
       *    "For every binding specified by each element of
       *    pVertexAttributeDescriptions, a
       *    VkVertexInputBindingDescription must exist in
       *    pVertexBindingDescriptions with the same value of binding"
       *
       * So we don't check if we found the matching binding description
       * or not.
       */

      struct pvr_pds_vertex_dma *const dma_desc = &dma_descriptions[dma_count];

      size_t location = attrib_desc->location;
      assert(location < vs_data->inputs.num_input_vars);

      dma_desc->offset = attrib_desc->offset;
      dma_desc->stride = binding_desc->stride;

      dma_desc->flags = 0;

      if (binding_desc->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
         dma_desc->flags |= PVR_PDS_VERTEX_DMA_FLAGS_INSTANCE_RATE;

      dma_desc->size_in_dwords = vs_data->inputs.components[location];
      /* TODO: This will be different when other types are supported.
       * Store in vs_data with base and components?
       */
      /* TODO: Use attrib_desc->format. */
      dma_desc->component_size_in_bytes = ROGUE_REG_SIZE_BYTES;
      dma_desc->destination = vs_data->inputs.base[location];
      dma_desc->binding_index = attrib_desc->binding;
      dma_desc->divisor = 1;
      dma_desc->robustness_buffer_offset = 0;

      ++dma_count;
   }

   *dma_count_out = dma_count;
}

static VkResult pvr_pds_vertex_attrib_program_create_and_upload(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   struct pvr_pds_vertex_primary_program_input *const input,
   struct pvr_pds_attrib_program *const program_out)
{
   const size_t const_entries_size_in_bytes =
      pvr_pds_get_max_vertex_program_const_map_size_in_bytes(
         &device->pdevice->dev_info,
         device->features.robustBufferAccess);
   struct pvr_pds_upload *const program = &program_out->program;
   struct pvr_pds_info *const info = &program_out->info;
   struct pvr_const_map_entry *entries_buffer;
   ASSERTED uint32_t code_size_in_dwords;
   size_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   memset(info, 0, sizeof(*info));

   entries_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              const_entries_size_in_bytes,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!entries_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   info->entries = entries_buffer;
   info->entries_size_in_bytes = const_entries_size_in_bytes;

   pvr_pds_generate_vertex_primary_program(input,
                                           NULL,
                                           info,
                                           device->features.robustBufferAccess,
                                           &device->pdevice->dev_info);

   code_size_in_dwords = info->code_size_in_dwords;
   staging_buffer_size = info->code_size_in_dwords * sizeof(*staging_buffer);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      vk_free2(&device->vk.alloc, allocator, entries_buffer);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* This also fills in info->entries. */
   pvr_pds_generate_vertex_primary_program(input,
                                           staging_buffer,
                                           info,
                                           device->features.robustBufferAccess,
                                           &device->pdevice->dev_info);

   assert(info->code_size_in_dwords <= code_size_in_dwords);

   /* FIXME: Add a vk_realloc2() ? */
   entries_buffer = vk_realloc((!allocator) ? &device->vk.alloc : allocator,
                               entries_buffer,
                               info->entries_written_size_in_bytes,
                               8,
                               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!entries_buffer) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   info->entries = entries_buffer;
   info->entries_size_in_bytes = info->entries_written_size_in_bytes;

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               staging_buffer,
                               info->code_size_in_dwords,
                               16,
                               16,
                               program);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, entries_buffer);
      vk_free2(&device->vk.alloc, allocator, staging_buffer);

      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
}

static inline void pvr_pds_vertex_attrib_program_destroy(
   struct pvr_device *const device,
   const struct VkAllocationCallbacks *const allocator,
   struct pvr_pds_attrib_program *const program)
{
   pvr_bo_free(device, program->program.pvr_bo);
   vk_free2(&device->vk.alloc, allocator, program->info.entries);
}

/* This is a const pointer to an array of pvr_pds_attrib_program structs.
 * The array being pointed to is of PVR_PDS_VERTEX_ATTRIB_PROGRAM_COUNT size.
 */
typedef struct pvr_pds_attrib_program (*const pvr_pds_attrib_programs_array_ptr)
   [PVR_PDS_VERTEX_ATTRIB_PROGRAM_COUNT];

/* Generate and uploads a PDS program for DMAing vertex attribs into USC vertex
 * inputs. This will bake the code segment and create a template of the data
 * segment for the command buffer to fill in.
 */
/* If allocator == NULL, the internal one will be used.
 *
 * programs_out_ptr is a pointer to the array where the outputs will be placed.
 * */
static VkResult pvr_pds_vertex_attrib_programs_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *const allocator,
   const VkPipelineVertexInputStateCreateInfo *const vertex_input_state,
   uint32_t usc_temp_count,
   const struct rogue_vs_build_data *vs_data,
   pvr_pds_attrib_programs_array_ptr programs_out_ptr)
{
   struct pvr_pds_vertex_dma dma_descriptions[PVR_MAX_VERTEX_ATTRIB_DMAS];
   struct pvr_pds_attrib_program *const programs_out = *programs_out_ptr;
   struct pvr_pds_vertex_primary_program_input input = {
      .dma_list = dma_descriptions,
   };
   VkResult result;

   pvr_pds_vertex_attrib_init_dma_descriptions(vertex_input_state,
                                               vs_data,
                                               &dma_descriptions,
                                               &input.dma_count);

   pvr_pds_setup_doutu(&input.usc_task_control,
                       0,
                       usc_temp_count,
                       PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                       false);

   /* TODO: If statements for all the "bRequired"s + ui32ExtraFlags. */

   /* Note: programs_out_ptr is a pointer to an array so this is fine. See the
    * typedef.
    */
   for (uint32_t i = 0; i < ARRAY_SIZE(*programs_out_ptr); i++) {
      switch (i) {
      case PVR_PDS_VERTEX_ATTRIB_PROGRAM_BASIC:
         input.flags = 0;
         break;

      case PVR_PDS_VERTEX_ATTRIB_PROGRAM_BASE_INSTANCE:
         input.flags = PVR_PDS_VERTEX_FLAGS_BASE_INSTANCE_VARIANT;
         break;

      case PVR_PDS_VERTEX_ATTRIB_PROGRAM_DRAW_INDIRECT:
         /* We unset INSTANCE and set INDIRECT. */
         input.flags = PVR_PDS_VERTEX_FLAGS_DRAW_INDIRECT_VARIANT;
         break;

      default:
         unreachable("Invalid vertex attrib program type.");
      }

      result =
         pvr_pds_vertex_attrib_program_create_and_upload(device,
                                                         allocator,
                                                         &input,
                                                         &programs_out[i]);
      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            pvr_pds_vertex_attrib_program_destroy(device,
                                                  allocator,
                                                  &programs_out[j]);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

static size_t pvr_pds_get_max_descriptor_upload_const_map_size_in_bytes()
{
   /* Maximum memory allocation needed for const map entries in
    * pvr_pds_generate_descriptor_upload_program().
    * It must be >= 688 bytes. This size is calculated as the sum of:
    *
    *  1. Max. number of descriptor sets (8) * (
    *         size of descriptor entry
    *             (pvr_const_map_entry_descriptor_set) +
    *         size of Common Store burst entry
    *             (pvr_const_map_entry_literal32))
    *
    *  2. Max. number of PDS program buffers (24) * (
    *         size of the largest buffer structure
    *             (pvr_const_map_entry_constant_buffer) +
    *         size of Common Store burst entry
    *             (pvr_const_map_entry_literal32)
    *
    *  3. Size of DOUTU entry (pvr_const_map_entry_doutu_address)
    */

   /* FIXME: PVR_MAX_DESCRIPTOR_SETS is 4 and not 8. The comment above seems to
    * say that it should be 8.
    * Figure our a define for this or is the comment wrong?
    */
   return (8 * (sizeof(struct pvr_const_map_entry_descriptor_set) +
                sizeof(struct pvr_const_map_entry_literal32)) +
           PVR_PDS_MAX_BUFFERS *
              (sizeof(struct pvr_const_map_entry_constant_buffer) +
               sizeof(struct pvr_const_map_entry_literal32)) +
           sizeof(struct pvr_const_map_entry_doutu_address));
}

/* This is a const pointer to an array of PVR_PDS_MAX_BUFFERS pvr_pds_buffer
 * structs.
 */
typedef struct pvr_pds_buffer (
      *const pvr_pds_uniform_program_buffer_array_ptr)[PVR_PDS_MAX_BUFFERS];

static void pvr_pds_uniform_program_setup_buffers(
   bool robust_buffer_access,
   const struct rogue_ubo_data *ubo_data,
   pvr_pds_uniform_program_buffer_array_ptr buffers_out_ptr,
   uint32_t *const buffer_count_out)
{
   struct pvr_pds_buffer *const buffers = *buffers_out_ptr;
   uint32_t buffer_count = 0;

   for (size_t i = 0; i < ubo_data->num_ubo_entries; i++) {
      struct pvr_pds_buffer *current_buffer = &buffers[buffer_count];

      /* This is fine since buffers_out_ptr is a pointer to an array. */
      assert(buffer_count < ARRAY_SIZE(*buffers_out_ptr));

      current_buffer->type = PVR_BUFFER_TYPE_UBO;
      current_buffer->size_in_dwords = ubo_data->size[i];
      current_buffer->destination = ubo_data->dest[i];

      current_buffer->buffer_id = buffer_count;
      current_buffer->desc_set = ubo_data->desc_set[i];
      current_buffer->binding = ubo_data->binding[i];
      /* TODO: Is this always the case?
       * E.g. can multiple UBOs have the same base buffer?
       */
      current_buffer->source_offset = 0;

      buffer_count++;
   }

   *buffer_count_out = buffer_count;
}

static VkResult pvr_pds_uniform_program_create_and_upload(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   const struct rogue_ubo_data *const ubo_data,
   const struct pvr_explicit_constant_usage *const explicit_const_usage,
   const struct pvr_pipeline_layout *const layout,
   enum pvr_stage_allocation stage,
   struct pvr_pds_upload *const pds_code_upload_out,
   struct pvr_pds_info *const pds_info_out)
{
   const size_t const_entries_size_in_bytes =
      pvr_pds_get_max_descriptor_upload_const_map_size_in_bytes();
   struct pvr_descriptor_program_input program = { 0 };
   struct pvr_const_map_entry *entries_buffer;
   ASSERTED uint32_t code_size_in_dwords;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   assert(stage != PVR_STAGE_ALLOCATION_COUNT);

   memset(pds_info_out, 0, sizeof(*pds_info_out));

   pvr_pds_uniform_program_setup_buffers(device->features.robustBufferAccess,
                                         ubo_data,
                                         &program.buffers,
                                         &program.buffer_count);

   for (uint32_t dma = 0; dma < program.buffer_count; dma++) {
      if (program.buffers[dma].type != PVR_BUFFER_TYPES_COMPILE_TIME)
         continue;

      assert(!"Unimplemented");
   }

   if (layout->per_stage_reg_info[stage].primary_dynamic_size_in_dwords)
      assert(!"Unimplemented");

   for (uint32_t set_num = 0; set_num < layout->set_count; set_num++) {
      const struct pvr_descriptor_set_layout_mem_layout *const reg_layout =
         &layout->register_layout_in_dwords_per_stage[stage][set_num];
      const uint32_t start_offset = explicit_const_usage->start_offset;

      /* TODO: Use compiler usage info to optimize this? */

      /* Only dma primaries if they are actually required. */
      if (reg_layout->primary_size) {
         program.descriptor_sets[program.descriptor_set_count++] =
            (struct pvr_pds_descriptor_set){
               .descriptor_set = set_num,
               .size_in_dwords = reg_layout->primary_size,
               .destination = reg_layout->primary_offset + start_offset,
               .primary = true,
            };
      }

      /* Only dma secondaries if they are actually required. */
      if (!reg_layout->secondary_size)
         continue;

      program.descriptor_sets[program.descriptor_set_count++] =
         (struct pvr_pds_descriptor_set){
            .descriptor_set = set_num,
            .size_in_dwords = reg_layout->secondary_size,
            .destination = reg_layout->secondary_offset + start_offset,
         };
   }

   entries_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              const_entries_size_in_bytes,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!entries_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pds_info_out->entries = entries_buffer;
   pds_info_out->entries_size_in_bytes = const_entries_size_in_bytes;

   pvr_pds_generate_descriptor_upload_program(&program, NULL, pds_info_out);

   code_size_in_dwords = pds_info_out->code_size_in_dwords;
   staging_buffer_size =
      pds_info_out->code_size_in_dwords * sizeof(*staging_buffer);

   if (!staging_buffer_size) {
      vk_free2(&device->vk.alloc, allocator, entries_buffer);

      memset(pds_info_out, 0, sizeof(*pds_info_out));
      memset(pds_code_upload_out, 0, sizeof(*pds_code_upload_out));
      return VK_SUCCESS;
   }

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer) {
      vk_free2(&device->vk.alloc, allocator, entries_buffer);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pvr_pds_generate_descriptor_upload_program(&program,
                                              staging_buffer,
                                              pds_info_out);

   assert(pds_info_out->code_size_in_dwords <= code_size_in_dwords);

   /* FIXME: use vk_realloc2() ? */
   entries_buffer = vk_realloc((!allocator) ? &device->vk.alloc : allocator,
                               entries_buffer,
                               pds_info_out->entries_written_size_in_bytes,
                               8,
                               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!entries_buffer) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pds_info_out->entries = entries_buffer;
   pds_info_out->entries_size_in_bytes =
      pds_info_out->entries_written_size_in_bytes;

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               staging_buffer,
                               pds_info_out->code_size_in_dwords,
                               16,
                               16,
                               pds_code_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, entries_buffer);
      vk_free2(&device->vk.alloc, allocator, staging_buffer);

      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
}

static void pvr_pds_uniform_program_destroy(
   struct pvr_device *const device,
   const struct VkAllocationCallbacks *const allocator,
   struct pvr_pds_upload *const pds_code,
   struct pvr_pds_info *const pds_info)
{
   pvr_bo_free(device, pds_code->pvr_bo);
   vk_free2(&device->vk.alloc, allocator, pds_info->entries);
}

static void pvr_pds_compute_program_setup(
   const struct pvr_device_info *dev_info,
   const uint32_t local_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   const uint32_t work_group_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   uint32_t barrier_coefficient,
   bool add_base_workgroup,
   uint32_t usc_temps,
   pvr_dev_addr_t usc_shader_dev_addr,
   struct pvr_pds_compute_shader_program *const program)
{
   *program = (struct pvr_pds_compute_shader_program){
      /* clang-format off */
      .local_input_regs = {
         local_input_regs[0],
         local_input_regs[1],
         local_input_regs[2]
      },
      .work_group_input_regs = {
         work_group_input_regs[0],
         work_group_input_regs[1],
         work_group_input_regs[2]
      },
      .global_input_regs = {
         [0 ... (PVR_WORKGROUP_DIMENSIONS - 1)] =
            PVR_PDS_COMPUTE_INPUT_REG_UNUSED
      },
      /* clang-format on */
      .barrier_coefficient = barrier_coefficient,
      .flattened_work_groups = true,
      .clear_pds_barrier = false,
      .add_base_workgroup = add_base_workgroup,
      .kick_usc = true,
   };

   STATIC_ASSERT(ARRAY_SIZE(program->local_input_regs) ==
                 PVR_WORKGROUP_DIMENSIONS);
   STATIC_ASSERT(ARRAY_SIZE(program->work_group_input_regs) ==
                 PVR_WORKGROUP_DIMENSIONS);
   STATIC_ASSERT(ARRAY_SIZE(program->global_input_regs) ==
                 PVR_WORKGROUP_DIMENSIONS);

   pvr_pds_setup_doutu(&program->usc_task_control,
                       usc_shader_dev_addr.addr,
                       usc_temps,
                       PVRX(PDSINST_DOUTU_SAMPLE_RATE_INSTANCE),
                       false);

   pvr_pds_compute_shader(program, NULL, PDS_GENERATE_SIZES, dev_info);
}

/* FIXME: See if pvr_device_init_compute_pds_program() and this could be merged.
 */
static VkResult pvr_pds_compute_program_create_and_upload(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   const uint32_t local_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   const uint32_t work_group_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   uint32_t barrier_coefficient,
   uint32_t usc_temps,
   pvr_dev_addr_t usc_shader_dev_addr,
   struct pvr_pds_upload *const pds_upload_out,
   struct pvr_pds_info *const pds_info_out)
{
   struct pvr_device_info *dev_info = &device->pdevice->dev_info;
   struct pvr_pds_compute_shader_program program;
   uint32_t staging_buffer_size;
   uint32_t *staging_buffer;
   VkResult result;

   pvr_pds_compute_program_setup(dev_info,
                                 local_input_regs,
                                 work_group_input_regs,
                                 barrier_coefficient,
                                 false,
                                 usc_temps,
                                 usc_shader_dev_addr,
                                 &program);

   /* FIXME: According to pvr_device_init_compute_pds_program() the code size
    * is in bytes. Investigate this.
    */
   staging_buffer_size =
      (program.code_size + program.data_size) * sizeof(*staging_buffer);

   staging_buffer = vk_alloc2(&device->vk.alloc,
                              allocator,
                              staging_buffer_size,
                              8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!staging_buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* FIXME: pvr_pds_compute_shader doesn't implement
    * PDS_GENERATE_CODEDATA_SEGMENTS.
    */
   pvr_pds_compute_shader(&program,
                          &staging_buffer[0],
                          PDS_GENERATE_CODE_SEGMENT,
                          dev_info);

   pvr_pds_compute_shader(&program,
                          &staging_buffer[program.code_size],
                          PDS_GENERATE_DATA_SEGMENT,
                          dev_info);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               &staging_buffer[program.code_size],
                               program.data_size,
                               16,
                               &staging_buffer[0],
                               program.code_size,
                               16,
                               16,
                               pds_upload_out);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, staging_buffer);
      return result;
   }

   *pds_info_out = (struct pvr_pds_info){
      .temps_required = program.highest_temp,
      .code_size_in_dwords = program.code_size,
      .data_size_in_dwords = program.data_size,
   };

   vk_free2(&device->vk.alloc, allocator, staging_buffer);

   return VK_SUCCESS;
};

static void pvr_pds_compute_program_destroy(
   struct pvr_device *const device,
   const struct VkAllocationCallbacks *const allocator,
   struct pvr_pds_upload *const pds_program,
   struct pvr_pds_info *const pds_info)
{
   /* We don't allocate an entries buffer so we don't need to free it */
   pvr_bo_free(device, pds_program->pvr_bo);
}

/* This only uploads the code segment. The data segment will need to be patched
 * with the base workgroup before uploading.
 */
static VkResult pvr_pds_compute_base_workgroup_variant_program_init(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   const uint32_t local_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   const uint32_t work_group_input_regs[static const PVR_WORKGROUP_DIMENSIONS],
   uint32_t barrier_coefficient,
   uint32_t usc_temps,
   pvr_dev_addr_t usc_shader_dev_addr,
   struct pvr_pds_base_workgroup_program *program_out)
{
   struct pvr_device_info *dev_info = &device->pdevice->dev_info;
   struct pvr_pds_compute_shader_program program;
   uint32_t buffer_size;
   uint32_t *buffer;
   VkResult result;

   pvr_pds_compute_program_setup(dev_info,
                                 local_input_regs,
                                 work_group_input_regs,
                                 barrier_coefficient,
                                 true,
                                 usc_temps,
                                 usc_shader_dev_addr,
                                 &program);

   /* FIXME: According to pvr_device_init_compute_pds_program() the code size
    * is in bytes. Investigate this.
    */
   buffer_size = MAX2(program.code_size, program.data_size) * sizeof(*buffer);

   buffer = vk_alloc2(&device->vk.alloc,
                      allocator,
                      buffer_size,
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!buffer)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_pds_compute_shader(&program,
                          &buffer[0],
                          PDS_GENERATE_CODE_SEGMENT,
                          dev_info);

   /* FIXME: Figure out the define for alignment of 16. */
   result = pvr_gpu_upload_pds(device,
                               NULL,
                               0,
                               0,
                               buffer,
                               program.code_size,
                               16,
                               16,
                               &program_out->code_upload);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, buffer);
      return result;
   }

   pvr_pds_compute_shader(&program, buffer, PDS_GENERATE_DATA_SEGMENT, dev_info);

   program_out->data_section = buffer;

   /* We'll need to patch the base workgroup in the PDS data section before
    * dispatch so we save the offsets at which to patch. We only need to save
    * the offset for the first workgroup id since the workgroup ids are stored
    * contiguously in the data segment.
    */
   program_out->base_workgroup_data_patching_offset =
      program.base_workgroup_constant_offset_in_dwords[0];

   program_out->info = (struct pvr_pds_info){
      .temps_required = program.highest_temp,
      .code_size_in_dwords = program.code_size,
      .data_size_in_dwords = program.data_size,
   };

   return VK_SUCCESS;
}

static void pvr_pds_compute_base_workgroup_variant_program_finish(
   struct pvr_device *device,
   const VkAllocationCallbacks *const allocator,
   struct pvr_pds_base_workgroup_program *const state)
{
   pvr_bo_free(device, state->code_upload.pvr_bo);
   vk_free2(&device->vk.alloc, allocator, state->data_section);
}

/******************************************************************************
   Generic pipeline functions
 ******************************************************************************/

static void pvr_pipeline_init(struct pvr_device *device,
                              enum pvr_pipeline_type type,
                              struct pvr_pipeline *const pipeline)
{
   assert(!pipeline->layout);

   vk_object_base_init(&device->vk, &pipeline->base, VK_OBJECT_TYPE_PIPELINE);

   pipeline->type = type;
}

static void pvr_pipeline_finish(struct pvr_pipeline *pipeline)
{
   vk_object_base_finish(&pipeline->base);
}

/******************************************************************************
   Compute pipeline functions
 ******************************************************************************/

/* Compiles and uploads shaders and PDS programs. */
static VkResult pvr_compute_pipeline_compile(
   struct pvr_device *const device,
   struct pvr_pipeline_cache *pipeline_cache,
   const VkComputePipelineCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *const allocator,
   struct pvr_compute_pipeline *const compute_pipeline)
{
   uint32_t work_group_input_regs[PVR_WORKGROUP_DIMENSIONS];
   struct pvr_explicit_constant_usage explicit_const_usage;
   uint32_t local_input_regs[PVR_WORKGROUP_DIMENSIONS];
   struct rogue_ubo_data ubo_data;
   uint32_t barrier_coefficient;
   uint32_t usc_temps;
   VkResult result;

   if (pvr_hard_code_shader_required(&device->pdevice->dev_info)) {
      struct pvr_hard_code_compute_build_info build_info;

      result = pvr_hard_code_compute_pipeline(device,
                                              &compute_pipeline->state.shader,
                                              &build_info);
      if (result != VK_SUCCESS)
         return result;

      ubo_data = build_info.ubo_data;

      /* We make sure that the compiler's unused reg value is compatible with
       * the pds api.
       */
      STATIC_ASSERT(ROGUE_REG_UNUSED == PVR_PDS_COMPUTE_INPUT_REG_UNUSED);

      barrier_coefficient = build_info.barrier_reg;

      /* TODO: Maybe change the pds api to use pointers so we avoid the copy. */
      local_input_regs[0] = build_info.local_invocation_regs[0];
      local_input_regs[1] = build_info.local_invocation_regs[1];
      /* This is not a mistake. We want to assign element 1 to 2. */
      local_input_regs[2] = build_info.local_invocation_regs[1];

      STATIC_ASSERT(
         __same_type(work_group_input_regs, build_info.work_group_regs));
      typed_memcpy(work_group_input_regs,
                   build_info.work_group_regs,
                   PVR_WORKGROUP_DIMENSIONS);

      usc_temps = build_info.usc_temps;

      explicit_const_usage = build_info.explicit_conts_usage;

   } else {
      /* FIXME: Compile and upload the shader. */
      /* FIXME: Initialize the shader state and setup build info. */
      abort();
   };

   result = pvr_pds_uniform_program_create_and_upload(
      device,
      allocator,
      &ubo_data,
      &explicit_const_usage,
      compute_pipeline->base.layout,
      PVR_STAGE_ALLOCATION_COMPUTE,
      &compute_pipeline->state.uniform.pds_code,
      &compute_pipeline->state.uniform.pds_info);
   if (result != VK_SUCCESS)
      goto err_free_shader;

   result = pvr_pds_compute_program_create_and_upload(
      device,
      allocator,
      local_input_regs,
      work_group_input_regs,
      barrier_coefficient,
      usc_temps,
      compute_pipeline->state.shader.bo->vma->dev_addr,
      &compute_pipeline->state.primary_program,
      &compute_pipeline->state.primary_program_info);
   if (result != VK_SUCCESS)
      goto err_free_uniform_program;

   /* If the workgroup ID is required, then we require the base workgroup
    * variant of the PDS compute program as well.
    */
   compute_pipeline->state.flags.base_workgroup =
      work_group_input_regs[0] != PVR_PDS_COMPUTE_INPUT_REG_UNUSED ||
      work_group_input_regs[1] != PVR_PDS_COMPUTE_INPUT_REG_UNUSED ||
      work_group_input_regs[2] != PVR_PDS_COMPUTE_INPUT_REG_UNUSED;

   if (compute_pipeline->state.flags.base_workgroup) {
      result = pvr_pds_compute_base_workgroup_variant_program_init(
         device,
         allocator,
         local_input_regs,
         work_group_input_regs,
         barrier_coefficient,
         usc_temps,
         compute_pipeline->state.shader.bo->vma->dev_addr,
         &compute_pipeline->state.primary_base_workgroup_variant_program);
      if (result != VK_SUCCESS)
         goto err_destroy_compute_program;
   }

   return VK_SUCCESS;

err_destroy_compute_program:
   pvr_pds_compute_program_destroy(
      device,
      allocator,
      &compute_pipeline->state.primary_program,
      &compute_pipeline->state.primary_program_info);

err_free_uniform_program:
   pvr_bo_free(device, compute_pipeline->state.uniform.pds_code.pvr_bo);

err_free_shader:
   pvr_bo_free(device, compute_pipeline->state.shader.bo);

   return result;
}

static VkResult
pvr_compute_pipeline_init(struct pvr_device *device,
                          struct pvr_pipeline_cache *pipeline_cache,
                          const VkComputePipelineCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *allocator,
                          struct pvr_compute_pipeline *compute_pipeline)
{
   VkResult result;

   pvr_pipeline_init(device,
                     PVR_PIPELINE_TYPE_COMPUTE,
                     &compute_pipeline->base);

   compute_pipeline->base.layout =
      pvr_pipeline_layout_from_handle(pCreateInfo->layout);

   result = pvr_compute_pipeline_compile(device,
                                         pipeline_cache,
                                         pCreateInfo,
                                         allocator,
                                         compute_pipeline);
   if (result != VK_SUCCESS) {
      pvr_pipeline_finish(&compute_pipeline->base);
      return result;
   }

   return VK_SUCCESS;
}

static VkResult
pvr_compute_pipeline_create(struct pvr_device *device,
                            struct pvr_pipeline_cache *pipeline_cache,
                            const VkComputePipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *allocator,
                            VkPipeline *const pipeline_out)
{
   struct pvr_compute_pipeline *compute_pipeline;
   VkResult result;

   compute_pipeline = vk_zalloc2(&device->vk.alloc,
                                 allocator,
                                 sizeof(*compute_pipeline),
                                 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!compute_pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Compiles and uploads shaders and PDS programs. */
   result = pvr_compute_pipeline_init(device,
                                      pipeline_cache,
                                      pCreateInfo,
                                      allocator,
                                      compute_pipeline);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, compute_pipeline);
      return result;
   }

   *pipeline_out = pvr_pipeline_to_handle(&compute_pipeline->base);

   return VK_SUCCESS;
}

static void pvr_compute_pipeline_destroy(
   struct pvr_device *const device,
   const VkAllocationCallbacks *const allocator,
   struct pvr_compute_pipeline *const compute_pipeline)
{
   if (compute_pipeline->state.flags.base_workgroup) {
      pvr_pds_compute_base_workgroup_variant_program_finish(
         device,
         allocator,
         &compute_pipeline->state.primary_base_workgroup_variant_program);
   }

   pvr_pds_compute_program_destroy(
      device,
      allocator,
      &compute_pipeline->state.primary_program,
      &compute_pipeline->state.primary_program_info);
   pvr_pds_uniform_program_destroy(device,
                                   allocator,
                                   &compute_pipeline->state.uniform.pds_code,
                                   &compute_pipeline->state.uniform.pds_info);
   pvr_bo_free(device, compute_pipeline->state.shader.bo);

   pvr_pipeline_finish(&compute_pipeline->base);

   vk_free2(&device->vk.alloc, allocator, compute_pipeline);
}

VkResult
pvr_CreateComputePipelines(VkDevice _device,
                           VkPipelineCache pipelineCache,
                           uint32_t createInfoCount,
                           const VkComputePipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   PVR_FROM_HANDLE(pvr_pipeline_cache, pipeline_cache, pipelineCache);
   PVR_FROM_HANDLE(pvr_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      const VkResult local_result =
         pvr_compute_pipeline_create(device,
                                     pipeline_cache,
                                     &pCreateInfos[i],
                                     pAllocator,
                                     &pPipelines[i]);
      if (local_result != VK_SUCCESS) {
         result = local_result;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

/******************************************************************************
   Graphics pipeline functions
 ******************************************************************************/

static inline uint32_t pvr_dynamic_state_bit_from_vk(VkDynamicState state)
{
   switch (state) {
   case VK_DYNAMIC_STATE_VIEWPORT:
      return PVR_DYNAMIC_STATE_BIT_VIEWPORT;
   case VK_DYNAMIC_STATE_SCISSOR:
      return PVR_DYNAMIC_STATE_BIT_SCISSOR;
   case VK_DYNAMIC_STATE_LINE_WIDTH:
      return PVR_DYNAMIC_STATE_BIT_LINE_WIDTH;
   case VK_DYNAMIC_STATE_DEPTH_BIAS:
      return PVR_DYNAMIC_STATE_BIT_DEPTH_BIAS;
   case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
      return PVR_DYNAMIC_STATE_BIT_BLEND_CONSTANTS;
   case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
      return PVR_DYNAMIC_STATE_BIT_STENCIL_COMPARE_MASK;
   case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
      return PVR_DYNAMIC_STATE_BIT_STENCIL_WRITE_MASK;
   case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
      return PVR_DYNAMIC_STATE_BIT_STENCIL_REFERENCE;
   default:
      unreachable("Unsupported state.");
   }
}

static void
pvr_graphics_pipeline_destroy(struct pvr_device *const device,
                              const VkAllocationCallbacks *const allocator,
                              struct pvr_graphics_pipeline *const gfx_pipeline)
{
   const uint32_t num_vertex_attrib_programs =
      ARRAY_SIZE(gfx_pipeline->vertex_shader_state.pds_attrib_programs);

   pvr_pds_uniform_program_destroy(
      device,
      allocator,
      &gfx_pipeline->fragment_shader_state.uniform_state.pds_code,
      &gfx_pipeline->fragment_shader_state.uniform_state.pds_info);

   pvr_pds_uniform_program_destroy(
      device,
      allocator,
      &gfx_pipeline->vertex_shader_state.uniform_state.pds_code,
      &gfx_pipeline->vertex_shader_state.uniform_state.pds_info);

   for (uint32_t i = 0; i < num_vertex_attrib_programs; i++) {
      struct pvr_pds_attrib_program *const attrib_program =
         &gfx_pipeline->vertex_shader_state.pds_attrib_programs[i];

      pvr_pds_vertex_attrib_program_destroy(device, allocator, attrib_program);
   }

   pvr_bo_free(device,
               gfx_pipeline->fragment_shader_state.pds_fragment_program.pvr_bo);
   pvr_bo_free(device,
               gfx_pipeline->fragment_shader_state.pds_coeff_program.pvr_bo);

   pvr_bo_free(device, gfx_pipeline->fragment_shader_state.bo);
   pvr_bo_free(device, gfx_pipeline->vertex_shader_state.bo);

   pvr_pipeline_finish(&gfx_pipeline->base);

   vk_free2(&device->vk.alloc, allocator, gfx_pipeline);
}

static void
pvr_vertex_state_init(struct pvr_graphics_pipeline *gfx_pipeline,
                      const struct rogue_common_build_data *common_data,
                      const struct rogue_vs_build_data *vs_data)
{
   struct pvr_vertex_shader_state *vertex_state =
      &gfx_pipeline->vertex_shader_state;

   /* TODO: Hard coding these for now. These should be populated based on the
    * information returned by the compiler.
    */
   vertex_state->stage_state.const_shared_reg_count = common_data->shareds;
   vertex_state->stage_state.const_shared_reg_offset = 0;
   vertex_state->stage_state.temps_count = common_data->temps;
   vertex_state->stage_state.coefficient_size = common_data->coeffs;
   vertex_state->stage_state.uses_atomic_ops = false;
   vertex_state->stage_state.uses_texture_rw = false;
   vertex_state->stage_state.uses_barrier = false;
   vertex_state->stage_state.has_side_effects = false;
   vertex_state->stage_state.empty_program = false;

   vertex_state->vertex_input_size = vs_data->num_vertex_input_regs;
   vertex_state->vertex_output_size =
      vs_data->num_vertex_outputs * ROGUE_REG_SIZE_BYTES;
   vertex_state->user_clip_planes_mask = 0;
   vertex_state->entry_offset = 0;

   /* TODO: The number of varyings should be checked against the fragment
    * shader inputs and assigned in the place where that happens.
    * There will also be an opportunity to cull unused fs inputs/vs outputs.
    */
   pvr_csb_pack (&gfx_pipeline->vertex_shader_state.varying[0],
                 TA_STATE_VARYING0,
                 varying0) {
      varying0.f32_linear = vs_data->num_varyings;
      varying0.f32_flat = 0;
      varying0.f32_npc = 0;
   }

   pvr_csb_pack (&gfx_pipeline->vertex_shader_state.varying[1],
                 TA_STATE_VARYING1,
                 varying1) {
      varying1.f16_linear = 0;
      varying1.f16_flat = 0;
      varying1.f16_npc = 0;
   }
}

static void
pvr_fragment_state_init(struct pvr_graphics_pipeline *gfx_pipeline,
                        const struct rogue_common_build_data *common_data)
{
   struct pvr_fragment_shader_state *fragment_state =
      &gfx_pipeline->fragment_shader_state;

   /* TODO: Hard coding these for now. These should be populated based on the
    * information returned by the compiler.
    */
   fragment_state->stage_state.const_shared_reg_count = 0;
   fragment_state->stage_state.const_shared_reg_offset = 0;
   fragment_state->stage_state.temps_count = common_data->temps;
   fragment_state->stage_state.coefficient_size = common_data->coeffs;
   fragment_state->stage_state.uses_atomic_ops = false;
   fragment_state->stage_state.uses_texture_rw = false;
   fragment_state->stage_state.uses_barrier = false;
   fragment_state->stage_state.has_side_effects = false;
   fragment_state->stage_state.empty_program = false;

   fragment_state->pass_type = 0;
   fragment_state->entry_offset = 0;
}

/* Compiles and uploads shaders and PDS programs. */
static VkResult
pvr_graphics_pipeline_compile(struct pvr_device *const device,
                              struct pvr_pipeline_cache *pipeline_cache,
                              const VkGraphicsPipelineCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *const allocator,
                              struct pvr_graphics_pipeline *const gfx_pipeline)
{
   /* FIXME: Remove this hard coding. */
   struct pvr_explicit_constant_usage vert_explicit_const_usage = {
      .start_offset = 16,
   };
   struct pvr_explicit_constant_usage frag_explicit_const_usage = {
      .start_offset = 0,
   };
   static uint32_t hard_code_pipeline_n = 0;

   const VkPipelineVertexInputStateCreateInfo *const vertex_input_state =
      pCreateInfo->pVertexInputState;
   const uint32_t cache_line_size =
      rogue_get_slc_cache_line_size(&device->pdevice->dev_info);
   struct rogue_compiler *compiler = device->pdevice->compiler;
   struct rogue_build_ctx *ctx;
   VkResult result;

   /* Setup shared build context. */
   ctx = rogue_create_build_context(compiler);
   if (!ctx)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* NIR middle-end translation. */
   for (gl_shader_stage stage = MESA_SHADER_FRAGMENT; stage > MESA_SHADER_NONE;
        stage--) {
      const VkPipelineShaderStageCreateInfo *create_info;
      size_t stage_index = gfx_pipeline->stage_indices[stage];

      if (pvr_hard_code_shader_required(&device->pdevice->dev_info)) {
         if (pvr_hard_code_graphics_get_flags(&device->pdevice->dev_info) &
             BITFIELD_BIT(stage)) {
            continue;
         }
      }

      /* Skip unused/inactive stages. */
      if (stage_index == ~0)
         continue;

      create_info = &pCreateInfo->pStages[stage_index];

      /* SPIR-V to NIR. */
      ctx->nir[stage] = pvr_spirv_to_nir(ctx, stage, create_info);
      if (!ctx->nir[stage]) {
         ralloc_free(ctx);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   /* Pre-back-end analysis and optimization, driver data extraction. */
   /* TODO: Analyze and cull unused I/O between stages. */
   /* TODO: Allocate UBOs between stages;
    * pipeline->layout->set_{count,layout}.
    */

   /* Back-end translation. */
   for (gl_shader_stage stage = MESA_SHADER_FRAGMENT; stage > MESA_SHADER_NONE;
        stage--) {
      if (pvr_hard_code_shader_required(&device->pdevice->dev_info) &&
          pvr_hard_code_graphics_get_flags(&device->pdevice->dev_info) &
             BITFIELD_BIT(stage)) {
         const struct pvr_device_info *const dev_info =
            &device->pdevice->dev_info;
         struct pvr_explicit_constant_usage *explicit_const_usage;

         switch (stage) {
         case MESA_SHADER_VERTEX:
            explicit_const_usage = &vert_explicit_const_usage;
            break;

         case MESA_SHADER_FRAGMENT:
            explicit_const_usage = &frag_explicit_const_usage;
            break;

         default:
            unreachable("Unsupported stage.");
         }

         pvr_hard_code_graphics_shader(dev_info,
                                       hard_code_pipeline_n,
                                       stage,
                                       &ctx->binary[stage]);

         pvr_hard_code_graphics_get_build_info(dev_info,
                                               hard_code_pipeline_n,
                                               stage,
                                               &ctx->common_data[stage],
                                               &ctx->stage_data,
                                               explicit_const_usage);

         continue;
      }

      if (!ctx->nir[stage])
         continue;

      ctx->rogue[stage] = pvr_nir_to_rogue(ctx, ctx->nir[stage]);
      if (!ctx->rogue[stage]) {
         ralloc_free(ctx);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      ctx->binary[stage] = pvr_rogue_to_binary(ctx, ctx->rogue[stage]);
      if (!ctx->binary[stage]) {
         ralloc_free(ctx);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   if (pvr_hard_code_shader_required(&device->pdevice->dev_info) &&
       pvr_hard_code_graphics_get_flags(&device->pdevice->dev_info) &
          BITFIELD_BIT(MESA_SHADER_VERTEX)) {
      pvr_hard_code_graphics_vertex_state(&device->pdevice->dev_info,
                                          hard_code_pipeline_n,
                                          &gfx_pipeline->vertex_shader_state);
   } else {
      pvr_vertex_state_init(gfx_pipeline,
                            &ctx->common_data[MESA_SHADER_VERTEX],
                            &ctx->stage_data.vs);
   }

   result = pvr_gpu_upload_usc(device,
                               ctx->binary[MESA_SHADER_VERTEX]->data,
                               ctx->binary[MESA_SHADER_VERTEX]->size,
                               cache_line_size,
                               &gfx_pipeline->vertex_shader_state.bo);
   if (result != VK_SUCCESS)
      goto err_free_build_context;

   if (pvr_hard_code_shader_required(&device->pdevice->dev_info) &&
       pvr_hard_code_graphics_get_flags(&device->pdevice->dev_info) &
          BITFIELD_BIT(MESA_SHADER_FRAGMENT)) {
      pvr_hard_code_graphics_fragment_state(
         &device->pdevice->dev_info,
         hard_code_pipeline_n,
         &gfx_pipeline->fragment_shader_state);
   } else {
      pvr_fragment_state_init(gfx_pipeline,
                              &ctx->common_data[MESA_SHADER_FRAGMENT]);
   }

   result = pvr_gpu_upload_usc(device,
                               ctx->binary[MESA_SHADER_FRAGMENT]->data,
                               ctx->binary[MESA_SHADER_FRAGMENT]->size,
                               cache_line_size,
                               &gfx_pipeline->fragment_shader_state.bo);
   if (result != VK_SUCCESS)
      goto err_free_vertex_bo;

   /* TODO: powervr has an optimization where it attempts to recompile shaders.
    * See PipelineCompileNoISPFeedbackFragmentStage. Unimplemented since in our
    * case the optimization doesn't happen.
    */

   /* TODO: The programs we use are hard coded for now, but these should be
    * selected dynamically.
    */

   result = pvr_pds_coeff_program_create_and_upload(
      device,
      allocator,
      ctx->stage_data.fs.iterator_args.fpu_iterators,
      ctx->stage_data.fs.iterator_args.num_fpu_iterators,
      ctx->stage_data.fs.iterator_args.destination,
      &gfx_pipeline->fragment_shader_state.pds_coeff_program);
   if (result != VK_SUCCESS)
      goto err_free_fragment_bo;

   result = pvr_pds_fragment_program_create_and_upload(
      device,
      allocator,
      gfx_pipeline->fragment_shader_state.bo,
      ctx->common_data[MESA_SHADER_FRAGMENT].temps,
      ctx->stage_data.fs.msaa_mode,
      ctx->stage_data.fs.phas,
      &gfx_pipeline->fragment_shader_state.pds_fragment_program);
   if (result != VK_SUCCESS)
      goto err_free_coeff_program;

   result = pvr_pds_vertex_attrib_programs_create_and_upload(
      device,
      allocator,
      vertex_input_state,
      ctx->common_data[MESA_SHADER_VERTEX].temps,
      &ctx->stage_data.vs,
      &gfx_pipeline->vertex_shader_state.pds_attrib_programs);
   if (result != VK_SUCCESS)
      goto err_free_frag_program;

   result = pvr_pds_uniform_program_create_and_upload(
      device,
      allocator,
      &ctx->common_data[MESA_SHADER_VERTEX].ubo_data,
      &vert_explicit_const_usage,
      gfx_pipeline->base.layout,
      PVR_STAGE_ALLOCATION_VERTEX_GEOMETRY,
      &gfx_pipeline->vertex_shader_state.uniform_state.pds_code,
      &gfx_pipeline->vertex_shader_state.uniform_state.pds_info);
   if (result != VK_SUCCESS)
      goto err_free_vertex_attrib_program;

   /* FIXME: When the temp_buffer_total_size is non-zero we need to allocate a
    * scratch buffer for both vertex and fragment stage.
    * Figure out the best place to do this.
    */
   /* assert(pvr_pds_uniform_program_variables.temp_buff_total_size == 0); */
   /* TODO: Implement spilling with the above. */

   /* TODO: Call pvr_pds_uniform_program_create_and_upload in a loop. */
   /* FIXME: For now we pass in the same explicit_const_usage since it contains
    * all invalid entries. Fix this by hooking it up to the compiler.
    */
   result = pvr_pds_uniform_program_create_and_upload(
      device,
      allocator,
      &ctx->common_data[MESA_SHADER_FRAGMENT].ubo_data,
      &frag_explicit_const_usage,
      gfx_pipeline->base.layout,
      PVR_STAGE_ALLOCATION_FRAGMENT,
      &gfx_pipeline->fragment_shader_state.uniform_state.pds_code,
      &gfx_pipeline->fragment_shader_state.uniform_state.pds_info);
   if (result != VK_SUCCESS)
      goto err_free_vertex_uniform_program;

   ralloc_free(ctx);

   hard_code_pipeline_n++;

   return VK_SUCCESS;

err_free_vertex_uniform_program:
   pvr_pds_uniform_program_destroy(
      device,
      allocator,
      &gfx_pipeline->vertex_shader_state.uniform_state.pds_code,
      &gfx_pipeline->vertex_shader_state.uniform_state.pds_info);
err_free_vertex_attrib_program:
   for (uint32_t i = 0;
        i < ARRAY_SIZE(gfx_pipeline->vertex_shader_state.pds_attrib_programs);
        i++) {
      struct pvr_pds_attrib_program *const attrib_program =
         &gfx_pipeline->vertex_shader_state.pds_attrib_programs[i];

      pvr_pds_vertex_attrib_program_destroy(device, allocator, attrib_program);
   }
err_free_frag_program:
   pvr_bo_free(device,
               gfx_pipeline->fragment_shader_state.pds_fragment_program.pvr_bo);
err_free_coeff_program:
   pvr_bo_free(device,
               gfx_pipeline->fragment_shader_state.pds_coeff_program.pvr_bo);
err_free_fragment_bo:
   pvr_bo_free(device, gfx_pipeline->fragment_shader_state.bo);
err_free_vertex_bo:
   pvr_bo_free(device, gfx_pipeline->vertex_shader_state.bo);
err_free_build_context:
   ralloc_free(ctx);
   return result;
}

static void pvr_graphics_pipeline_init_depth_and_stencil_state(
   struct pvr_graphics_pipeline *gfx_pipeline,
   const VkPipelineDepthStencilStateCreateInfo *depth_stencil_state)
{
   const VkStencilOpState *front;
   const VkStencilOpState *back;

   if (!depth_stencil_state)
      return;

   front = &depth_stencil_state->front;
   back = &depth_stencil_state->back;

   if (depth_stencil_state->depthTestEnable) {
      gfx_pipeline->depth_compare_op = depth_stencil_state->depthCompareOp;
      gfx_pipeline->depth_write_disable =
         !depth_stencil_state->depthWriteEnable;
   } else {
      gfx_pipeline->depth_compare_op = VK_COMPARE_OP_ALWAYS;
      gfx_pipeline->depth_write_disable = true;
   }

   if (depth_stencil_state->stencilTestEnable) {
      gfx_pipeline->stencil_front.compare_op = front->compareOp;
      gfx_pipeline->stencil_front.fail_op = front->failOp;
      gfx_pipeline->stencil_front.depth_fail_op = front->depthFailOp;
      gfx_pipeline->stencil_front.pass_op = front->passOp;

      gfx_pipeline->stencil_back.compare_op = back->compareOp;
      gfx_pipeline->stencil_back.fail_op = back->failOp;
      gfx_pipeline->stencil_back.depth_fail_op = back->depthFailOp;
      gfx_pipeline->stencil_back.pass_op = back->passOp;
   } else {
      gfx_pipeline->stencil_front.compare_op = VK_COMPARE_OP_ALWAYS;
      gfx_pipeline->stencil_front.fail_op = VK_STENCIL_OP_KEEP;
      gfx_pipeline->stencil_front.depth_fail_op = VK_STENCIL_OP_KEEP;
      gfx_pipeline->stencil_front.pass_op = VK_STENCIL_OP_KEEP;

      gfx_pipeline->stencil_back = gfx_pipeline->stencil_front;
   }
}

static void pvr_graphics_pipeline_init_dynamic_state(
   struct pvr_graphics_pipeline *gfx_pipeline,
   const VkPipelineDynamicStateCreateInfo *dynamic_state,
   const VkPipelineViewportStateCreateInfo *viewport_state,
   const VkPipelineDepthStencilStateCreateInfo *depth_stencil_state,
   const VkPipelineColorBlendStateCreateInfo *color_blend_state,
   const VkPipelineRasterizationStateCreateInfo *rasterization_state)
{
   struct pvr_dynamic_state *const internal_dynamic_state =
      &gfx_pipeline->dynamic_state;
   uint32_t dynamic_states = 0;

   if (dynamic_state) {
      for (uint32_t i = 0; i < dynamic_state->dynamicStateCount; i++) {
         dynamic_states |=
            pvr_dynamic_state_bit_from_vk(dynamic_state->pDynamicStates[i]);
      }
   }

   /* TODO: Verify this.
    * We don't zero out the pipeline's state if they are dynamic since they
    * should be set later on in the command buffer.
    */

   /* TODO: Handle rasterizerDiscardEnable. */

   if (rasterization_state) {
      if (!(dynamic_states & PVR_DYNAMIC_STATE_BIT_LINE_WIDTH))
         internal_dynamic_state->line_width = rasterization_state->lineWidth;

      /* TODO: Do we need the depthBiasEnable check? */
      if (!(dynamic_states & PVR_DYNAMIC_STATE_BIT_DEPTH_BIAS)) {
         internal_dynamic_state->depth_bias.constant_factor =
            rasterization_state->depthBiasConstantFactor;
         internal_dynamic_state->depth_bias.clamp =
            rasterization_state->depthBiasClamp;
         internal_dynamic_state->depth_bias.slope_factor =
            rasterization_state->depthBiasSlopeFactor;
      }
   }

   /* TODO: handle viewport state flags. */

   /* TODO: handle static viewport state. */
   /* We assume the viewport state to by dynamic for now. */

   /* TODO: handle static scissor state. */
   /* We assume the scissor state to by dynamic for now. */

   if (depth_stencil_state) {
      const VkStencilOpState *const front = &depth_stencil_state->front;
      const VkStencilOpState *const back = &depth_stencil_state->back;

      /* VkPhysicalDeviceFeatures->depthBounds is false. */
      assert(depth_stencil_state->depthBoundsTestEnable == VK_FALSE);

      if (!(dynamic_states & PVR_DYNAMIC_STATE_BIT_STENCIL_COMPARE_MASK)) {
         internal_dynamic_state->compare_mask.front = front->compareMask;
         internal_dynamic_state->compare_mask.back = back->compareMask;
      }

      if (!(dynamic_states & PVR_DYNAMIC_STATE_BIT_STENCIL_WRITE_MASK)) {
         internal_dynamic_state->write_mask.front = front->writeMask;
         internal_dynamic_state->write_mask.back = back->writeMask;
      }

      if (!(dynamic_states & PVR_DYNAMIC_STATE_BIT_STENCIL_REFERENCE)) {
         internal_dynamic_state->reference.front = front->reference;
         internal_dynamic_state->reference.back = back->reference;
      }
   }

   if (color_blend_state &&
       !(dynamic_states & PVR_DYNAMIC_STATE_BIT_BLEND_CONSTANTS)) {
      STATIC_ASSERT(__same_type(internal_dynamic_state->blend_constants,
                                color_blend_state->blendConstants));

      typed_memcpy(internal_dynamic_state->blend_constants,
                   color_blend_state->blendConstants,
                   ARRAY_SIZE(internal_dynamic_state->blend_constants));
   }

   /* TODO: handle STATIC_STATE_DEPTH_BOUNDS ? */

   internal_dynamic_state->mask = dynamic_states;
}

static VkResult
pvr_graphics_pipeline_init(struct pvr_device *device,
                           struct pvr_pipeline_cache *pipeline_cache,
                           const VkGraphicsPipelineCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *allocator,
                           struct pvr_graphics_pipeline *gfx_pipeline)
{
   /* If rasterization is not enabled, various CreateInfo structs must be
    * ignored.
    */
   const bool raster_discard_enabled =
      pCreateInfo->pRasterizationState->rasterizerDiscardEnable;
   const VkPipelineViewportStateCreateInfo *vs_info =
      !raster_discard_enabled ? pCreateInfo->pViewportState : NULL;
   const VkPipelineDepthStencilStateCreateInfo *dss_info =
      !raster_discard_enabled ? pCreateInfo->pDepthStencilState : NULL;
   const VkPipelineRasterizationStateCreateInfo *rs_info =
      !raster_discard_enabled ? pCreateInfo->pRasterizationState : NULL;
   const VkPipelineColorBlendStateCreateInfo *cbs_info =
      !raster_discard_enabled ? pCreateInfo->pColorBlendState : NULL;
   const VkPipelineMultisampleStateCreateInfo *ms_info =
      !raster_discard_enabled ? pCreateInfo->pMultisampleState : NULL;
   VkResult result;

   pvr_pipeline_init(device, PVR_PIPELINE_TYPE_GRAPHICS, &gfx_pipeline->base);

   pvr_finishme("ignoring pCreateInfo flags.");
   pvr_finishme("ignoring pipeline cache.");

   gfx_pipeline->raster_state.discard_enable = raster_discard_enabled;
   gfx_pipeline->raster_state.cull_mode =
      pCreateInfo->pRasterizationState->cullMode;
   gfx_pipeline->raster_state.front_face =
      pCreateInfo->pRasterizationState->frontFace;
   gfx_pipeline->raster_state.depth_bias_enable =
      pCreateInfo->pRasterizationState->depthBiasEnable;
   gfx_pipeline->raster_state.depth_clamp_enable =
      pCreateInfo->pRasterizationState->depthClampEnable;

   /* FIXME: Handle depthClampEnable. */

   pvr_graphics_pipeline_init_depth_and_stencil_state(gfx_pipeline, dss_info);
   pvr_graphics_pipeline_init_dynamic_state(gfx_pipeline,
                                            pCreateInfo->pDynamicState,
                                            vs_info,
                                            dss_info,
                                            cbs_info,
                                            rs_info);

   if (pCreateInfo->pInputAssemblyState) {
      gfx_pipeline->input_asm_state.topology =
         pCreateInfo->pInputAssemblyState->topology;
      gfx_pipeline->input_asm_state.primitive_restart =
         pCreateInfo->pInputAssemblyState->primitiveRestartEnable;
   }

   memset(gfx_pipeline->stage_indices, ~0, sizeof(gfx_pipeline->stage_indices));

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      VkShaderStageFlagBits vk_stage = pCreateInfo->pStages[i].stage;
      gl_shader_stage gl_stage = vk_to_mesa_shader_stage(vk_stage);
      /* From the Vulkan 1.2.192 spec for VkPipelineShaderStageCreateInfo:
       *
       *    "stage must not be VK_SHADER_STAGE_ALL_GRAPHICS,
       *    or VK_SHADER_STAGE_ALL."
       *
       * So we don't handle that.
       *
       * We also don't handle VK_SHADER_STAGE_TESSELLATION_* and
       * VK_SHADER_STAGE_GEOMETRY_BIT stages as 'tessellationShader' and
       * 'geometryShader' are set to false in the VkPhysicalDeviceFeatures
       * structure returned by the driver.
       */
      switch (pCreateInfo->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         gfx_pipeline->stage_indices[gl_stage] = i;
         break;
      default:
         unreachable("Unsupported stage.");
      }
   }

   gfx_pipeline->base.layout =
      pvr_pipeline_layout_from_handle(pCreateInfo->layout);

   if (ms_info) {
      gfx_pipeline->rasterization_samples = ms_info->rasterizationSamples;
      gfx_pipeline->sample_mask =
         (ms_info->pSampleMask) ? ms_info->pSampleMask[0] : 0xFFFFFFFF;
   } else {
      gfx_pipeline->rasterization_samples = VK_SAMPLE_COUNT_1_BIT;
      gfx_pipeline->sample_mask = 0xFFFFFFFF;
   }

   /* Compiles and uploads shaders and PDS programs. */
   result = pvr_graphics_pipeline_compile(device,
                                          pipeline_cache,
                                          pCreateInfo,
                                          allocator,
                                          gfx_pipeline);
   if (result != VK_SUCCESS) {
      pvr_pipeline_finish(&gfx_pipeline->base);
      return result;
   }

   return VK_SUCCESS;
}

/* If allocator == NULL, the internal one will be used. */
static VkResult
pvr_graphics_pipeline_create(struct pvr_device *device,
                             struct pvr_pipeline_cache *pipeline_cache,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *allocator,
                             VkPipeline *const pipeline_out)
{
   struct pvr_graphics_pipeline *gfx_pipeline;
   VkResult result;

   gfx_pipeline = vk_zalloc2(&device->vk.alloc,
                             allocator,
                             sizeof(*gfx_pipeline),
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!gfx_pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Compiles and uploads shaders and PDS programs too. */
   result = pvr_graphics_pipeline_init(device,
                                       pipeline_cache,
                                       pCreateInfo,
                                       allocator,
                                       gfx_pipeline);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, allocator, gfx_pipeline);
      return result;
   }

   *pipeline_out = pvr_pipeline_to_handle(&gfx_pipeline->base);

   return VK_SUCCESS;
}

VkResult
pvr_CreateGraphicsPipelines(VkDevice _device,
                            VkPipelineCache pipelineCache,
                            uint32_t createInfoCount,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   PVR_FROM_HANDLE(pvr_pipeline_cache, pipeline_cache, pipelineCache);
   PVR_FROM_HANDLE(pvr_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      const VkResult local_result =
         pvr_graphics_pipeline_create(device,
                                      pipeline_cache,
                                      &pCreateInfos[i],
                                      pAllocator,
                                      &pPipelines[i]);
      if (local_result != VK_SUCCESS) {
         result = local_result;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

/*****************************************************************************
   Other functions
*****************************************************************************/

void pvr_DestroyPipeline(VkDevice _device,
                         VkPipeline _pipeline,
                         const VkAllocationCallbacks *pAllocator)
{
   PVR_FROM_HANDLE(pvr_pipeline, pipeline, _pipeline);
   PVR_FROM_HANDLE(pvr_device, device, _device);

   if (!pipeline)
      return;

   switch (pipeline->type) {
   case PVR_PIPELINE_TYPE_GRAPHICS: {
      struct pvr_graphics_pipeline *const gfx_pipeline =
         to_pvr_graphics_pipeline(pipeline);

      pvr_graphics_pipeline_destroy(device, pAllocator, gfx_pipeline);
      break;
   }

   case PVR_PIPELINE_TYPE_COMPUTE: {
      struct pvr_compute_pipeline *const compute_pipeline =
         to_pvr_compute_pipeline(pipeline);

      pvr_compute_pipeline_destroy(device, pAllocator, compute_pipeline);
      break;
   }

   default:
      unreachable("Unknown pipeline type.");
   }
}
