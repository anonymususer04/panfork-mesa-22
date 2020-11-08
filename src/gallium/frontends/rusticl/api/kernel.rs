extern crate rusticl_opencl_gen;

use crate::api::util::*;

use self::rusticl_opencl_gen::*;

pub fn create_kernel(
    program: cl_program,
    kernel_name: *const ::std::os::raw::c_char,
) -> Result<cl_kernel, cl_int> {
    let _p = program.check()?;

    // CL_INVALID_VALUE if kernel_name is NULL.
    if kernel_name.is_null() {
        Err(CL_INVALID_VALUE)?;
    }

    println!("create_kernel not implemented");
    Err(CL_OUT_OF_HOST_MEMORY)

    //• CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built executable for program.
    //• CL_INVALID_KERNEL_NAME if kernel_name is not found in program.
    //• CL_INVALID_KERNEL_DEFINITION if the function definition for __kernel function given by kernel_name such as the number of arguments, the argument types are not the same for all devices for which the program executable has been built.
}
