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

static void dump_array(nodearray *a, const char *str) {
        puts(str);
        unsigned i = 0;
        util_dynarray_foreach(a, uint32_t, elem) {
                if (i % 4 == 0)
                        printf("%p:", elem);
                printf(" 0x%08x", *elem);
                if (++i % 4 == 0)
                        printf("\n");
        }
        printf("\n");
}

#define nodearray_sparse_foreach_vec(buf, elem, value) \
        for (uint8_t *elem = (uint8_t *)(buf)->data, \
                     *value = ((uint8_t *)(buf)->data) + (buf)->size / 5; \
             value < ((uint8_t *)(buf)->data + (buf)->size); \
             elem += 4, value += 16)

#define nodearray_sparse_foreach_val(buf, value) \
        for (uint8_t *value = ((uint8_t *)(buf)->data) + (buf)->size / 5; \
             value < ((uint8_t *)(buf)->data + (buf)->size); \
             value += 16)

struct nodearray_iter {
        nodearray *a;
        // TODO: Why even bother with these?
        uint32_t *elem;
        uint8_t *val;

        uint32_t key;
        uint8_t value;
};

static inline unsigned
nodearray_key(const uint32_t *elem)
{
        return *elem >> 8;
}

static inline bool
nodearray_iter_next(struct nodearray_iter *i)
{
        while ((++i->key) & 15) {
                uint8_t value = *(++i->val);
                if (value) {
                        i->value = value;
                        return true;
                }
        }

        ++i->elem;
        if ((uint8_t *)i->elem == (uint8_t *)(i->a)->data + (i->a)->size / 5)
                return false;

        i->key = nodearray_key(i->elem);

        for (;;) {
                uint8_t value = *(++i->val);
                if (value) {
                        i->value = value;
                        return true;
                }
                ++i->key;
        }
}

#define nodearray_sparse_foreach(buf, iter) \
        for (struct nodearray_iter iter = {.a = buf, .elem = (uint32_t *)(buf)->data - 1, .val = (uint8_t *)(buf)->data + (buf)->size / 5 - 1, .key = ~0U }; nodearray_iter_next(&iter);)

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

static inline uint8_t
nodearray_value(const uint32_t *elem)
{
        return *elem & 0xff;
}

static inline unsigned
nodearray_largest_value(const nodearray *a)
{
        if (!a->size)
                return 0;
        uint8_t *dat = (uint8_t *)a->data + a->size / 5;
        uint32_t *elem = (uint32_t *)dat - 1;
        return nodearray_key(elem) + 15;
}

static inline uint8_t *
nodearray_data_block_addr(const nodearray *a, unsigned block)
{
        uint8_t *dat = (uint8_t *)a->data + a->size / 5;
        return dat + block * 16;
}

struct nodearray_sparse_elem_fake {
        uint8_t data[20];
};

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
        assert(*value < (uint8_t *)a->data + a->size);

        return left;
}

//extern uint8_t *valptr;

static inline uint8_t
nodearray_get(const nodearray *a, unsigned key, unsigned max)
{
        if (nodearray_sparse(a, max)) {
                if (!a->size)
                        return 0;

                uint32_t *elem;
                uint8_t *value;
                nodearray_sparse_search(a, key, &elem, &value);

//                valptr = value;

//                if (key == 992)
//                        printf("key %p = %i\n", value, *value);

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
                                        ++*elem;
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

#ifdef TEST_NODEARRAY_ORR
                        nodearray copy;
                        util_dynarray_clone(&copy, NULL, a);
#endif

//                        unsigned osz = a->size;
                        ASSERTED void *grown = util_dynarray_grow(a, struct nodearray_sparse_elem_fake, 1);
                        assert(grown);

                        /* First move vector data (TODO: why move at all??),
                         * then "headers" */

//                        printf("before\n");
//                        dump_array(a);

                        uint8_t *far_elem = util_dynarray_element(a, uint8_t, a->size / 5 + left * 16);
                        if (left != size) {
//                                printf("memmove(%p, %p, 0x%x);\n", far_elem + 16, far_elem - 4, (size - left) * 16);
                                memmove(far_elem + 16, far_elem - 4, (size - left) * 16);
                        }
//                        printf("memset (%i %i) %p\n", a->size, left, far_elem);
                        memset(far_elem, 0, 16);

                        uint32_t *elem = util_dynarray_element(a, uint32_t, left);
                        if (size) {
//                                printf("memmove2(%p, %p, 0x%lx);\n", elem + 1, elem, (size - left) * sizeof(uint32_t) + left * 16);
                                memmove(elem + 1, elem, (size - left) * sizeof(uint32_t) + left * 16);
                        }

//                        printf("after\n");
//                        dump_array(a);

                        for (unsigned i = 0; i < 16; ++i)
                                assert(far_elem[i] == 0);

                        far_elem[key & 15] = value;
                        *elem = nodearray_encode(key & ~15, 1);

#ifdef TEST_NODEARRAY_ORR
                        unsigned ll = nodearray_largest_value(&copy);
                        for (unsigned i = 0; i < ll; ++i) {
                                if (i == key)
                                        continue;

                                uint8_t left = nodearray_get(a, i, max);
                                uint8_t right = nodearray_get(&copy, i, max);
                                if (left != right)
                                        printf("mismatch!! %p=%x %p=%x 0x%x/0x%x\n", a, left, &copy, right, i, ll);
                        }
                        util_dynarray_fini(&copy);
#endif

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
nodearray_orr_hint(nodearray *a, unsigned key, uint8_t value,
              unsigned max_sparse, unsigned max, unsigned *insert_hint)
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
                        elem = util_dynarray_element(a, uint32_t, *insert_hint);
                        unsigned diff = key - nodearray_key(elem);
                        if (diff < 16) {
                                vv = nodearray_data_block_addr(a, *insert_hint) + diff;
                                if (!*vv)
                                        ++*elem;
                                *vv |= value;
                                return;
                        }

                        left = nodearray_sparse_search(a, key, &elem, &vv);

                        *insert_hint = left;

                        diff = key - nodearray_key(elem);
                        if (diff < 16) {
                                // TOOD: Is this read bad for perf?
                                if (!*vv)
                                        ++*elem;
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

#ifdef TEST_NODEARRAY_ORR
                        nodearray copy;
                        util_dynarray_clone(&copy, NULL, a);
#endif

//                        unsigned osz = a->size;
                        ASSERTED void *grown = util_dynarray_grow(a, struct nodearray_sparse_elem_fake, 1);
                        assert(grown);

                        /* First move vector data (TODO: why move at all??),
                         * then "headers" */

//                        printf("before\n");
//                        dump_array(a);

                        uint8_t *far_elem = util_dynarray_element(a, uint8_t, a->size / 5 + left * 16);
                        if (left != size) {
//                                printf("memmove(%p, %p, 0x%x);\n", far_elem + 16, far_elem - 4, (size - left) * 16);
                                memmove(far_elem + 16, far_elem - 4, (size - left) * 16);
                        }
//                        printf("memset (%i %i) %p\n", a->size, left, far_elem);
                        memset(far_elem, 0, 16);

                        uint32_t *elem = util_dynarray_element(a, uint32_t, left);
                        if (size) {
//                                printf("memmove2(%p, %p, 0x%lx);\n", elem + 1, elem, (size - left) * sizeof(uint32_t) + left * 16);
                                memmove(elem + 1, elem, (size - left) * sizeof(uint32_t) + left * 16);
                        }

//                        printf("after\n");
//                        dump_array(a);

                        for (unsigned i = 0; i < 16; ++i)
                                assert(far_elem[i] == 0);

                        far_elem[key & 15] = value;
                        *elem = nodearray_encode(key & ~15, 1);

#ifdef TEST_NODEARRAY_ORR
                        unsigned ll = nodearray_largest_value(&copy);
                        for (unsigned i = 0; i < ll; ++i) {
                                if (i == key)
                                        continue;

                                uint8_t left = nodearray_get(a, i, max);
                                uint8_t right = nodearray_get(&copy, i, max);
                                if (left != right)
                                        printf("mismatch!! %p=%x %p=%x 0x%x/0x%x\n", a, left, &copy, right, i, ll);
                        }
                        util_dynarray_fini(&copy);
#endif

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

static inline uint8_t *
nodearray_orr_loc(nodearray *a, unsigned key, unsigned max_sparse, unsigned max, bool *nnew)
{
        assert(key < (1 << 24));
        assert(key < max);

        if (nodearray_sparse(a, max)) {
                // TODO: Why do this division?
                unsigned size = util_dynarray_num_elements(a, struct nodearray_sparse_elem_fake);

                unsigned left = 0;

                if (size) {
                        /* Do a binary search for key.. */
                        uint32_t *elem;
                        uint8_t *vv;
                        left = nodearray_sparse_search(a, key, &elem, &vv);

//                        printf("%i %i\n", key, nodearray_key(elem));
                        if (key == nodearray_key(elem)) {
                                *nnew = false;
                                return vv;
                        }

                        /* We insert before `left`, so increment it if it's
                         * out of order */
                        if (nodearray_key(elem) < key)
                                ++left;
                }

                if (size < max_sparse && (size + 1) * 20 < max) {
                        /* We didn't find it, but we know where to insert it. */

                        ASSERTED void *grown = util_dynarray_grow(a, struct nodearray_sparse_elem_fake, 1);
                        assert(grown);

                        uint8_t *far_elem = util_dynarray_element(a, uint8_t, a->size / 5 + left * 16);
                        if (left != size) {
                                memmove(far_elem + 16, far_elem - 4, (size - left) * 16);
                        }

                        uint32_t *elem = util_dynarray_element(a, uint32_t, left);
                        if (size) {
                                memmove(elem + 1, elem, (size - left) * sizeof(uint32_t) + left * 16);
                        }

                        // TODO: better value?
                        *elem = nodearray_encode(key & ~15, 1);
                        return far_elem;
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
        } else {
                *nnew = false;
        }

        return util_dynarray_element(a, uint8_t, key);
}

static inline void
nodearray_orr_array(nodearray *a, const nodearray *b, unsigned max_sparse,
                    unsigned max)
{
        assert(nodearray_sparse(b, max));

        nodearray_sparse_foreach_vec(b, elem, value) {
                unsigned base = nodearray_key((uint32_t *)elem);
                for (unsigned i = 0; i < 16; ++i)
                        nodearray_orr(a, base + i, value[i], max_sparse, max);
        }

#ifdef TEST_NODEARRAY_ORR_ARRAY
        unsigned ll = nodearray_largest_value(b);
        for (unsigned i = 0; i < ll; ++i) {
                uint8_t left = nodearray_get(a, i, max);
                uint8_t right = nodearray_get(b, i, max);
                if ((left ^ right) & right)
                        printf("mismatch!! %p=%x %p=%x 0x%x/0x%x\n", a, left, b, right, i, ll);
        }
#endif
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

                if (!*vv)
                        return;

                *vv &= ~value;
                if (*vv)
                        return;

                --*elem;
//                printf("--refcnt %i == %i\n", key & ~15, nodearray_value(elem));
                if (nodearray_value(elem))
                        return;

                /* Delete the element, todo: what if we want to add the vec
                 * right back? */

#ifdef TEST_NODEARRAY_BIC
                nodearray copy;
                util_dynarray_clone(&copy, NULL, a);
#endif

//                dump_array(a, "before");

//                printf("memmove2(%p, %p, 0x%lx);\n", elem, elem + 1, (size - loc - 1) * sizeof(uint32_t) + loc * 16);
                memmove(elem, elem + 1, (size - loc - 1) * sizeof(uint32_t) + loc * 16);

                vv -= diff;
//                printf("memmove1(%p, %p, 0x%x);\n", vv, vv + 16, (size - loc - 1) * 16);
                memmove(vv - 4, vv + 16, (size - loc - 1) * 16);

                // can this do an oob read?

//                dump_array(a, "after");

                (void)util_dynarray_pop(a, struct nodearray_sparse_elem_fake);

#ifdef TEST_NODEARRAY_BIC
                unsigned ll = nodearray_largest_value(&copy);
                for (unsigned i = 0; i < ll; ++i) {
                        if (i == key)
                                continue;

                        uint8_t left = nodearray_get(a, i, max);
                        uint8_t right = nodearray_get(&copy, i, max);
                        if (left != right)
                                printf("mismatch!! %p=%x %p=%x 0x%x/0x%x\n", a, left, &copy, right, i, ll);
                }
                util_dynarray_fini(&copy);
#endif
        } else {
                *util_dynarray_element(a, uint8_t, key) &= ~value;
        }
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif
