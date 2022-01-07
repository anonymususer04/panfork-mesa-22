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
 */

#include "compiler.h"
#include "nodearray.h"

#include <gtest/gtest.h>

class NodeArray : public testing::Test {
protected:
   NodeArray() {
      a = new nodearray;
      util_dynarray_init(a, NULL);
   }

   ~NodeArray() {
      util_dynarray_fini(a);
      delete a;
   }

   nodearray *a;
};

TEST_F(NodeArray, NodeArrayORR)
{
   unsigned seed = 1;

   unsigned max_sparse = ~0, max = ~0;

   uint8_t *map = (uint8_t *)calloc(1, 1001 * 16);

   for (uint8_t i = 1; i < 100; ++i) {
      unsigned key;
      do {
         key = rand_r(&seed) % 1000;
      } while (nodearray_get(a, key, max));

      EXPECT_EQ(map[key], 0);
      map[key] = i;
      nodearray_orr(a, key, i, max_sparse, max);

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }
}

TEST_F(NodeArray, NodeArrayForEach)
{
   unsigned seed = 1;

   unsigned max_sparse = ~0, max = ~0;

   uint8_t *map = (uint8_t *)calloc(1, 1001 * 16);

   for (uint8_t i = 1; i < 100; ++i) {
      unsigned key;
      do {
         key = rand_r(&seed) % 1000;
      } while (nodearray_get(a, key, max));

      EXPECT_EQ(map[key], 0);
      map[key] = i;
      nodearray_orr(a, key, i, max_sparse, max);

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }

   unsigned count = 0;
   nodearray_sparse_foreach(a, it) {
      ASSERT_NE(it.value, 0);
      ASSERT_EQ(map[it.key], it.value);
      ++count;
   }
   ASSERT_EQ(count, 99);
}

TEST_F(NodeArray, NodeArrayForEachVec)
{
   unsigned seed = 1;

   unsigned max_sparse = ~0, max = ~0;

   uint8_t *map = (uint8_t *)calloc(1, 1001 * 16);

   for (uint8_t i = 1; i < 100; ++i) {
      unsigned key;
      do {
         key = rand_r(&seed) % 1000;
      } while (nodearray_get(a, key, max));

      EXPECT_EQ(map[key], 0);
      map[key] = i;
      nodearray_orr(a, key, i, max_sparse, max);

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }

   unsigned count = 0;
   nodearray_sparse_foreach_vec(a, elem, value) {
      unsigned base = nodearray_key((uint32_t *)elem);
      for (unsigned i = 0; i < 16; ++i) {
         ASSERT_EQ(map[base + i], value[i]);
         if (value[i])
            ++count;
      }
   }
   ASSERT_EQ(count, 99);
}

TEST_F(NodeArray, NodeArrayORRArray)
{
   unsigned seed = 1;

   unsigned max_sparse = ~0, max = ~0;

   uint8_t *map = (uint8_t *)calloc(1, 1001 * 16);

   for (uint8_t i = 1; i < 100; ++i) {
      unsigned key;
      do {
         key = rand_r(&seed) % 1000;
      } while (nodearray_get(a, key, max));

      EXPECT_EQ(map[key], 0);
      map[key] = i;
      nodearray_orr(a, key, i, max_sparse, max);

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }

   nodearray x;
   util_dynarray_init(&x, NULL);
   nodearray_orr_array(&x, a, ~0, ~0);
   for (unsigned n = 0; n < 1000 * 16; ++n) {
      unsigned r = map[n];

      ASSERT_EQ(nodearray_get(&x, n, max), r);
   }
   util_dynarray_fini(&x);
}

#define TEST_SIZE 100

TEST_F(NodeArray, NodeArrayORRThenBIC)
{
   unsigned seed = 1;

   unsigned max_sparse = ~0, max = ~0;

   uint8_t *map = (uint8_t *)calloc(1, 1001 * 16);

   unsigned bic_list[TEST_SIZE];
   for (uint8_t i = 1; i < TEST_SIZE; ++i) {
      unsigned key;
      do {
         key = rand_r(&seed) % 1000;
      } while (nodearray_get(a, key, max));

      EXPECT_EQ(map[key], 0);
      map[key] = i;
      bic_list[i] = key;
      nodearray_orr(a, key, i, max_sparse, max);

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }

   for (uint8_t i = 1; i < TEST_SIZE; ++i) {
      unsigned key = bic_list[i];

      nodearray_bic(a, key, map[key], max);
      map[key] = 0;

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }
}

TEST_F(NodeArray, NodeArrayORRThenBICRandom)
{
   unsigned seed = 1;

   unsigned max_sparse = ~0, max = ~0;

   uint8_t *map = (uint8_t *)calloc(1, 1001 * 16);

   unsigned bic_list[TEST_SIZE];
   for (uint8_t i = 1; i < TEST_SIZE; ++i) {
      unsigned key;
      do {
         key = rand_r(&seed) % 1000;
      } while (nodearray_get(a, key, max));

      EXPECT_EQ(map[key], 0);
      map[key] = i;
      bic_list[i] = key;
      nodearray_orr(a, key, i, max_sparse, max);

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }

   for (uint8_t i = 1; i < TEST_SIZE; ++i) {
      unsigned take = rand_r(&seed) % (TEST_SIZE - (unsigned)i) + 1;
      unsigned key = bic_list[take];
      memmove(bic_list + take, bic_list + take + 1, TEST_SIZE - take - 1);

      nodearray_bic(a, key, map[key], max);
      map[key] = 0;

      for (unsigned n = 0; n < 1000 * 16; ++n) {
         unsigned r = map[n];

         ASSERT_EQ(nodearray_get(a, n, max), r);
      }
   }
}

TEST_F(NodeArray, NodeArrayRandom)
{
   unsigned seed = 1;

   unsigned max = 100 * 16;

   uint8_t *dense = (uint8_t *)calloc(1, max);

   for (unsigned i = 0; i < 100000; ++i) {
      unsigned key = rand_r(&seed) % max;
      uint8_t value = rand_r(&seed) & 0xff;
      int op = rand_r(&seed) % 10;

      if (op == 0) {
         printf("nodearray_orr(%x, %x);\n", key, value);
         nodearray_orr(a, key, value, ~0, ~0);
         dense[key] |= value;
      } else if (op == 1) {
         printf("nodearray_bic(%x, %x);\n", key, value);
         nodearray_bic(a, key, value, ~0);
         dense[key] &= ~value;
      } else {
         for (unsigned j = 0; j < 100; ++j) {
            if (dense[key])
               break;
            key = rand_r(&seed) % max;
         }
         value = 0xff;
         printf("Nodearray_bic(%x, %x);\n", key, value);
         nodearray_bic(a, key, value, ~0);
         dense[key] &= ~value;
      }

      for (unsigned n = 0; n < max; ++n) {
         ASSERT_EQ(nodearray_get(a, n, ~0), dense[n]);
      }
   }

   for (;;) {
      bool empty = true;
      for (unsigned i = 0; i < max; ++i) {
         if (dense[i]) {
            empty = false;
            break;
         }
      }
      if (empty)
         break;

      for (unsigned i = 0; i < 1000; ++i) {
         unsigned key = rand_r(&seed) % max;
         uint8_t value = dense[key];

         if (!value)
            continue;

         printf("nodearray_kill(%x, %x);\n", key, value);
         nodearray_bic(a, key, value, ~0);
         dense[key] &= ~value;

         for (unsigned n = 0; n < max; ++n) {
            ASSERT_EQ(nodearray_get(a, n, ~0), dense[n]);
         }
      }
   }
}
