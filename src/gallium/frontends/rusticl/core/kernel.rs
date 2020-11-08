extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::decl_cl_type;
use crate::init_cl_type;

use self::rusticl_opencl_gen::*;

use std::ptr;

decl_cl_type!(_cl_kernel, CLKernel, CLKernelRef, CL_INVALID_KERNEL);

pub struct CLKernel {
    pub cl: cl_kernel,
}

impl CLKernel {
    pub fn new() -> CLKernelRef {
        let c = Self {
            cl: ptr::null_mut(),
        };

        init_cl_type!(c, _cl_kernel)
    }
}
