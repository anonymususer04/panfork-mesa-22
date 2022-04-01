extern crate mesa_rust;
extern crate mesa_rust_gen;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::cl_prop;
use crate::core::device::*;
use crate::core::event::*;
use crate::core::memory::*;
use crate::core::program::*;
use crate::core::queue::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::compiler::clc::*;
use self::mesa_rust::compiler::nir::*;
use self::mesa_rust_gen::*;
use self::rusticl_opencl_gen::*;

use std::cell::RefCell;
use std::cmp;
use std::collections::HashMap;
use std::collections::HashSet;
use std::convert::TryInto;
use std::ffi::CStr;
use std::os::raw::c_void;
use std::ptr;
use std::slice;
use std::sync::Arc;

// ugh, we are not allowed to take refs, so...
#[derive(Clone)]
pub enum KernelArgValue {
    None,
    Constant(Vec<u8>),
    MemObject(&'static Mem),
    Sampler(&'static Sampler),
    LocalMem(usize),
}

#[derive(PartialEq, Eq, Clone)]
pub enum KernelArgType {
    Constant, // for anything passed by value
    Image,
    Sampler,
    Texture,
    MemGlobal,
    MemConstant,
    MemLocal,
}

#[derive(Hash, PartialEq, Eq, Clone)]
pub enum InternalKernelArgType {
    ConstantBuffer,
    GlobalWorkOffsets,
    PrintfBuffer,
    InlineSampler((cl_addressing_mode, cl_filter_mode, bool)),
}

#[derive(Clone)]
pub struct KernelArg {
    spirv: spirv::SPIRVKernelArg,
    pub kind: KernelArgType,
    pub size: usize,
    pub offset: usize,
    pub dead: bool,
}

#[derive(Hash, PartialEq, Eq, Clone)]
pub struct InternalKernelArg {
    pub kind: InternalKernelArgType,
    pub size: usize,
    pub offset: usize,
}

impl KernelArg {
    fn from_spirv_nir(spirv: Vec<spirv::SPIRVKernelArg>, nir: &mut NirShader) -> Vec<Self> {
        let nir_arg_map: HashMap<_, _> = nir
            .variables_with_mode(
                nir_variable_mode::nir_var_uniform | nir_variable_mode::nir_var_image,
            )
            .map(|v| (v.data.location, v))
            .collect();
        let mut res = Vec::new();

        for (i, s) in spirv.into_iter().enumerate() {
            let nir = nir_arg_map.get(&(i as i32)).unwrap();
            let kind = match s.address_qualifier {
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_PRIVATE => {
                    if unsafe { glsl_type_is_sampler(nir.type_) } {
                        KernelArgType::Sampler
                    } else {
                        KernelArgType::Constant
                    }
                }
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_CONSTANT => {
                    KernelArgType::MemConstant
                }
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_LOCAL => {
                    KernelArgType::MemLocal
                }
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_GLOBAL => {
                    if unsafe { glsl_type_is_image(nir.type_) } {
                        if nir.data.access() == gl_access_qualifier::ACCESS_NON_WRITEABLE.0 {
                            KernelArgType::Texture
                        } else {
                            KernelArgType::Image
                        }
                    } else {
                        KernelArgType::MemGlobal
                    }
                }
            };

            res.push(Self {
                spirv: s,
                size: unsafe { glsl_get_cl_size(nir.type_) } as usize,
                // we'll update it later in the 2nd pass
                kind: kind,
                offset: 0,
                dead: true,
            });
        }
        res
    }

    fn assign_locations(
        args: &mut Vec<Self>,
        internal_args: &mut Vec<InternalKernelArg>,
        nir: &mut NirShader,
    ) {
        for var in nir.variables_with_mode(
            nir_variable_mode::nir_var_uniform | nir_variable_mode::nir_var_image,
        ) {
            if let Some(arg) = args.get_mut(var.data.location as usize) {
                arg.offset = var.data.driver_location as usize;
                arg.dead = false;
            } else {
                internal_args
                    .get_mut(var.data.location as usize - args.len())
                    .unwrap()
                    .offset = var.data.driver_location as usize;
            }
        }
    }
}

#[repr(C)]
pub struct Kernel {
    pub base: CLObjectBase<CL_INVALID_KERNEL>,
    pub prog: Arc<Program>,
    pub name: String,
    pub args: Vec<KernelArg>,
    pub values: Vec<RefCell<Option<KernelArgValue>>>,
    pub work_group_size: [usize; 3],
    internal_args: Vec<InternalKernelArg>,
    nirs: HashMap<Arc<Device>, NirShader>,
}

impl_cl_type_trait!(cl_kernel, Kernel, CL_INVALID_KERNEL);

fn create_kernel_arr<T>(vals: &[usize], val: T) -> [T; 3]
where
    T: std::convert::TryFrom<usize> + Copy,
    <T as std::convert::TryFrom<usize>>::Error: std::fmt::Debug,
{
    let mut res = [val; 3];
    for (i, v) in vals.iter().enumerate() {
        res[i] = (*v).try_into().expect("64 bit work groups not supported");
    }
    res
}

struct RusticlLowerConstantBufferState {
    base_global_invoc_id: *mut nir_variable,
    const_buf: *mut nir_variable,
    printf_buf: *mut nir_variable,
}

impl Default for RusticlLowerConstantBufferState {
    fn default() -> Self {
        Self {
            base_global_invoc_id: ptr::null_mut(),
            const_buf: ptr::null_mut(),
            printf_buf: ptr::null_mut(),
        }
    }
}

unsafe extern "C" fn rusticl_lower_intrinsics_filter(
    instr: *const nir_instr,
    _: *const c_void,
) -> bool {
    (*instr).type_ == nir_instr_type::nir_instr_type_intrinsic
}

unsafe extern "C" fn rusticl_lower_intrinsics_instr(
    b: *mut nir_builder,
    instr: *mut nir_instr,
    state: *mut c_void,
) -> *mut nir_ssa_def {
    let instr = &*nir_instr_as_intrinsic(instr);
    let state: &mut RusticlLowerConstantBufferState = &mut *state.cast();

    match instr.intrinsic {
        nir_intrinsic_op::nir_intrinsic_load_base_global_invocation_id => {
            nir_load_var(b, state.base_global_invoc_id)
        }
        nir_intrinsic_op::nir_intrinsic_load_constant_base_ptr => nir_load_var(b, state.const_buf),
        nir_intrinsic_op::nir_intrinsic_load_printf_buffer_address => {
            nir_load_var(b, state.printf_buf)
        }
        _ => ptr::null_mut(),
    }
}

extern "C" fn rusticl_lower_intrinsics(
    nir: *mut nir_shader,
    state: *mut RusticlLowerConstantBufferState,
) -> bool {
    unsafe {
        nir_shader_lower_instructions(
            nir,
            Some(rusticl_lower_intrinsics_filter),
            Some(rusticl_lower_intrinsics_instr),
            state.cast(),
        )
    }
}

fn opt_nir(nir: &mut NirShader, dev: &Device) {
    let nir_options = unsafe {
        &*dev
            .screen
            .nir_shader_compiler_options(pipe_shader_type::PIPE_SHADER_COMPUTE)
    };

    while {
        let mut progress = false;

        progress |= nir.pass0(nir_copy_prop);
        progress |= nir.pass0(nir_opt_copy_prop_vars);
        progress |= nir.pass0(nir_opt_dead_write_vars);

        if nir_options.lower_to_scalar {
            nir.pass2(
                nir_lower_alu_to_scalar,
                nir_options.lower_to_scalar_filter,
                ptr::null(),
            );
            nir.pass1(nir_lower_phis_to_scalar, false);
        }

        progress |= nir.pass0(nir_opt_deref);
        progress |= nir.pass0(nir_opt_memcpy);
        progress |= nir.pass0(nir_opt_dce);
        progress |= nir.pass0(nir_opt_undef);
        progress |= nir.pass0(nir_opt_constant_folding);
        progress |= nir.pass0(nir_opt_cse);
        progress |= nir.pass0(nir_split_var_copies);
        progress |= nir.pass0(nir_lower_var_copies);
        progress |= nir.pass0(nir_lower_vars_to_ssa);
        nir.pass0(nir_lower_alu);
        nir.pass0(nir_lower_pack);
        progress |= nir.pass0(nir_opt_phi_precision);
        progress |= nir.pass0(nir_opt_algebraic);
        progress |= nir.pass1(nir_opt_if, true);
        progress |= nir.pass0(nir_opt_dead_cf);
        progress |= nir.pass0(nir_opt_remove_phis);
        // we don't want to be too aggressive here, but it kills a bit of CFG
        progress |= nir.pass3(nir_opt_peephole_select, 8, true, true);
        progress |= nir.pass1(
            nir_lower_vec3_to_vec4,
            nir_variable_mode::nir_var_mem_generic | nir_variable_mode::nir_var_uniform,
        );

        if nir_options.max_unroll_iterations != 0 {
            progress |= nir.pass0(nir_opt_loop_unroll);
        }
        nir.sweep_mem();
        progress
    } {}
}

fn lower_and_optimize_nir_pre_inputs(dev: &Device, nir: &mut NirShader, lib_clc: &NirShader) {
    nir.set_workgroup_size_variable_if_zero();
    nir.structurize();
    while {
        let mut progress = false;
        progress |= nir.pass0(nir_copy_prop);
        progress |= nir.pass0(nir_opt_copy_prop_vars);
        progress |= nir.pass0(nir_opt_dead_write_vars);
        progress |= nir.pass0(nir_opt_deref);
        progress |= nir.pass0(nir_opt_dce);
        progress |= nir.pass0(nir_opt_undef);
        progress |= nir.pass0(nir_opt_constant_folding);
        progress |= nir.pass0(nir_opt_cse);
        progress |= nir.pass0(nir_lower_vars_to_ssa);
        progress |= nir.pass0(nir_opt_algebraic);
        progress
    } {}
    nir.inline(lib_clc);
    nir.remove_non_entrypoints();
    // that should free up tons of memory
    nir.sweep_mem();

    nir.pass0(nir_move_inline_samplers_to_end);
    nir.pass2(
        nir_lower_vars_to_explicit_types,
        nir_variable_mode::nir_var_uniform
            | nir_variable_mode::nir_var_function_temp
            | nir_variable_mode::nir_var_shader_temp
            | nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_mem_generic
            | nir_variable_mode::nir_var_mem_global,
        Some(glsl_get_cl_type_size_align),
    );

    let mut printf_opts = nir_lower_printf_options::default();
    printf_opts.set_treat_doubles_as_floats(false);
    printf_opts.max_buffer_size = dev.printf_buffer_size() as u32;
    nir.pass1(nir_lower_printf, &printf_opts);

    opt_nir(nir, dev);
}

extern "C" fn can_remove_var(var: *mut nir_variable, _: *mut c_void) -> bool {
    unsafe {
        let var = var.as_ref().unwrap();

        !glsl_type_is_sampler(var.type_)
            && !glsl_type_is_image(var.type_)
            && !glsl_type_is_image(var.type_)
    }
}

fn lower_and_optimize_nir_late(
    dev: &Device,
    nir: &mut NirShader,
    args: usize,
) -> Vec<InternalKernelArg> {
    let mut res = Vec::new();
    let nir_options = unsafe {
        &*dev
            .screen
            .nir_shader_compiler_options(pipe_shader_type::PIPE_SHADER_COMPUTE)
    };
    let mut lower_state = RusticlLowerConstantBufferState::default();

    // asign locations for inline samplers
    let mut last_loc = -1;
    for v in nir.variables_with_mode(nir_variable_mode::nir_var_uniform) {
        if unsafe { !glsl_type_is_sampler(v.type_) } {
            continue;
        }
        let s = unsafe { v.data.anon_1.sampler };
        if s.is_inline_sampler() != 0 {
            last_loc += 1;
            v.data.location = last_loc;

            res.push(InternalKernelArg {
                kind: InternalKernelArgType::InlineSampler(Sampler::nir_to_cl(
                    s.addressing_mode(),
                    s.filter_mode(),
                    s.normalized_coordinates(),
                )),
                offset: 0,
                size: 0,
            });
        } else {
            last_loc = v.data.location;
        }
    }

    nir.pass1(nir_lower_readonly_images_to_tex, false);
    nir.pass0(nir_lower_cl_images);
    nir.pass0(nir_lower_memcpy);

    let dv_opts = nir_remove_dead_variables_options {
        can_remove_var: Some(can_remove_var),
        can_remove_var_data: ptr::null_mut(),
    };
    nir.pass2(
        nir_remove_dead_variables,
        nir_variable_mode::nir_var_uniform
            | nir_variable_mode::nir_var_image
            | nir_variable_mode::nir_var_mem_constant
            | nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_function_temp,
        &dv_opts,
    );
    nir.reset_scratch_size();
    nir.pass2(
        nir_lower_vars_to_explicit_types,
        nir_variable_mode::nir_var_mem_constant,
        Some(glsl_get_cl_type_size_align),
    );
    nir.extract_constant_initializers();

    // TODO 32 bit devices
    // add vars for global offsets
    res.push(InternalKernelArg {
        kind: InternalKernelArgType::GlobalWorkOffsets,
        offset: 0,
        size: 24,
    });
    lower_state.base_global_invoc_id = nir.add_var(
        nir_variable_mode::nir_var_uniform,
        unsafe { glsl_vector_type(glsl_base_type::GLSL_TYPE_UINT64, 3) },
        args + res.len() - 1,
        "base_global_invocation_id",
    );
    if nir.has_constant() {
        res.push(InternalKernelArg {
            kind: InternalKernelArgType::ConstantBuffer,
            offset: 0,
            size: 8,
        });
        lower_state.const_buf = nir.add_var(
            nir_variable_mode::nir_var_uniform,
            unsafe { glsl_uint64_t_type() },
            args + res.len() - 1,
            "constant_buffer_addr",
        );
    }
    if nir.has_printf() {
        res.push(InternalKernelArg {
            kind: InternalKernelArgType::PrintfBuffer,
            offset: 0,
            size: 8,
        });
        lower_state.printf_buf = nir.add_var(
            nir_variable_mode::nir_var_uniform,
            unsafe { glsl_uint64_t_type() },
            args + res.len() - 1,
            "printf_buffer_addr",
        );
    }

    nir.pass2(
        nir_lower_vars_to_explicit_types,
        nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_function_temp
            | nir_variable_mode::nir_var_uniform
            | nir_variable_mode::nir_var_mem_global,
        Some(glsl_get_cl_type_size_align),
    );
    nir.pass2(
        nir_lower_explicit_io,
        nir_variable_mode::nir_var_mem_global | nir_variable_mode::nir_var_mem_constant,
        nir_address_format::nir_address_format_64bit_global,
    );
    nir.pass0(nir_lower_system_values);
    let mut compute_options = nir_lower_compute_system_values_options::default();
    compute_options.set_has_base_global_invocation_id(true);
    nir.pass1(nir_lower_compute_system_values, &compute_options);
    nir.pass1(rusticl_lower_intrinsics, &mut lower_state);
    nir.pass2(
        nir_lower_explicit_io,
        nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_function_temp
            | nir_variable_mode::nir_var_uniform,
        nir_address_format::nir_address_format_32bit_offset_as_64bit,
    );

    if nir_options.lower_int64_options.0 != 0 {
        nir.pass0(nir_lower_int64);
    }

    nir.pass1(nir_lower_convert_alu_types, None);

    opt_nir(nir, dev);
    dev.screen.finalize_nir(nir);

    nir.pass0(nir_opt_dce);
    nir.sweep_mem();
    res
}

fn extract<'a, const S: usize>(buf: &'a mut &[u8]) -> &'a [u8; S] {
    let val;
    (val, *buf) = (*buf).split_at(S);
    // we split of 4 bytes and convert to [u8; 4], so this should be safe
    // use split_array_ref once it's stable
    val.try_into().unwrap()
}

impl Kernel {
    pub fn new(
        name: String,
        prog: Arc<Program>,
        mut nirs: HashMap<Arc<Device>, NirShader>,
        args: Vec<spirv::SPIRVKernelArg>,
    ) -> Arc<Kernel> {
        nirs.iter_mut()
            .for_each(|(d, n)| lower_and_optimize_nir_pre_inputs(d, n, &d.lib_clc));
        let nir = nirs.values_mut().next().unwrap();
        let wgs = nir.workgroup_size();
        let work_group_size = [wgs[0] as usize, wgs[1] as usize, wgs[2] as usize];
        let mut args = KernelArg::from_spirv_nir(args, nir);
        // can't use vec!...
        let values = args.iter().map(|_| RefCell::new(None)).collect();
        let internal_args: HashSet<_> = nirs
            .iter_mut()
            .map(|(d, n)| lower_and_optimize_nir_late(d, n, args.len()))
            .collect();
        // we want the same internal args for every compiled kernel, for now
        assert!(internal_args.len() == 1);
        let mut internal_args = internal_args.into_iter().next().unwrap();

        nirs.values_mut()
            .for_each(|n| KernelArg::assign_locations(&mut args, &mut internal_args, n));

        Arc::new(Self {
            base: CLObjectBase::new(),
            prog: prog,
            name: name,
            args: args,
            work_group_size: work_group_size,
            values: values,
            internal_args: internal_args,
            // caller has to verify all kernels have the same sig
            nirs: nirs,
        })
    }

    // the painful part is, that host threads are allowed to modify the kernel object once it was
    // enqueued, so return a closure with all req data included.
    pub fn launch(
        self: &Arc<Self>,
        q: &Arc<Queue>,
        work_dim: u32,
        block: &[usize],
        grid: &[usize],
        offsets: &[usize],
    ) -> CLResult<EventSig> {
        let mut block = create_kernel_arr::<u32>(block, 1);
        let mut grid = create_kernel_arr::<u32>(grid, 1);
        let offsets = create_kernel_arr::<u64>(offsets, 0);
        let mut input: Vec<u8> = Vec::new();
        let mut resource_info = Vec::new();
        let mut local_size: u32 = 0;
        let printf_size = q.device.printf_buffer_size() as u32;
        let mut samplers = Vec::new();
        let mut iviews = Vec::new();
        let mut sviews = Vec::new();

        for i in 0..3 {
            if block[i] == 0 {
                if i == 0 {
                    // TODO: make this more nice, but at least that should work
                    let threads = q.device.max_block_sizes()[i] as u32;
                    if grid[0] % threads == 0 {
                        block[i] = threads;
                        grid[i] /= threads;
                    } else {
                        block[i] = 1;
                    }
                } else {
                    block[i] = 1;
                }
            } else {
                // we already made sure everything is fine
                grid[i] /= block[i];
            }
        }

        for (arg, val) in self.args.iter().zip(&self.values) {
            if arg.dead {
                continue;
            }

            if arg.kind != KernelArgType::Image
                && arg.kind != KernelArgType::Texture
                && arg.kind != KernelArgType::Sampler
            {
                input.resize(arg.offset, 0);
            }
            match val.borrow().as_ref().unwrap() {
                KernelArgValue::Constant(c) => input.extend_from_slice(&c),
                KernelArgValue::MemObject(mem) => {
                    let res = mem.get_res_of_dev(&q.device)?;
                    if mem.is_buffer() {
                        input.extend_from_slice(&mem.offset.to_ne_bytes());
                        resource_info.push((Some(res.clone()), arg.offset));
                    } else {
                        if arg.kind == KernelArgType::Image {
                            iviews.push(res.pipe_image_view())
                        } else {
                            sviews.push(q.context().create_sampler_view(res))
                        }
                    }
                }
                KernelArgValue::LocalMem(size) => {
                    // TODO 32 bit
                    input.extend_from_slice(&[0; 8]);
                    local_size += *size as u32;
                }
                KernelArgValue::Sampler(sampler) => {
                    samplers.push(q.context().create_sampler_state(&sampler.pipe()));
                }
                KernelArgValue::None => {
                    assert!(
                        arg.kind == KernelArgType::MemGlobal
                            || arg.kind == KernelArgType::MemConstant
                    );
                    input.extend_from_slice(&[0; 8]);
                }
            }
        }

        let nir = self.nirs.get(&q.device).unwrap();
        let mut printf_buf = None;
        for arg in &self.internal_args {
            if arg.offset > input.len() {
                input.resize(arg.offset, 0);
            }
            match arg.kind {
                InternalKernelArgType::ConstantBuffer => {
                    input.extend_from_slice(&[0; 8]);
                    let buf = nir.get_constant_buffer();
                    let res = Arc::new(
                        q.device
                            .screen()
                            .resource_create_buffer(buf.len() as u32)
                            .unwrap(),
                    );
                    q.device.helper_ctx.buffer_subdata(
                        &res,
                        0,
                        buf.as_ptr().cast(),
                        buf.len() as u32,
                    );
                    resource_info.push((Some(res), arg.offset));
                }
                InternalKernelArgType::GlobalWorkOffsets => {
                    input.extend_from_slice(&cl_prop::<[u64; 3]>(offsets));
                }
                InternalKernelArgType::PrintfBuffer => {
                    let buf =
                        Arc::new(q.device.screen.resource_create_buffer(printf_size).unwrap());

                    input.extend_from_slice(&[0; 8]);
                    resource_info.push((Some(buf.clone()), arg.offset));

                    printf_buf = Some(buf);
                }
                InternalKernelArgType::InlineSampler(cl) => {
                    samplers.push(q.context().create_sampler_state(&Sampler::cl_to_pipe(cl)));
                }
            }
        }

        let k = self.clone();
        Ok(Box::new(move |q| {
            let nir = k.nirs.get(&q.device).unwrap();
            let mut input = input.clone();
            let mut resources = Vec::with_capacity(resource_info.len());
            let mut globals: Vec<*mut u32> = Vec::new();
            let printf_format = nir.printf_format();
            let printf_buf = printf_buf.clone();
            let iviews = iviews.clone();
            let mut sviews = sviews.clone();

            for (res, offset) in resource_info.clone() {
                resources.push(res);
                globals.push(unsafe { input.as_mut_ptr().add(offset) }.cast());
            }

            if let Some(printf_buf) = &printf_buf {
                let init_data: [u8; 1] = [4];
                q.context().buffer_subdata(
                    &printf_buf,
                    0,
                    init_data.as_ptr().cast(),
                    init_data.len() as u32,
                );
            }
            let cso = q
                .context()
                .create_compute_state(nir, input.len() as u32, local_size);

            q.context().bind_compute_state(cso);
            q.context().bind_sampler_states(&samplers);
            q.context().set_sampler_views(&mut sviews);
            q.context().set_shader_images(&iviews);
            q.context()
                .set_global_binding(resources.as_slice(), &mut globals);

            q.context().launch_grid(work_dim, block, grid, &input);

            q.context().clear_global_binding(globals.len() as u32);
            q.context().clear_shader_images(iviews.len() as u32);
            q.context().clear_sampler_views(sviews.len() as u32);
            q.context().clear_sampler_states(samplers.len() as u32);
            q.context().delete_compute_state(cso);
            q.context().memory_barrier(PIPE_BARRIER_GLOBAL_BUFFER);

            samplers
                .iter()
                .for_each(|s| q.context().delete_sampler_state(*s));
            sviews
                .iter()
                .for_each(|v| q.context().sampler_view_destroy(*v));

            if let Some(printf_buf) = &printf_buf {
                let tx = q
                    .context()
                    .buffer_map(&printf_buf, 0, printf_size as i32, true);
                let mut buf: &[u8] =
                    unsafe { slice::from_raw_parts(tx.ptr().cast(), printf_size as usize) };
                let length = u32::from_ne_bytes(*extract(&mut buf));

                // update our slice to make sure we don't go out of bounds
                buf = &buf[0..(length - 4) as usize];
                eprintln!("printf length {:?}", buf);

                // as long as we can still read out a printf format string id continue
                while buf.len() >= 4 {
                    let id = u32::from_ne_bytes(*extract(&mut buf)) as usize;
                    let format = printf_format[id - 1];
                    let string: &[u8] = unsafe {
                        slice::from_raw_parts(format.strings.cast(), format.string_size as usize)
                    };
                    let _args = unsafe {
                        slice::from_raw_parts(format.arg_sizes, format.num_args as usize)
                    };

                    // flags
                    let mut iter = string.iter();
                    // throw away '%'
                    iter.next();

                    let mut left_justified = false;
                    let mut leading_zeros = false;
                    let mut always_sign = false;
                    let mut special = false;
                    let mut width: usize = 0;
                    let mut precision: usize = 0;
                    let mut _vector = 1;
                    let mut _length = 0;

                    // TODO: replace with regex crate
                    let mut c = *iter.next().unwrap();
                    loop {
                        match c {
                            b'-' => {
                                left_justified = true;
                                c = *iter.next().unwrap()
                            }
                            b'+' => {
                                always_sign = true;
                                c = *iter.next().unwrap()
                            }
                            b' ' => assert!(false),
                            b'#' => {
                                special = true;
                                c = *iter.next().unwrap()
                            }
                            b'0' => {
                                leading_zeros = true;
                                c = *iter.next().unwrap()
                            }
                            _ => break,
                        }
                    }

                    loop {
                        match c {
                            b'0' => {
                                width *= 10;
                                width += 0;
                                c = *iter.next().unwrap()
                            }
                            b'1' => {
                                width *= 10;
                                width += 1;
                                c = *iter.next().unwrap()
                            }
                            b'2' => {
                                width *= 10;
                                width += 2;
                                c = *iter.next().unwrap()
                            }
                            b'3' => {
                                width *= 10;
                                width += 3;
                                c = *iter.next().unwrap()
                            }
                            b'4' => {
                                width *= 10;
                                width += 4;
                                c = *iter.next().unwrap()
                            }
                            b'5' => {
                                width *= 10;
                                width += 5;
                                c = *iter.next().unwrap()
                            }
                            b'6' => {
                                width *= 10;
                                width += 6;
                                c = *iter.next().unwrap()
                            }
                            b'7' => {
                                width *= 10;
                                width += 7;
                                c = *iter.next().unwrap()
                            }
                            b'8' => {
                                width *= 10;
                                width += 8;
                                c = *iter.next().unwrap()
                            }
                            b'9' => {
                                width *= 10;
                                width += 9;
                                c = *iter.next().unwrap()
                            }
                            _ => {
                                width = cmp::max(width, 1);
                                break;
                            }
                        }
                    }

                    if c == b'.' {
                        c = *iter.next().unwrap();
                        loop {
                            match c {
                                b'0' => {
                                    precision *= 10;
                                    precision += 0;
                                    c = *iter.next().unwrap()
                                }
                                b'1' => {
                                    precision *= 10;
                                    precision += 1;
                                    c = *iter.next().unwrap()
                                }
                                b'2' => {
                                    precision *= 10;
                                    precision += 2;
                                    c = *iter.next().unwrap()
                                }
                                b'3' => {
                                    precision *= 10;
                                    precision += 3;
                                    c = *iter.next().unwrap()
                                }
                                b'4' => {
                                    precision *= 10;
                                    precision += 4;
                                    c = *iter.next().unwrap()
                                }
                                b'5' => {
                                    precision *= 10;
                                    precision += 5;
                                    c = *iter.next().unwrap()
                                }
                                b'6' => {
                                    precision *= 10;
                                    precision += 6;
                                    c = *iter.next().unwrap()
                                }
                                b'7' => {
                                    precision *= 10;
                                    precision += 7;
                                    c = *iter.next().unwrap()
                                }
                                b'8' => {
                                    precision *= 10;
                                    precision += 8;
                                    c = *iter.next().unwrap()
                                }
                                b'9' => {
                                    precision *= 10;
                                    precision += 9;
                                    c = *iter.next().unwrap()
                                }
                                _ => break,
                            }
                        }
                    }

                    if c == b'v' {
                        c = *iter.next().unwrap();
                        match c {
                            b'1' => {
                                _vector = 16;
                                iter.next();
                            }
                            b'2' => _vector = 2,
                            b'3' => _vector = 3,
                            b'4' => _vector = 4,
                            b'8' => _vector = 8,
                            _ => panic!("invalid vec"),
                        }
                        c = *iter.next().unwrap();
                    }

                    match c {
                        b'h' | b'l' => {
                            let c2 = *iter.next().unwrap();

                            _length = if c == b'h' && c2 == b'h' {
                                1
                            } else if c == b'h' && c2 == b'l' {
                                4
                            } else if c == b'h' {
                                2
                            } else if c == b'l' {
                                8
                            } else {
                                0
                            };

                            c = if _length == 1 || _length == 4 {
                                *iter.next().unwrap()
                            } else {
                                c2
                            }
                        }
                        _ => (),
                    }

                    let mut is_int = false;
                    let mut is_float = false;
                    let mut is_zero = false;
                    let mut val;
                    match c {
                        b'c' => {
                            val = String::from(
                                char::from_u32(u32::from_ne_bytes(*extract(&mut buf))).unwrap(),
                            );
                        }
                        b's' => {
                            let offset = u64::from_ne_bytes(*extract(&mut buf)) as usize;
                            let str =
                                unsafe { CStr::from_ptr((&string[offset..]).as_ptr().cast()) }
                                    .to_string_lossy();

                            val = if precision != 0 {
                                str[..precision].to_string()
                            } else {
                                str.into_owned()
                            }
                        }

                        b'o' => {
                            let prefix = if special { "0" } else { "" };
                            let nr = u32::from_ne_bytes(*extract(&mut buf));
                            is_zero = nr == 0;
                            val = format!("{}{:o}", prefix, nr);
                            is_int = true;
                        }
                        b'd' | b'i' => {
                            let nr = i32::from_ne_bytes(*extract(&mut buf));
                            val = nr.to_string();
                            is_zero = nr == 0;
                            is_int = true;
                        }
                        b'u' => {
                            let nr = u32::from_ne_bytes(*extract(&mut buf));
                            val = nr.to_string();
                            is_zero = nr == 0;
                            is_int = true;
                        }
                        b'x' => {
                            let prefix = if special { "0x" } else { "" };
                            let nr = u32::from_ne_bytes(*extract(&mut buf));
                            is_zero = nr == 0;
                            val = format!("{}{:x}", prefix, nr);
                            is_int = true;
                        }
                        b'X' => {
                            let prefix = if special { "0X" } else { "" };
                            let nr = u32::from_ne_bytes(*extract(&mut buf));
                            is_zero = nr == 0;
                            val = format!("{}{:X}", prefix, nr);
                            is_int = true;
                        }
                        b'p' => {
                            let nr = u64::from_ne_bytes(*extract(&mut buf));
                            val = format!("{:#016x?}", nr);
                        }

                        b'f' => {
                            let nr = f64::from_ne_bytes(*extract(&mut buf));
                            val = format!("{}", nr);
                            is_float = true;
                        }
                        _ => panic!("unknown type"),
                    }

                    // post processing
                    leading_zeros &= !left_justified && (!is_int || precision == 0);

                    if is_zero {
                        if is_int && special {
                            println!("0");
                            eprintln!("0");
                        } else {
                            println!("");
                            eprintln!("");
                        }
                        continue;
                    }

                    if is_int {
                        precision = precision.checked_sub(val.len()).unwrap_or(0);
                        for _ in 0..precision {
                            val.insert(0, '0');
                        }
                    }

                    if is_float {
                        precision = if precision == 0 { 6 } else { precision };
                        let fraction = val.split_once('.');

                        if let Some(fraction) = fraction {
                            let fraction = fraction.1.len();
                            if precision > fraction {
                                precision -= fraction;
                                for _ in 0..precision {
                                    val.push('0');
                                }
                            } else {
                                let (a, _) = val.split_at(val.len() - (fraction - precision));
                                val = String::from(a);
                            }
                        }
                    }

                    if always_sign && val.chars().nth(0).unwrap() != '-' {
                        val = format!("+{}", val);
                    }

                    let mut padding = String::from("");
                    let padding_char = if leading_zeros { '0' } else { ' ' };
                    width = width.checked_sub(val.len()).unwrap_or(0);

                    for _ in 0..width {
                        padding.push(padding_char);
                    }

                    if left_justified {
                        println!("{}{}", val, padding);
                        eprintln!("{}{}", val, padding);
                    } else {
                        println!("{}{}", padding, val);
                        eprintln!("{}{}", padding, val);
                    }
                }

                drop(tx);
            }

            Ok(())
        }))
    }

    pub fn access_qualifier(&self, idx: cl_uint) -> cl_kernel_arg_access_qualifier {
        let aq = self.args[idx as usize].spirv.access_qualifier;

        if aq
            == clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_READ
                | clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_WRITE
        {
            CL_KERNEL_ARG_ACCESS_READ_WRITE
        } else if aq == clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_READ {
            CL_KERNEL_ARG_ACCESS_READ_ONLY
        } else if aq == clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_WRITE {
            CL_KERNEL_ARG_ACCESS_WRITE_ONLY
        } else {
            CL_KERNEL_ARG_ACCESS_NONE
        }
    }

    pub fn address_qualifier(&self, idx: cl_uint) -> cl_kernel_arg_address_qualifier {
        match self.args[idx as usize].spirv.address_qualifier {
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_PRIVATE => {
                CL_KERNEL_ARG_ADDRESS_PRIVATE
            }
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_CONSTANT => {
                CL_KERNEL_ARG_ADDRESS_CONSTANT
            }
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_LOCAL => {
                CL_KERNEL_ARG_ADDRESS_LOCAL
            }
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_GLOBAL => {
                CL_KERNEL_ARG_ADDRESS_GLOBAL
            }
        }
    }

    pub fn type_qualifier(&self, idx: cl_uint) -> cl_kernel_arg_type_qualifier {
        let tq = self.args[idx as usize].spirv.type_qualifier;
        let zero = clc_kernel_arg_type_qualifier(0);
        let mut res = CL_KERNEL_ARG_TYPE_NONE;

        if tq & clc_kernel_arg_type_qualifier::CLC_KERNEL_ARG_TYPE_CONST != zero {
            res |= CL_KERNEL_ARG_TYPE_CONST;
        }

        if tq & clc_kernel_arg_type_qualifier::CLC_KERNEL_ARG_TYPE_RESTRICT != zero {
            res |= CL_KERNEL_ARG_TYPE_RESTRICT;
        }

        if tq & clc_kernel_arg_type_qualifier::CLC_KERNEL_ARG_TYPE_VOLATILE != zero {
            res |= CL_KERNEL_ARG_TYPE_VOLATILE;
        }

        res.into()
    }

    pub fn arg_name(&self, idx: cl_uint) -> &String {
        &self.args[idx as usize].spirv.name
    }

    pub fn arg_type_name(&self, idx: cl_uint) -> &String {
        &self.args[idx as usize].spirv.type_name
    }

    pub fn priv_mem_size(&self, dev: &Arc<Device>) -> cl_ulong {
        self.nirs.get(dev).unwrap().scratch_size() as cl_ulong
    }

    pub fn local_mem_size(&self, dev: &Arc<Device>) -> cl_ulong {
        // TODO include args
        self.nirs.get(dev).unwrap().shared_size() as cl_ulong
    }
}

impl Clone for Kernel {
    fn clone(&self) -> Self {
        Self {
            base: CLObjectBase::new(),
            prog: self.prog.clone(),
            name: self.name.clone(),
            args: self.args.clone(),
            values: self.values.clone(),
            work_group_size: self.work_group_size.clone(),
            internal_args: self.internal_args.clone(),
            nirs: self.nirs.clone(),
        }
    }
}
