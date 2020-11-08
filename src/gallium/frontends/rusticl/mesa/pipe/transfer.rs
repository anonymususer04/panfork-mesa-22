extern crate mesa_rust_gen;

use crate::pipe::context::*;

use self::mesa_rust_gen::*;

use std::os::raw::c_void;
use std::ptr;
use std::rc::Rc;

pub struct PipeTransfer {
    pipe: *mut pipe_transfer,
    res: *mut pipe_resource,
    ptr: *mut c_void,
    ctx: Rc<PipeContext>,
}

impl PipeTransfer {
    pub fn new(pipe: *mut pipe_transfer, ptr: *mut c_void, ctx: &Rc<PipeContext>) -> Self {
        let mut res: *mut pipe_resource = ptr::null_mut();
        unsafe { pipe_resource_reference(&mut res, (*pipe).resource) }

        Self {
            pipe: pipe,
            res: res,
            ptr: ptr,
            ctx: ctx.clone(),
        }
    }

    pub fn ptr(&self) -> *mut c_void {
        self.ptr
    }
}

impl Drop for PipeTransfer {
    fn drop(&mut self) {
        self.ctx.buffer_unmap(self.pipe);
        unsafe { pipe_resource_reference(&mut self.res, ptr::null_mut()) }
    }
}
