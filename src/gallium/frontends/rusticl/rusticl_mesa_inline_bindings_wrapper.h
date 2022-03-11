#define nir_instr_as_intrinsic __nir_instr_as_intrinsic_wraped
#define nir_load_var __nir_load_var
#define pipe_resource_reference __pipe_resource_reference_wraped
#include "util/u_inlines.h"
#include "nir.h"
#include "nir_builder.h"
#undef nir_instr_as_intrinsic
#undef nir_load_var
#undef pipe_resource_reference

void pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src);
nir_intrinsic_instr *nir_instr_as_intrinsic(const nir_instr *instr);
nir_ssa_def *nir_load_var(nir_builder *, nir_variable *);
