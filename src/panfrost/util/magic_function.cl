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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#define DIV_ROUND_UP( A, B )  ( ((A) + (B) - 1) / (B) )

#define uint32_t uint

/* Could be uchar, but Midgard has very buggy 8-bit support  */
#define blocksz_t uint

unsigned
pan_afbc_data_size(uint32_t data, unsigned literal_size)
{
        data &= 63;
        return (data == 1) ? literal_size : data;
}

unsigned
pan_afbc_decode_data_size(uint4 header, unsigned literal_size)
{
        /* The size is stored in 16 six-bit values for each 4x4 block, with 1
         * denoting a literal block */

        uint32_t s1 = header.s1;
        uint32_t s2 = header.s2;
        uint32_t s3 = header.s3;

        unsigned size = 0;

        for (unsigned i = 0; i < 30; i += 6)
                size += pan_afbc_data_size(s1 >> i, literal_size);

        size += pan_afbc_data_size((s1 >> 30) | (s2 << 2), literal_size);

        for (unsigned i = 4; i < 28; i += 6)
                size += pan_afbc_data_size(s2 >> i, literal_size);

        size += pan_afbc_data_size((s2 >> 28) | (s3 << 4), literal_size);

        for (unsigned i = 2; i < 32; i += 6)
                size += pan_afbc_data_size(s3 >> i, literal_size);

        /* Uncomment to disable compression.
         * TODO: Can we use something like an extern global variable
         * which will be const-folded for controlling this? */
        //size = literal_size * 16;

        return size;
}

/* TODO: Find a way to do type checking for arguments when calling from C */
kernel void
count(global blocksz_t *sizes, global uint *offsets,
      global uint4 *headers, uint literal_size,
      uint num_per_thread, uint num_threads, uint total_size)
{
   uint thread_id = get_global_id(0);

   uint start = thread_id * num_per_thread;
   uint end = min(start + num_per_thread, total_size);

   printf("thread: %i, num_per: %i, total: %i, %i - %i\n",
          thread_id, num_per_thread, total_size, start, end);

   uint sum = 0;

   for (uint i = start; i < end; ++i) {
      uint size = pan_afbc_decode_data_size(headers[i], literal_size);

      uint size_blocks = DIV_ROUND_UP(size, 64);

      printf("sizes[%i] = %i (%i blocks);\n", i, size, size_blocks);

      sizes[i] = size_blocks;
      sum += size_blocks;
   }

   printf("sum for thread %i: %i\n", thread_id, sum);
   for (uint i = thread_id; i < num_threads; ++i) {
      /* TODO: Atomics seem to be not completely reliable on Panfrost, is it
       * good enough for here?
       * Maybe we should set a flag from the _copy shader if something doesn't
       * add up, but by the time we get to check it, we might have already
       * freed the original BO. */
      atomic_add(offsets + i, sum);

      /* "New-style" atomics only avaiable in OpenCL 2.0 */
      /* atomic_fetch_add_explicit(offsets + i, sum, memory_order_relaxed); */
   }
}

/* All sizes are in 64-byte blocks */

kernel void
copy(global uint4 *dest, global uint4 *src,
     global blocksz_t *sizes, global uint *offsets,
     uint afbc_header_blocks,
     uint num_per_thread, uint num_threads, uint total_size)
{
   uint thread_id = get_global_id(0);

   uint start = thread_id * num_per_thread;
   uint end = min(start + num_per_thread, total_size);

   uint offset = afbc_header_blocks;
   if (thread_id)
      offset += offsets[thread_id - 1];

   for (uint i = start; i < end; ++i) {
      uint4 header = src[i];

      global uint4 *dest_mem = dest + offset * 4;
      global uint4 *src_mem = src + (header.s0 / 16);

      header.s0 = offset * 64;

      uint size = sizes[i];

      dest[i] = header;

      printf("copying %i blocks to %p from %p\n", size, dest_mem, src_mem);

      for (uint c = 0; c < size; ++c) {
         /* TODO: Reorder for better scheduling */
         dest_mem[c * 4 + 0] = src_mem[c * 4 + 0];
         dest_mem[c * 4 + 1] = src_mem[c * 4 + 1];
         dest_mem[c * 4 + 2] = src_mem[c * 4 + 2];
         dest_mem[c * 4 + 3] = src_mem[c * 4 + 3];
      }

      offset += size;
   }
}

/*!
[config]
name: Copy shader

build_options: -cl-std=CL1.2
dimensions: 1
global_size: 1 0 0

[test]
name: count basic
kernel_name: count
arg_out: 0 buffer uint[1] 1
arg_out: 1 buffer uint[1] 1
arg_in: 2 buffer uint[4] 0 7 0 0
arg_in: 3 uint 64
arg_in: 4 uint 1
arg_in: 5 uint 1
arg_in: 6 uint 1

[test]
name: copy basic
kernel_name: copy
arg_out: 0 buffer uint[32] 64 1 2 3 0 0 0 0 0 0 0 0 0 0 0 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16
arg_in: 1 buffer uint[48] 128 1 2 3 1 1 1 1 2 2 2 2 3 3 3 3 4 4 4 4 5 5 5 5 6 6 6 6 7 7 7 7 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16
arg_in: 2 buffer uint[1] 1
arg_in: 3 buffer uint[1] 123
arg_in: 4 uint 1
arg_in: 5 uint 1
arg_in: 6 uint 1
arg_in: 7 uint 1

!*/
/* TODO: more tests.. */
