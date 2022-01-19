# Copyright © 2018 Rob Clark
# Copyright © 2019 Collabora

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

inc_panfrost_hw = include_directories([
   'include'
])

inc_panfrost = include_directories([
   '.', 'include', 'shared', 'midgard', 'bifrost', 'lib'
])

subdir('shared')
subdir('util')
subdir('midgard')
subdir('bifrost')

if with_gallium_panfrost or with_panfrost_vk
   subdir('lib')
   subdir('perf')
endif

files_bifrost = files(
  'bifrost/cmdline.c',
)

bifrost_compiler = executable(
  'bifrost_compiler',
  [files_bifrost],
  include_directories : [
    inc_mapi,
    inc_mesa,
    inc_gallium,
    inc_gallium_aux,
    inc_include,
    inc_src,
    inc_panfrost,
    inc_panfrost_hw,
 ],
  dependencies : [
    idep_nir,
    idep_mesautil,
    idep_bi_opcodes_h,
    dep_libdrm,
  ],
  link_with : [
    libglsl_standalone,
    libpanfrost_bifrost,
  ],
  build_by_default : with_tools.contains('panfrost')
)

# TODO: Automatically run this at build time if available. Add a Python script
# so that we can use the files checked into git if there are no source
# changes.
# TODO: This doesn't use anything Panfrost-specific, could it be somewhere else?
if with_clc
  prog_pan_clc = executable(
    'pan_clc',
    ['util/pan_clc.c'],
    include_directories : [inc_include, inc_src, inc_compiler, inc_panfrost, inc_gallium_aux],
    c_args : [pre_args, no_override_init_args],
    dependencies : [idep_nir, idep_clc, dep_libdrm],
    native : true,
    build_by_default : with_tools.contains('panfrost')
  )
endif

if with_panfrost_vk
  subdir('vulkan')
endif

if with_perfetto and (with_datasources.contains('panfrost') or with_datasources.contains('auto'))
  subdir('ds')
endif

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif
