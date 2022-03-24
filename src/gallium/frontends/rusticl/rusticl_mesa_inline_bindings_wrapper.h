#define nir_instr_as_intrinsic __nir_instr_as_intrinsic_wraped
#define nir_load_var __nir_load_var
#define pipe_resource_reference __pipe_resource_reference_wraped
#define util_format_pack_rgba __util_format_pack_rgba
#include "util/u_inlines.h"
#include "nir.h"
#include "nir_builder.h"
#include "util/format/u_format.h"
#undef nir_instr_as_intrinsic
#undef nir_load_var
#undef pipe_resource_reference
#undef util_format_pack_rgba

void pipe_resource_reference(struct pipe_resource **dst, struct pipe_resource *src);
nir_intrinsic_instr *nir_instr_as_intrinsic(const nir_instr *instr);
nir_ssa_def *nir_load_var(nir_builder *, nir_variable *);
void util_format_pack_rgba(enum pipe_format format, void *dst, const void *src, unsigned w);
