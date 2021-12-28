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
 * mode is used, and the nodearray is simply a container for an array with an
 * 8-bit element per node.
 *
 * In sparse mode, the array has 32-bit elements, with a 24-bit node index
 * and an 8-bit value. The nodes are always sorted, so that a binary search
 * can be used to find elements. Nonexistent elements are treated as zero.
 *
 * Function names follow ARM instruction names: orr does *elem |= value, bic
 * does *elem &= ~value.
 *
 * Although it's probably already fast enough, the datastructure could be sped
 * up a lot, especially when NEON is available, by making the sparse mode
 * store sixteen adjacent values, so that adding new keys also allocates
 * nearby keys, and to allow for vectorising iteration, as can be done when in
 * the dense mode.
 */

#ifndef __BIFROST_NODEARRAY_H
#define __BIFROST_NODEARRAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
// Defined in compiler.h
typedef struct {
        union {
                uint32_t *sparse;
                uint8_t *dense;
        }
        unsigned size; // either 8-bit or 32-bit elements
        unsigned sparse_capacity;
} nodearray;
*/

#define nodearray_sparse_foreach(buf, elem) \
   for (uint32_t *elem = (buf)->sparse; \
        elem < (buf)->sparse + (buf)->size; elem++)

#define nodearray_dense_foreach(buf, elem) \
   for (uint8_t *elem = (buf)->dense; \
        elem < (buf)->dense + (buf)->size; elem++)

#define nodearray_dense_foreach_64(buf, elem) \
   for (uint64_t *elem = (uint64_t *)(buf)->dense; \
        (uint8_t *)elem < (buf)->dense + (buf)->size; elem++)

static inline bool
nodearray_sparse(const nodearray *a)
{
        return a->sparse_capacity != ~0;
}

static inline void
nodearray_clone(nodearray *dest, const nodearray *src)
{
        dest->size = src->size;
        dest->sparse_capacity = src->sparse_capacity;
        if (nodearray_sparse(src)) {
                dest->sparse = malloc(src->sparse_capacity * sizeof(uint32_t));
                memcpy(dest->sparse, src->sparse, src->size * sizeof(uint32_t));
        } else {
                unsigned aligned = ALIGN_POT(src->size, 16);
                dest->dense = malloc(aligned * sizeof(uint8_t));
                memcpy(dest->dense, src->dense, aligned * sizeof(uint8_t));
        }
}

static inline void
nodearray_init(nodearray *a)
{
        *a = (nodearray) {0};
}

static inline void
nodearray_reset(nodearray *a)
{
        free(a->sparse);
        nodearray_init(a);
}

/* Arrays with equivalent elements but different sparseness are considered
 * different */
static inline bool
nodearray_equal(const nodearray *a, const nodearray *b)
{
        if (a->size != b->size)
                return false;

        if (nodearray_sparse(a) != nodearray_sparse(b))
                return false;

        if (nodearray_sparse(a))
                return !memcmp(a->sparse, b->sparse, a->size * sizeof(uint32_t));
        else
                return !memcmp(a->dense, b->dense, a->size * sizeof(uint8_t));
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
        uint32_t *data = a->sparse;

        uint32_t skey = nodearray_encode(key, 0xff);

        unsigned left = 0;
        unsigned right = a->size - 1;

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
        return left;
}

static inline uint8_t
nodearray_get(const nodearray *a, unsigned key)
{
        if (nodearray_sparse(a)) {
                if (!a->size)
                        return 0;

                uint32_t *elem;
                nodearray_sparse_search(a, key, &elem);

                if (nodearray_key(elem) == key)
                        return nodearray_value(elem);
                else
                        return 0;
        } else {
                assert(key < a->size);
                return a->dense[key];
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

        if (nodearray_sparse(a)) {
                unsigned size = a->size;

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

                        uint32_t *data = a->sparse;
                        uint32_t *data_move = data + left;

                        uint32_t *elem = NULL;

                        bool realloc = (++a->size) > a->sparse_capacity;

                        if (realloc) {
                                a->sparse_capacity = CLAMP(a->sparse_capacity * 2, 64, max / 4);

                                /* This case shouldn't happen, but handle it just in case */
                                if (a->size > a->sparse_capacity)
                                        ++a->sparse_capacity;

                                a->sparse = malloc(a->sparse_capacity * sizeof(uint32_t));
                                elem = a->sparse + left;

                                if (left)
                                        memcpy(a->sparse, data, left * sizeof(uint32_t));

                                if (left != size)
                                        memcpy(elem + 1, data_move, (size - left) * sizeof(uint32_t));
                        } else {
                                elem = data_move;
                                if (left != size)
                                        memmove(elem + 1, elem, (size - left) * sizeof(uint32_t));
                        }

                        *elem = nodearray_encode(key, value);

                        if (realloc)
                                free(data);

                        return;
                }

                /* There are too many elements, so convert to a dense array */
                nodearray old = *a;

                /* Align to 16 bytes to allow SIMD operations */
                a->dense = calloc(ALIGN_POT(max, 16), sizeof(uint8_t));
                a->size = max;
                a->sparse_capacity = ~0;

                uint8_t *data = a->dense;

                nodearray_sparse_foreach(&old, x) {
                        unsigned key = nodearray_key(x);
                        uint8_t value = nodearray_value(x);

                        assert(key < max);
                        data[key] = value;
                }

                free(old.sparse);
        }

        a->dense[key] |= value;
}

static inline void
nodearray_orr_array(nodearray *a, const nodearray *b, unsigned max_sparse,
                    unsigned max)
{
        assert(nodearray_sparse(b));

        nodearray_sparse_foreach(b, elem)
                nodearray_orr(a, nodearray_key(elem), nodearray_value(elem),
                              max_sparse, max);
}

static inline void
nodearray_bic(nodearray *a, unsigned key, uint8_t value)
{
        if (!value)
                return;

        if (nodearray_sparse(a)) {
                unsigned size = a->size;
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
                --a->size;
        } else {
                a->dense[key] &= ~value;
        }
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif
