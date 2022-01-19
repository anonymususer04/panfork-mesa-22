/*
 * Copyright (C) 2022 Icecream95
 * Copyright © 2021 Intel Corporation
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

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include "util/u_dynarray.h"
#include "compiler/clc/clc.h"
#include "compiler/nir/nir_serialize.h"

#include "pan_kernel.h"

#include "panfrost/bifrost/bifrost_compile.h"

static void
msg_callback(void *priv, const char *msg)
{
        (void)priv;
        fprintf(stderr, "%s", msg);
}

static void
print_u32_data(FILE *fp, const char *prefix, const char *arr_name,
               const uint32_t *data, size_t len)
{
        assert(len % 4 == 0);
        fprintf(fp, "static const uint32_t %s_%s[] = {", prefix, arr_name);
        for (unsigned i = 0; i < (len / 4); i++) {
                if (i % 4 == 0)
                        fprintf(fp,"\n        ");

                fprintf(fp, " 0x%08" PRIx32 ",", data[i]);
        }
        fprintf(fp, "\n};\n");
}

static void
print_kernel(FILE *fp, const char *prefix, const char *entry)
{
        fprintf(fp, "static const struct pan_kernel_template pan_kernel_%s_%s = {\n", prefix, entry);
        fprintf(fp, "        .name = \"%s\",\n", prefix);
        fprintf(fp, "        .entrypoint = \"%s\",\n", entry);
        fprintf(fp, "        .spirv = %s_spirv,\n", prefix);
        fprintf(fp, "        .spirv_size = sizeof(%s_spirv),\n", prefix);
        fprintf(fp, "};\n");
}

int main(int argc, char *argv[])
{
        char *outfile = NULL, *spv_outfile = NULL, *name = NULL;

        static struct option long_options[] = {
                {"help", no_argument, 0, 'h'},
                {"out", required_argument, 0, 'o' },
                {"spv", required_argument, 0, 's' },
                {"name", required_argument, 0, 'n' },
                {0, 0, 0, 0},
        };

        int ch;
        while ((ch = getopt_long(argc, argv, "ho:s:n:e:", long_options, NULL)) != -1) {
                switch (ch) {
                case 'h':
                        printf("TODO: help\n");
                        return 0;
                case 'o':
                        outfile = optarg;
                        break;
                case 's':
                        spv_outfile = optarg;
                        break;
                case 'n':
                        name = optarg;
                        break;
                default:
                        fprintf(stderr, "Unrecognised option \"%s\".\n", optarg);
                        return 1;
                }
        }

        // TODO: Check arguments

        // TODO: Properly initialise
        struct util_dynarray clang_args = {0};
        struct util_dynarray input_files = {0};
        struct util_dynarray spirv_objs = {0};
        struct util_dynarray spirv_ptr_objs = {0};

        for (int i = optind; i < argc; ++i) {
                if (argv[i][0] == '-')
                        util_dynarray_append(&clang_args, char *, argv[i]);
                else
                        util_dynarray_append(&input_files, char *, argv[i]);
        }

        if (util_dynarray_num_elements(&input_files, char *) == 0) {
                fprintf(stderr, "No input file(s).\n");
                return -1;
        }

        struct clc_logger logger = {
                .error = msg_callback,
                .warning = msg_callback,
        };

        util_dynarray_foreach(&input_files, char *, infile) {
                int fd = open(*infile, O_RDONLY);
                if (fd < 0) {
                        fprintf(stderr, "Failed to open %s\n", *infile);
                        return 1;
                }

                off_t len = lseek(fd, 0, SEEK_END);
                const void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (map == MAP_FAILED) {
                        fprintf(stderr, "Failed to mmap the file: errno=%d, %s\n",
                                errno, strerror(errno));
                        return 1;
                }

                struct clc_compile_args clc_args = {
                        .source = {
                                .name = *infile,
                                .value = map,
                        },
                        .args = util_dynarray_begin(&clang_args),
                        .num_args = util_dynarray_num_elements(&clang_args, char *),
                };

                struct clc_binary *spirv_out =
                        util_dynarray_grow(&spirv_objs, struct clc_binary, 1);

                if (!clc_compile_c_to_spirv(&clc_args, &logger, spirv_out))
                        return 1;

                util_dynarray_append(&spirv_ptr_objs, struct clc_binary *, spirv_out);
        }

        struct clc_linker_args link_args = {
                .in_objs = util_dynarray_begin(&spirv_ptr_objs),
                .num_in_objs = util_dynarray_num_elements(&spirv_ptr_objs,
                                                          struct clc_binary *),
                .create_library = false,
        };
        struct clc_binary final_spirv;
        if (!clc_link_spirv(&link_args, &logger, &final_spirv))
                return 1;

        if (spv_outfile) {
                FILE *fp = fopen(spv_outfile, "w");
                fwrite(final_spirv.data, final_spirv.size, 1, fp);
                fclose(fp);
        }

        if (!outfile)
                return 0;

        struct clc_parsed_spirv parsed_spirv_data;
        if (!clc_parse_spirv(&final_spirv, &logger, &parsed_spirv_data))
                return 1;

        FILE *fp = fopen(outfile, "w");

        fprintf(fp, "#ifndef PAN_KERNEL_HEADER_%s\n", name);
        fprintf(fp, "#define PAN_KERNEL_HEADER_%s\n", name);

        fprintf(fp, "#include \"pan_kernel.h\"\n");
        fprintf(fp, "\n");

        // TODO: include sha1sum of source?

        print_u32_data(fp, name, "spirv", final_spirv.data, final_spirv.size);

        for (unsigned i = 0; i < parsed_spirv_data.num_kernels; i++)
                print_kernel(fp, name, parsed_spirv_data.kernels[i].name);

        fprintf(fp, "#endif\n");
        fclose(fp);
}
