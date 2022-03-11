#include "rusticl_mesa_inline_bindings_wrapper.h"

void
pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src)
{
   __pipe_resource_reference_wraped(dst, src);
}

nir_intrinsic_instr *
nir_instr_as_intrinsic(const nir_instr *instr)
{
    return __nir_instr_as_intrinsic_wraped(instr);
}

nir_ssa_def *
nir_load_var(nir_builder *build, nir_variable *var)
{
    return __nir_load_var(build, var);
}
