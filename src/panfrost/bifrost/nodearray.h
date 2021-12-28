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
 */

/* A nodearray is an array type that is either sparse or dense, depending on
 * the number of elements.
 *
 * When the number of elements is over a threshold (max_sparse), the dense
 * mode is used, and the nodearray is simply a dynarray with an 8-bit element
 * per node.
 *
 * In sparse mode, the dynarray has 32-bit elements, with a 24-bit node index
 * and an 8-bit value. The nodes are always sorted, so that a binary search
 * can be used to find elements. Nonexistent elements are treated as zero.
 *
 * Function names follow ARM instruction names: orr does *elem |= value, bic
 * does *elem &= ~value.
 *
 * Although it's probably already fast enough, the datastructure could be sped
 * up quite a bit, especially when NEON is available, by making the sparse
 * mode store sixteen adjacent values, so that adding new keys also allocates
 * nearby keys, and to allow for vectorising iteration, as can be done when in
 * the dense mode.
 */

#ifndef __BIFROST_NODEARRAY_H
#define __BIFROST_NODEARRAY_H

#include <stdint.h>

#include "util/u_dynarray.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct util_dynarray nodearray;

static inline bool
nodearray_sparse(const nodearray *a, unsigned max)
{
        return a->size < max;
}

static inline uint32_t
nodearray_encode(unsigned key, uint8_t value)
{
        return (key << 8) | value;
}

static inline unsigned
nodearray_key(const uint32_t *elem)
{
        return *elem >> 8;
}

static inline uint8_t
nodearray_value(const uint32_t *elem)
{
        return *elem & 0xff;
}

static inline unsigned
nodearray_sparse_search(const nodearray *a, uint32_t key, uint32_t **elem)
{
        uint32_t *data = (uint32_t *)util_dynarray_begin(a);
        unsigned size = util_dynarray_num_elements(a, uint32_t);

        uint32_t skey = nodearray_encode(key, 0xff);

        unsigned left = 0;
        unsigned right = size - 1;
        while (left != right) {
                unsigned probe = (left + right + 1) / 2;

                if (data[probe] > skey)
                        right = probe - 1;
                else
                        left = probe;
        }

        *elem = data + left;
        return left;
}

static inline uint8_t
nodearray_get(const nodearray *a, unsigned key, unsigned max)
{
        if (nodearray_sparse(a, max)) {
                if (!a->size)
                        return 0;

                uint32_t *elem;
                nodearray_sparse_search(a, key, &elem);

                if (nodearray_key(elem) == key)
                        return nodearray_value(elem);
                else
                        return 0;
        } else {
                return *util_dynarray_element(a, uint8_t, key);
        }
}

static inline void
nodearray_orr(nodearray *a, unsigned key, uint8_t value,
              unsigned max_sparse, unsigned max)
{
        assert(key < (1 << 24));
        assert(key < max);

        if (!value)
                return;

        if (nodearray_sparse(a, max)) {
                unsigned size = util_dynarray_num_elements(a, uint32_t);

                unsigned left = 0;

                if (size) {
                        /* Do a binary search for key.. */
                        uint32_t *elem;
                        left = nodearray_sparse_search(a, key, &elem);

                        if (nodearray_key(elem) == key) {
                                *elem |= value;
                                return;
                        }

                        /* We insert before `left`, so increment it if it's
                         * out of order */
                        if (nodearray_key(elem) < key)
                                ++left;
                }

                if (size < max_sparse && (size + 1) * 4 < max) {
                        /* We didn't find it, but we know where to insert it. */

                        ASSERTED void *grown = util_dynarray_grow(a, uint32_t, 1);
                        assert(grown);

                        uint32_t *elem = util_dynarray_element(a, uint32_t, left);
                        if (left != size)
                                memmove(elem + 1, elem, (size - left) * sizeof(uint32_t));

                        *elem = nodearray_encode(key, value);

                        return;
                }

                /* There are too many elements, so convert to a dense array */
                nodearray old = *a;
                util_dynarray_init(a, old.mem_ctx);

                /* Align to 16 bytes to allow SIMD operations */
                unsigned dyn_size = ALIGN_POT(max, 16);

                uint8_t *elements = (uint8_t *)util_dynarray_resize(a, uint8_t, dyn_size);
                assert(elements);
                memset(elements, 0, dyn_size);

                util_dynarray_foreach(&old, uint32_t, x) {
                        unsigned key = nodearray_key(x);
                        uint8_t value = nodearray_value(x);

                        assert(key < max);
                        elements[key] = value;
                }

                util_dynarray_fini(&old);
        }

        *util_dynarray_element(a, uint8_t, key) |= value;
}

static inline void
nodearray_orr_array(nodearray *a, const nodearray *b, unsigned max_sparse,
                    unsigned max)
{
        assert(nodearray_sparse(b, max));

        util_dynarray_foreach(b, uint32_t, elem)
                nodearray_orr(a, nodearray_key(elem), nodearray_value(elem),
                              max_sparse, max);
}

static inline void
nodearray_bic(nodearray *a, unsigned key, uint8_t value, unsigned max)
{
        if (!value)
                return;

        if (nodearray_sparse(a, max)) {
                unsigned size = util_dynarray_num_elements(a, uint32_t);
                if (!size)
                        return;

                uint32_t *elem;
                unsigned loc = nodearray_sparse_search(a, key, &elem);

                if (nodearray_key(elem) != key)
                        return;

                *elem &= ~value;

                if (nodearray_value(elem))
                        return;

                /* Delete the element */
                memmove(elem, elem + 1, (size - loc - 1) * sizeof(uint32_t));
                (void)util_dynarray_pop(a, uint32_t);
        } else {
                *util_dynarray_element(a, uint8_t, key) &= ~value;
        }
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif
