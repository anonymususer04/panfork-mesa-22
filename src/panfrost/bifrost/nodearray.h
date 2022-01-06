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

#define nodearray_sparse_foreach_vec(buf, elem, value) \
        for (uint8_t *elem = (uint8_t *)(buf)->data, \
                     *value = ((uint8_t *)(buf)->data) + (buf)->size / 5; \
             value < ((uint8_t *)(buf)->data + (buf)->size); \
             elem += 4, value += 16)

#define nodearray_sparse_foreach_val(buf, value) \
        for (uint8_t *value = ((uint8_t *)(buf)->data) + (buf)->size / 5; \
             value < ((uint8_t *)(buf)->data + (buf)->size); \
             value += 16)

#define nodearray_sparse_foreach(buf, elem, value) \
        for (uint32_t *elem = (uint32_t *)(buf)->data; \
             elem < (uint32_t *)((uint8_t *)(buf)->data + (buf)->size / 5); \
             ++elem) \
                for (uint8_t *value = ((uint8_t *)(buf)->data) + (buf)->size / 5 + ((uint8_t *)elem - (uint8_t *)(buf)->data) * 4; \
                     value < ((uint8_t *)(buf)->data) + (buf)->size / 5 + ((uint8_t *)elem - (uint8_t *)(buf)->data) * 4 + 16; ++value)

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


struct nodearray_sparse_elem_fake {
        uint8_t data[20];
};

#include "arm_neon.h"

static inline unsigned
nodearray_sparse_search(const nodearray *a, uint32_t key, uint32_t **elem, uint8_t **value)
{
        uint32_t *data = (uint32_t *)util_dynarray_begin(a);
        unsigned size = util_dynarray_num_elements(a, struct nodearray_sparse_elem_fake);

        uint32_t skey = nodearray_encode(key, 0xff);

        unsigned left = 0;
        unsigned right = size - 1;

        if (data[right] <= skey)
                left = right;

        while (left != right) {
                unsigned probe = (left + right + 1) / 2;

                if (data[probe] > skey)
                        right = probe - 1;
                else
                        left = probe;
        }

        *elem = data + left;
        // todo; unaligned elem?
        *value = (uint8_t *)(data + size) + left * 16 + (key & 15);
        return left;
}

static inline uint8_t
nodearray_get(const nodearray *a, unsigned key, unsigned max)
{
        if (nodearray_sparse(a, max)) {
                if (!a->size)
                        return 0;

                uint32_t *elem;
                uint8_t *value;
                nodearray_sparse_search(a, key, &elem, &value);

                unsigned diff = key - nodearray_key(elem);
                if (diff < 16)
                        return *value;
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
                // TODO: Why do this division?
                unsigned size = util_dynarray_num_elements(a, struct nodearray_sparse_elem_fake);

                unsigned left = 0;

                if (size) {
                        /* Do a binary search for key.. */
                        uint32_t *elem;
                        uint8_t *vv;
                        left = nodearray_sparse_search(a, key, &elem, &vv);

                        unsigned diff = key - nodearray_key(elem);
                        if (diff < 16) {
                                // TOOD: Is this read bad for perf?
                                if (!*vv)
                                        ++elem;
                                *vv |= value;
                                return;
                        }

                        /* We insert before `left`, so increment it if it's
                         * out of order */
                        if (nodearray_key(elem) < key)
                                ++left;
                }

                if (size < max_sparse && (size + 1) * 20 < max) {
                        /* We didn't find it, but we know where to insert it. */

                        unsigned osz = a->size;
                        ASSERTED void *grown = util_dynarray_grow(a, struct nodearray_sparse_elem_fake, 1);
                        assert(grown);

                        /* First move vector data (TODO: why move at all??),
                         * then "headers" */

                        uint8_t *far_elem = util_dynarray_element(a, uint8_t, a->size / 5 + left * 16);
                        if (left != size) {
                                memmove(far_elem + 20, far_elem, (size - left) * 16);
                        }
                        printf("memset (%i %i) %p\n", a->size, left, far_elem);
                        memset(far_elem, 0, 16);

                        uint32_t *elem = util_dynarray_element(a, uint32_t, left);
                        if (size)
                                memmove(elem + 1, elem, (size - left) * sizeof(uint32_t) + left * 16);

                        far_elem[key & 15] = value;
                        *elem = nodearray_encode(key & ~15, 1);

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

                nodearray_sparse_foreach_vec(&old, x, vv) {
                        unsigned key = nodearray_key((uint32_t *)x);

                        assert(key < max);
                        memcpy(elements + key, vv, 16);
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
                unsigned size = util_dynarray_num_elements(a, struct nodearray_sparse_elem_fake);
                if (!size)
                        return;

                uint32_t *elem;
                uint8_t *vv;
                unsigned loc = nodearray_sparse_search(a, key, &elem, &vv);

                unsigned diff = key - nodearray_key(elem);
                if (diff >= 16)
                        return;

                *vv &= ~value;
                if (*vv)
                        return;

                --*elem;
                if (nodearray_value(elem))
                        return;

                /* Delete the element, todo: what if we want to add the vec
                 * right back? */

                vv -= diff;
                printf("memmove %p : %i <%i\n", vv, (size - loc - 1) * 16, diff);
                memmove(vv, vv + 16, (size - loc - 1) * 16);

                // can this do an oob read?
                memmove(elem, elem + 1, (size - loc - 1) * sizeof(uint32_t) + loc * 16);
                (void)util_dynarray_pop(a, struct nodearray_sparse_elem_fake);
        } else {
                *util_dynarray_element(a, uint8_t, key) &= ~value;
        }
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif
