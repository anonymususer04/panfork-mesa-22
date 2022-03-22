extern crate mesa_rust_gen;

use crate::pipe::context::*;

use self::mesa_rust_gen::*;

use std::os::raw::c_void;
use std::ptr;
use std::sync::Arc;

pub struct PipeTransfer {
    pipe: *mut pipe_transfer,
    res: *mut pipe_resource,
    ptr: *mut c_void,
    ctx: Arc<PipeContext>,
    is_buffer: bool,
}

impl PipeTransfer {
    pub(super) fn new(
        is_buffer: bool,
        pipe: *mut pipe_transfer,
        ptr: *mut c_void,
        ctx: &Arc<PipeContext>,
    ) -> Self {
        let mut res: *mut pipe_resource = ptr::null_mut();
        unsafe { pipe_resource_reference(&mut res, (*pipe).resource) }

        Self {
            pipe: pipe,
            res: res,
            ptr: ptr,
            ctx: ctx.clone(),
            is_buffer: is_buffer,
        }
    }

    pub fn ptr(&self) -> *mut c_void {
        self.ptr
    }

    pub fn row_pitch(&self) -> u32 {
        unsafe { (*self.pipe).stride }
    }

    pub fn slice_pitch(&self) -> u32 {
        unsafe { (*self.pipe).layer_stride }
    }
}

impl Drop for PipeTransfer {
    fn drop(&mut self) {
        if self.is_buffer {
            self.ctx.buffer_unmap(self.pipe);
        } else {
            self.ctx.texture_unmap(self.pipe);
        }
        unsafe { pipe_resource_reference(&mut self.res, ptr::null_mut()) }
    }
}
