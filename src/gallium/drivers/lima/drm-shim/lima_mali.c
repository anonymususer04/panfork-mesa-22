/*
 * Copyright (C) 2021 Icecream95
 * Copyright (C) 2019 Google LLC
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include "drm-shim/drm_shim.h"
#include "drm-uapi/lima_drm.h"

#include "util/u_math.h"

#include "mali_osk_types.h"
#include "mali_utgard_uk_types.h"
#include "mali_utgard_ioctl.h"

bool drm_shim_driver_prefers_first_render_node = true;

static int kbase_fd = -1;
static FILE *log_file;

static void
lima_open_kbase(void)
{
   if (kbase_fd != -1)
      return;

   kbase_fd = open("/dev/mali", O_RDWR);
   if (kbase_fd == -1)
      perror("open(\"/dev/mali\")");

   const char *log = getenv("LIMA_KBASE_LOG");
   if (log)
      log_file = fopen(log, "a");
   else
      log_file = stderr;

   if (!log_file) {
      perror("fopen(getenv(\"LIMA_KBASE_LOG\"))");
      log_file = stderr;
   }

   close(shim_device.mem_fd);
   shim_device.mem_fd = kbase_fd;
}

static int
lima_ioctl_noop(int fd, unsigned long request, void *arg)
{
   return 0;
}

static int
lima_ioctl_get_param(int fd, unsigned long request, void *arg)
{
   struct drm_lima_get_param *gp = arg;

   lima_open_kbase();

   switch (gp->param) {
   case DRM_LIMA_PARAM_GPU_ID: {
      // TODO: Don't hardcode GPU ID
      gp->value = DRM_LIMA_PARAM_GPU_ID_MALI450;
      return 0;
   }
   case DRM_LIMA_PARAM_NUM_PP: {
      _mali_uk_get_pp_number_of_cores_s cores = {0};
      drmIoctl(kbase_fd, MALI_IOC_PP_NUMBER_OF_CORES_GET, &cores);
      gp->value = cores.number_of_enabled_cores;
      return 0;
   }
   default:
      fprintf(log_file, "Unknown DRM_IOCTL_LIMA_GET_PARAM %d\n", gp->param);
      return -1;
   }
}

static int
lima_ioctl_gem_create(int fd, unsigned long request, void *arg)
{
   struct drm_lima_gem_create *create = arg;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = calloc(1, sizeof(*bo));
   size_t size = ALIGN(create->size, 4096);

   drm_shim_bo_init(bo, size);

   create->handle = drm_shim_bo_get_handle(shim_fd, bo);

   drm_shim_bo_put(bo);

   return 0;
}

static int
lima_ioctl_gem_info(int fd, unsigned long request, void *arg)
{
   struct drm_lima_gem_info *gem_info = arg;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, gem_info->handle);

   gem_info->va = bo->mem_addr;
   gem_info->offset = drm_shim_bo_get_mmap_offset(shim_fd, bo);

   return 0;
}

static void
on_alarm(int sig)
{
}

static int
lima_ioctl_gem_submit(int fd, unsigned long request, void *arg)
{
   struct drm_lima_gem_submit *submit = arg;

   if (submit->pipe == LIMA_PIPE_GP) {
      _mali_uk_gp_start_job_s job = {
         .fence.sync_fd = -1,
      };

      memcpy(&job.frame_registers, (void *)(uintptr_t)submit->frame, sizeof(struct drm_lima_gp_frame));

      drmIoctl(kbase_fd, MALI_IOC_GP2_START_JOB, &job);
   }
   else {
      struct drm_lima_m450_pp_frame *frame = (void *)(uintptr_t)submit->frame;

      _mali_uk_pp_start_job_s job = {
         .fence.sync_fd = -1,
      };

      memcpy(&job.frame_registers, &frame->frame, sizeof(frame->frame));

      if (frame->use_dlbu) {
         memcpy(&job.dlbu_registers, &frame->dlbu_regs, sizeof(frame->dlbu_regs));
      } else {
         job.num_cores = frame->num_pp;
         job.frame_registers[0] = frame->plbu_array_address[0];
         memcpy(&job.frame_registers_addr_frame, &(frame->plbu_array_address[1]), sizeof(frame->plbu_array_address) - 4);
      }

      memcpy(&job.frame_registers_addr_stack, &(frame->fragment_stack_address[1]), sizeof(frame->fragment_stack_address) - 4);
      memcpy(&job.wb0_registers, &frame->wb, sizeof(frame->wb));

      const uint32_t *fp_pointer = (u32 *)&job.frame_registers;

      if (0) {
#define DUMP(name) fprintf(stderr, "0x%08x %10u %s\n", *fp_pointer, *fp_pointer, name); ++fp_pointer
              DUMP("Renderer List Address Register");
              DUMP("Renderer State Word Base Address Register");
              DUMP("Renderer Vertex Base Register");
              DUMP("Feature Enable Register");
              DUMP("Z Clear Value Register");
              DUMP("Stencil Clear Value Register");
              DUMP("ABGR Clear Value 0 Register");
              DUMP("ABGR Clear Value 1 Register");
              DUMP("ABGR Clear Value 2 Register");
              DUMP("ABGR Clear Value 3 Register");
              DUMP("Bounding Box Left Right Register");
              DUMP("Bounding Box Bottom Register");
              DUMP("FS Stack Address Register");
              DUMP("FS Stack Size and Initial Value Register");
              DUMP("Reserved");
              DUMP("Reserved");
              DUMP("Origin Offset X Register");
              DUMP("Origin Offset Y Register");
              DUMP("Subpixel Specifier Register");
              DUMP("Tiebreak mode Register");
              DUMP("Polygon List Format Register");
              DUMP("Scaling Register");
              DUMP("Tilebuffer configuration Register");
#undef DUMP
      }

      drmIoctl(kbase_fd, MALI_IOC_PP_START_JOB, &job);
   }

   struct sigaction act = {
           .sa_handler = on_alarm,
   };
   sigaction(SIGALRM, &act, NULL);
   alarm(5);

   _mali_uk_wait_for_notification_s notification = {0};
   /* Use ioctl and not drmIoctl because we don't want to restart on EINTR */
   ioctl(kbase_fd, MALI_IOC_WAIT_FOR_NOTIFICATION, &notification);

   switch (notification.type) {
   case _MALI_NOTIFICATION_GP_FINISHED: {
      _mali_uk_gp_job_finished_s *info =
         &notification.data.gp_job_finished;

      if (info->status != 0x10000) {
         fprintf(log_file, ".data.gp_job_finished = {\n");

         fprintf(log_file, "\t.user_job_ptr = 0x%"PRIx64",\n", (uint64_t)info->user_job_ptr);
         fprintf(log_file, "\t.status = 0x%x,\n", info->status);
         fprintf(log_file, "\t.heap_current_addr = 0x%x,\n",
                 info->heap_current_addr);

         fprintf(log_file, "},\n");
      }
      break;
   }
   case _MALI_NOTIFICATION_PP_FINISHED: {
      _mali_uk_pp_job_finished_s *info =
         &notification.data.pp_job_finished;

      if (info->status != 0x10000) {
         fprintf(log_file, ".data.pp_job_finished = {\n");

         fprintf(log_file, "\t.user_job_ptr = 0x%"PRIx64",\n", (uint64_t)info->user_job_ptr);
         fprintf(log_file, "\t.status = 0x%x,\n", info->status);

         fprintf(log_file, "},\n");
      }
      break;
   }
   case _MALI_NOTIFICATION_GP_STALLED:
      fprintf(log_file, "gp_job_suspended: 0x%x\n",
             notification.data.gp_job_suspended.cookie);
      break;
   default:
      break;
   }

   return 0;
}

static ioctl_fn_t driver_ioctls[] = {
   [DRM_LIMA_GET_PARAM] = lima_ioctl_get_param,
   [DRM_LIMA_GEM_CREATE] = lima_ioctl_gem_create,
   [DRM_LIMA_GEM_INFO] = lima_ioctl_gem_info,
   [DRM_LIMA_GEM_SUBMIT] = lima_ioctl_gem_submit,
   [DRM_LIMA_GEM_WAIT] = lima_ioctl_noop,
   [DRM_LIMA_CTX_CREATE] = lima_ioctl_noop,
   [DRM_LIMA_CTX_FREE] = lima_ioctl_noop,
};

void
drm_shim_driver_init(void)
{
   shim_device.bus_type = DRM_BUS_PLATFORM;
   shim_device.driver_name = "lima";
   shim_device.driver_ioctls = driver_ioctls;
   shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);

   /* lima uses the DRM version to expose features, instead of getparam. */
   shim_device.version_major = 1;
   shim_device.version_minor = 0;
   shim_device.version_patchlevel = 0;

   drm_shim_override_file("DRIVER=lima\n"
                          "OF_FULLNAME=/soc/mali\n"
                          "OF_COMPATIBLE_0=arm,mali-400\n"
                          "OF_COMPATIBLE_N=1\n",
                          "/sys/dev/char/%d:%d/device/uevent", DRM_MAJOR,
                          render_node_minor);
}
