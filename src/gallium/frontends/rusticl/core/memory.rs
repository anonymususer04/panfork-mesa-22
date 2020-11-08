extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::types::*;
use crate::api::util::*;
use crate::core::context::*;
use crate::core::queue::*;
use crate::decl_cl_type;
use crate::init_cl_type;

use self::mesa_rust::pipe::resource::*;
use self::mesa_rust::pipe::transfer::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::convert::TryInto;
use std::os::raw::c_void;
use std::ptr;
use std::sync::Mutex;

decl_cl_type!(_cl_mem, CLMem, CLMemRef, CL_INVALID_MEM_OBJECT);
decl_cl_type!(_cl_sampler, CLSampler, CLSamplerRef, CL_INVALID_SAMPLER);

pub struct CLMem {
    pub cl: cl_mem,
    pub context: CLContextRef,
    pub parent: Option<CLMemRef>,
    pub mem_type: cl_mem_object_type,
    pub flags: cl_mem_flags,
    pub size: usize,
    pub offset: usize,
    pub host_ptr: *mut c_void,
    pub image_format: cl_image_format,
    pub image_desc: cl_image_desc,
    pub image_elem_size: u8,
    pub cbs: Mutex<Vec<Box<dyn Fn(cl_mem) -> ()>>>,
    res: Option<HashMap<cl_device_id, PipeResource>>,
    maps: Mutex<HashMap<*mut c_void, PipeTransfer>>,
}

fn sw_copy(
    src: *const c_void,
    dst: *mut c_void,
    region: &CLVec<usize>,
    src_origin: &CLVec<usize>,
    src_row_pitch: usize,
    src_slice_pitch: usize,
    dst_origin: &CLVec<usize>,
    dst_row_pitch: usize,
    dst_slice_pitch: usize,
) {
    for z in 0..region[2] {
        for y in 0..region[1] {
            unsafe {
                ptr::copy_nonoverlapping(
                    src.add((*src_origin + [0, y, z]) * [1, src_row_pitch, src_slice_pitch]),
                    dst.add((*dst_origin + [0, y, z]) * [1, dst_row_pitch, dst_slice_pitch]),
                    region[0],
                )
            };
        }
    }
}

impl CLMem {
    pub fn new_buffer(
        context: &CLContextRef,
        flags: cl_mem_flags,
        size: usize,
        host_ptr: *mut c_void,
    ) -> Result<CLMemRef, cl_int> {
        if bit_check(flags, CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR) {
            println!("host ptr semantics not implemented!");
        }

        let buffer = if bit_check(flags, CL_MEM_USE_HOST_PTR) {
            context.create_buffer_from_user(size, host_ptr)
        } else {
            context.create_buffer(size)
        }?;

        let m = Self {
            cl: ptr::null_mut(),
            context: context.clone(),
            parent: None,
            mem_type: CL_MEM_OBJECT_BUFFER,
            flags: flags,
            size: size,
            offset: 0,
            host_ptr: host_ptr,
            image_format: cl_image_format::default(),
            image_desc: cl_image_desc::default(),
            image_elem_size: 0,
            cbs: Mutex::new(Vec::new()),
            res: Some(buffer),
            maps: Mutex::new(HashMap::new()),
        };

        Ok(init_cl_type!(m, _cl_mem))
    }

    pub fn new_sub_buffer(
        parent: &CLMemRef,
        flags: cl_mem_flags,
        offset: usize,
        size: usize,
    ) -> CLMemRef {
        let m = Self {
            cl: ptr::null_mut(),
            context: parent.context.clone(),
            parent: Some(parent.clone()),
            mem_type: CL_MEM_OBJECT_BUFFER,
            flags: flags,
            size: size,
            offset: offset,
            host_ptr: unsafe { parent.host_ptr.offset(offset as isize) },
            image_format: cl_image_format::default(),
            image_desc: cl_image_desc::default(),
            image_elem_size: 0,
            cbs: Mutex::new(Vec::new()),
            res: None,
            maps: Mutex::new(HashMap::new()),
        };

        init_cl_type!(m, _cl_mem)
    }

    pub fn new_image(
        context: &CLContextRef,
        mem_type: cl_mem_object_type,
        flags: cl_mem_flags,
        image_format: &cl_image_format,
        image_desc: cl_image_desc,
        image_elem_size: u8,
        host_ptr: *mut c_void,
    ) -> CLMemRef {
        if bit_check(
            flags,
            CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR,
        ) {
            println!("host ptr semantics not implemented!");
        }

        let m = Self {
            cl: ptr::null_mut(),
            context: context.clone(),
            parent: None,
            mem_type: mem_type,
            flags: flags,
            size: 0,
            offset: 0,
            host_ptr: host_ptr,
            image_format: *image_format,
            image_desc: image_desc,
            image_elem_size: image_elem_size,
            cbs: Mutex::new(Vec::new()),
            res: None,
            maps: Mutex::new(HashMap::new()),
        };

        init_cl_type!(m, _cl_mem)
    }

    pub fn has_same_parent(&self, other: &Self) -> bool {
        let a = self.parent.as_ref().map_or(self, |p| &p);
        let b = other.parent.as_ref().map_or(other, |p| &p);
        a == b
    }

    fn get_res(&self) -> &HashMap<cl_device_id, PipeResource> {
        self.parent
            .as_ref()
            .map_or(self, |p| p.as_ref())
            .res
            .as_ref()
            .unwrap()
    }

    pub fn write_from_user(
        &self,
        q: &CLQueueRef,
        offset: usize,
        ptr: *const c_void,
        size: usize,
    ) -> Result<(), cl_int> {
        // TODO support sub buffers
        let r = self.get_res().get(&q.device.cl).unwrap();
        q.context().buffer_subdata(
            r,
            offset.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?,
            ptr,
            size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?,
        );
        Ok(())
    }

    pub fn write_from_user_rect(
        &self,
        src: *const c_void,
        q: &CLQueueRef,
        region: &CLVec<usize>,
        src_origin: &CLVec<usize>,
        src_row_pitch: usize,
        src_slice_pitch: usize,
        dst_origin: &CLVec<usize>,
        dst_row_pitch: usize,
        dst_slice_pitch: usize,
    ) -> Result<(), cl_int> {
        let r = self.res.as_ref().unwrap().get(&q.device.cl).unwrap();
        let tx = q.context().buffer_map(r, 0, self.size.try_into().unwrap());

        sw_copy(
            src,
            tx.ptr(),
            region,
            src_origin,
            src_row_pitch,
            src_slice_pitch,
            dst_origin,
            dst_row_pitch,
            dst_slice_pitch,
        );

        drop(tx);
        Ok(())
    }

    pub fn read_to_user_rect(
        &self,
        dst: *mut c_void,
        q: &CLQueueRef,
        region: &CLVec<usize>,
        src_origin: &CLVec<usize>,
        src_row_pitch: usize,
        src_slice_pitch: usize,
        dst_origin: &CLVec<usize>,
        dst_row_pitch: usize,
        dst_slice_pitch: usize,
    ) -> Result<(), cl_int> {
        let r = self.res.as_ref().unwrap().get(&q.device.cl).unwrap();
        let tx = q.context().buffer_map(r, 0, self.size.try_into().unwrap());

        sw_copy(
            tx.ptr(),
            dst,
            region,
            src_origin,
            src_row_pitch,
            src_slice_pitch,
            dst_origin,
            dst_row_pitch,
            dst_slice_pitch,
        );

        drop(tx);
        Ok(())
    }

    pub fn copy_to(
        &self,
        dst: &Self,
        q: &CLQueueRef,
        region: &CLVec<usize>,
        src_origin: &CLVec<usize>,
        src_row_pitch: usize,
        src_slice_pitch: usize,
        dst_origin: &CLVec<usize>,
        dst_row_pitch: usize,
        dst_slice_pitch: usize,
    ) -> Result<(), cl_int> {
        let res_src = self.res.as_ref().unwrap().get(&q.device.cl).unwrap();
        let res_dst = dst.res.as_ref().unwrap().get(&q.device.cl).unwrap();

        let tx_src = q
            .context()
            .buffer_map(res_src, 0, self.size.try_into().unwrap());
        let tx_dst = q
            .context()
            .buffer_map(res_dst, 0, dst.size.try_into().unwrap());

        // TODO check to use hw accelerated paths (e.g. resource_copy_region or blits)
        sw_copy(
            tx_src.ptr(),
            tx_dst.ptr(),
            region,
            src_origin,
            src_row_pitch,
            src_slice_pitch,
            dst_origin,
            dst_row_pitch,
            dst_slice_pitch,
        );

        drop(tx_src);
        drop(tx_dst);

        Ok(())
    }

    // TODO use PIPE_MAP_UNSYNCHRONIZED for non blocking
    pub fn map(&self, q: &CLQueueRef, offset: usize, size: usize) -> *mut c_void {
        let res = self.res.as_ref().unwrap().get(&q.device.cl).unwrap();
        let tx = q
            .context()
            .buffer_map(res, offset.try_into().unwrap(), size.try_into().unwrap());
        let ptr = tx.ptr();

        self.maps.lock().unwrap().insert(tx.ptr(), tx);

        ptr
    }

    pub fn unmap(&self, ptr: *mut c_void) -> bool {
        self.maps.lock().unwrap().remove(&ptr).is_some()
    }
}

impl Drop for CLMem {
    fn drop(&mut self) {
        let cl = self.cl;
        self.cbs
            .get_mut()
            .unwrap()
            .iter()
            .rev()
            .for_each(|cb| cb(cl));
    }
}

pub struct CLSampler {
    pub cl: cl_sampler,
    pub context: CLContextRef,
    pub normalized_coords: bool,
    pub addressing_mode: cl_addressing_mode,
    pub filter_mode: cl_filter_mode,
}

impl CLSampler {
    pub fn new(
        context: &CLContextRef,
        normalized_coords: bool,
        addressing_mode: cl_addressing_mode,
        filter_mode: cl_filter_mode,
    ) -> CLSamplerRef {
        let s = Self {
            cl: ptr::null_mut(),
            context: context.clone(),
            normalized_coords: normalized_coords,
            addressing_mode: addressing_mode,
            filter_mode: filter_mode,
        };

        init_cl_type!(s, _cl_sampler)
    }
}
