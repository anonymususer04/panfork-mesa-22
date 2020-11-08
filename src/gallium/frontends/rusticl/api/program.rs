extern crate mesa_rust;
extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::types::*;
use crate::api::util::*;
use crate::core::device::*;
use crate::core::program::*;

use self::mesa_rust::compiler::clc::*;
use self::mesa_rust_util::string::*;
use self::rusticl_opencl_gen::*;

use std::ffi::CStr;
use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;
use std::slice;

impl CLInfo<cl_program_info> for crate::core::program::_cl_program {
    fn query(&self, q: cl_program_info) -> Result<Vec<u8>, cl_int> {
        Ok(match q {
            CL_PROGRAM_CONTEXT => cl_prop::<cl_context>(self.context.cl),
            CL_PROGRAM_DEVICES => {
                cl_prop::<&Vec<cl_device_id>>(&self.devs.iter().map(|d| d.cl).collect())
            }
            CL_PROGRAM_NUM_DEVICES => cl_prop::<cl_uint>(self.devs.len() as cl_uint),
            CL_PROGRAM_NUM_KERNELS => cl_prop::<usize>(self.kernels.len()),
            CL_PROGRAM_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refs()),
            CL_PROGRAM_SOURCE => cl_prop::<&CStr>(self.src.as_c_str()),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

impl CLInfoObj<cl_program_build_info, cl_device_id> for crate::core::program::_cl_program {
    fn query(&self, d: cl_device_id, q: cl_program_build_info) -> Result<Vec<u8>, cl_int> {
        let dev = d.check()?;
        Ok(match q {
            CL_PROGRAM_BUILD_LOG => cl_prop::<String>(self.log(dev)),
            CL_PROGRAM_BUILD_OPTIONS => cl_prop::<String>(self.options(dev)),
            CL_PROGRAM_BUILD_STATUS => cl_prop::<cl_build_status>(self.status(dev)),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

fn validate_devices(
    device_list: *const cl_device_id,
    num_devices: cl_uint,
    default: &Vec<CLDeviceRef>,
) -> Result<Vec<&CLDeviceRef>, cl_int> {
    let mut devs = check_cl_objs(device_list, num_devices)?;

    // If device_list is a NULL value, the compile is performed for all devices associated with
    // program.
    if devs.is_empty() {
        devs = default.iter().collect();
    }

    Ok(devs)
}

fn call_cb(
    pfn_notify: Option<ProgramCB>,
    program: cl_program,
    user_data: *mut ::std::os::raw::c_void,
) {
    if let Some(cb) = pfn_notify {
        unsafe { cb(program, user_data) };
    }
}

pub fn create_program_with_source(
    context: cl_context,
    count: cl_uint,
    strings: *mut *const c_char,
    lengths: *const usize,
) -> Result<cl_program, cl_int> {
    let c = context.check()?;

    // CL_INVALID_VALUE if count is zero or if strings ...
    if count == 0 || strings.is_null() {
        Err(CL_INVALID_VALUE)?
    }

    // ... or any entry in strings is NULL.
    let srcs = unsafe { slice::from_raw_parts(strings, count as usize) };
    if srcs.contains(&ptr::null()) {
        Err(CL_INVALID_VALUE)?
    }

    let mut source = String::new();
    // we don't want encoding or any other problems with the source to prevent compilations, so
    // just use CString::from_vec_unchecked and to_string_lossy
    for i in 0..count as usize {
        unsafe {
            if lengths.is_null() || *lengths.add(i) == 0 {
                source.push_str(&CStr::from_ptr(*strings.add(i)).to_string_lossy());
            } else {
                let l = *lengths.add(i);
                let arr = slice::from_raw_parts(*strings.add(i) as *const u8, l);
                source.push_str(&CString::from_vec_unchecked(arr.to_vec()).to_string_lossy());
            }
        }
    }

    Ok(CLProgram::new(
        c,
        &c.devs,
        CString::new(source).map_err(|_| CL_INVALID_VALUE)?,
    )
    .cl)
}

pub fn build_program(
    program: cl_program,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const c_char,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> Result<(), cl_int> {
    let mut res = true;
    let p = program.check()?;
    let devs = validate_devices(device_list, num_devices, &p.devs)?;

    check_cb(&pfn_notify, user_data)?;

    // CL_BUILD_PROGRAM_FAILURE if there is a failure to build the program executable. This error
    // will be returned if clBuildProgram does not return until the build has completed.
    for dev in devs {
        res &= p.compile(&dev, c_string_to_string(options), &Vec::new());
    }

    call_cb(pfn_notify, program, user_data);

    // TODO link

    //• CL_INVALID_BINARY if program is created with clCreateProgramWithBinary and devices listed in device_list do not have a valid program binary loaded.
    //• CL_INVALID_BUILD_OPTIONS if the build options specified by options are invalid.
    //• CL_INVALID_OPERATION if the build of a program executable for any of the devices listed in device_list by a previous call to clBuildProgram for program has not completed.
    //• CL_INVALID_OPERATION if there are kernel objects attached to program.
    //• CL_INVALID_OPERATION if program was not created with clCreateProgramWithSource, clCreateProgramWithIL or clCreateProgramWithBinary.

    if res {
        Ok(())
    } else {
        Err(CL_BUILD_PROGRAM_FAILURE)
    }
}

pub fn compile_program(
    program: cl_program,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const c_char,
    num_input_headers: cl_uint,
    input_headers: *const cl_program,
    header_include_names: *mut *const c_char,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> Result<(), cl_int> {
    let mut res = true;
    let p = program.check()?;
    let devs = validate_devices(device_list, num_devices, &p.devs)?;

    check_cb(&pfn_notify, user_data)?;

    // CL_INVALID_VALUE if num_input_headers is zero and header_include_names or input_headers are
    // not NULL or if num_input_headers is not zero and header_include_names or input_headers are
    // NULL.
    if num_input_headers == 0 && (!header_include_names.is_null() || !input_headers.is_null())
        || num_input_headers != 0 && (header_include_names.is_null() || input_headers.is_null())
    {
        Err(CL_INVALID_VALUE)?
    }

    let mut headers = Vec::new();
    for h in 0..num_input_headers as usize {
        unsafe {
            headers.push(spirv::CLCHeader {
                name: CStr::from_ptr(*header_include_names.add(h)).to_owned(),
                source: &(*input_headers.add(h)).check()?.src,
            });
        }
    }

    // CL_COMPILE_PROGRAM_FAILURE if there is a failure to compile the program source. This error
    // will be returned if clCompileProgram does not return until the compile has completed.
    for dev in devs {
        res &= p.compile(&dev, c_string_to_string(options), &headers);
    }

    call_cb(pfn_notify, program, user_data);

    // CL_INVALID_OPERATION if program has no source or IL available, i.e. it has not been created with clCreateProgramWithSource or clCreateProgramWithIL.
    // • CL_INVALID_COMPILER_OPTIONS if the compiler options specified by options are invalid.
    // • CL_INVALID_OPERATION if the compilation or build of a program executable for any of the devices listed in device_list by a previous call to clCompileProgram or clBuildProgram for program has not completed.
    // • CL_INVALID_OPERATION if there are kernel objects attached to program.

    if res {
        Ok(())
    } else {
        Err(CL_COMPILE_PROGRAM_FAILURE)
    }
}

pub fn link_program(
    context: cl_context,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    _options: *const ::std::os::raw::c_char,
    num_input_programs: cl_uint,
    input_programs: *const cl_program,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> Result<(cl_program, cl_int), cl_int> {
    let c = context.check()?;
    let devs = validate_devices(device_list, num_devices, &c.devs)?;
    let progs = check_cl_objs_mut(input_programs, num_input_programs)?;

    check_cb(&pfn_notify, user_data)?;

    // CL_INVALID_VALUE if num_input_programs is zero and input_programs is NULL
    if progs.is_empty() {
        Err(CL_INVALID_VALUE)?;
    }

    // CL_INVALID_DEVICE if any device in device_list is not in the list of devices associated with
    // context.
    if !devs.iter().all(|d| c.devs.contains(d)) {
        Err(CL_INVALID_DEVICE)?;
    }

    // CL_INVALID_OPERATION if the compilation or build of a program executable for any of the
    // devices listed in device_list by a previous call to clCompileProgram or clBuildProgram for
    // program has not completed.
    for d in &devs {
        if progs
            .iter()
            .map(|p| p.status(d))
            .any(|s| s != CL_BUILD_SUCCESS as cl_build_status)
        {
            Err(CL_INVALID_OPERATION)?;
        }
    }

    // CL_LINK_PROGRAM_FAILURE if there is a failure to link the compiled binaries and/or libraries.
    let res = CLProgram::link(c, &devs, progs);
    let code = if devs
        .iter()
        .map(|d| res.status(d))
        .all(|s| s == CL_BUILD_SUCCESS as cl_build_status)
    {
        CL_SUCCESS as cl_int
    } else {
        CL_LINK_PROGRAM_FAILURE
    };

    call_cb(pfn_notify, res.cl, user_data);

    Ok((res.cl, code))

    //• CL_INVALID_LINKER_OPTIONS if the linker options specified by options are invalid.
    //• CL_INVALID_OPERATION if the rules for devices containing compiled binaries or libraries as described in input_programs argument above are not followed.
}
