extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::core::device::*;
use crate::decl_cl_type;
use crate::init_cl_type;

use self::mesa_rust::pipe::resource::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::convert::TryInto;
use std::os::raw::c_void;
use std::ptr;

decl_cl_type!(_cl_context, CLContext, CLContextRef, CL_INVALID_CONTEXT);

pub struct CLContext {
    pub cl: cl_context,
    pub devs: Vec<CLDeviceRef>,
    pub properties: Vec<cl_context_properties>,
}

impl CLContext {
    pub fn new(devs: Vec<CLDeviceRef>, properties: Vec<cl_context_properties>) -> CLContextRef {
        let c = Self {
            cl: ptr::null_mut(),
            devs: devs,
            properties: properties,
        };

        init_cl_type!(c, _cl_context)
    }

    pub fn create_buffer(
        &self,
        size: usize,
    ) -> Result<HashMap<cl_device_id, PipeResource>, cl_int> {
        let adj_size: u32 = size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_buffer(adj_size)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(dev.cl, resource?);
        }
        Ok(res)
    }

    pub fn create_buffer_from_user(
        &self,
        size: usize,
        user_ptr: *mut c_void,
    ) -> Result<HashMap<cl_device_id, PipeResource>, cl_int> {
        let adj_size: u32 = size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_buffer_from_user(adj_size, user_ptr)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(dev.cl, resource?);
        }
        Ok(res)
    }
}
