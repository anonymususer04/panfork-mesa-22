extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::util::*;
use crate::core::context::*;
use crate::core::queue::*;
use crate::decl_cl_type;
use crate::init_cl_type;

use self::rusticl_opencl_gen::*;

use std::convert::TryInto;
use std::ptr;
use std::slice;
use std::sync::atomic::AtomicI32;
use std::sync::atomic::Ordering;

decl_cl_type!(_cl_event, CLEvent, CLEventRef, CL_INVALID_EVENT);

pub struct CLEvent {
    pub cl: cl_event,
    pub context: CLContextRef,
    pub queue: Option<CLQueueRef>,
    pub cmd_type: cl_command_type,
    pub deps: Vec<CLEventRef>,
    // use AtomicI32 instead of cl_int so we can change it without a &mut reference
    status: AtomicI32,
    work: Option<Box<dyn Fn(&CLQueueRef) -> Result<(), cl_int>>>,
}

// TODO shouldn't be needed, but... uff C pointers are annoying
unsafe impl Send for CLEvent {}
unsafe impl Sync for CLEvent {}

impl CLEvent {
    pub fn new(
        queue: &CLQueueRef,
        cmd_type: cl_command_type,
        deps: Vec<&CLEventRef>,
        work: Box<dyn Fn(&CLQueueRef) -> Result<(), cl_int>>,
    ) -> CLEventRef {
        let q = Self {
            cl: ptr::null_mut(),
            context: queue.context.clone(),
            queue: Some(queue.clone()),
            cmd_type: cmd_type,
            deps: deps.into_iter().map(|e| e.clone()).collect(),
            status: AtomicI32::new(CL_QUEUED as cl_int),
            work: Some(work),
        };

        init_cl_type!(q, _cl_event)
    }

    pub fn new_user(context: &CLContextRef) -> CLEventRef {
        let q = Self {
            cl: ptr::null_mut(),
            context: context.clone(),
            queue: None,
            cmd_type: CL_COMMAND_USER,
            deps: Vec::new(),
            status: AtomicI32::new(CL_SUBMITTED as cl_int),
            work: None,
        };

        init_cl_type!(q, _cl_event)
    }

    pub fn from_raw(
        events: *const cl_event,
        num_events: cl_uint,
    ) -> Result<Vec<&'static CLEventRef>, cl_int> {
        let c = num_events.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let s = unsafe { slice::from_raw_parts(events, c) };

        s.into_iter().map(cl_event::check).collect()
    }

    pub fn is_error(&self) -> bool {
        self.status.load(Ordering::Relaxed) < 0
    }

    pub fn status(&self) -> cl_int {
        self.status.load(Ordering::Relaxed)
    }

    // We always assume that work here simply submits stuff to the hardware even if it's just doing
    // sw emulation or nothing at all.
    // If anything requets waiting, we will update the status through fencing later.
    pub fn call(&self) -> cl_int {
        let status = self.status();
        if status == CL_QUEUED as i32 {
            let new = self.work.as_ref().map_or(
                // if there is no work
                CL_SUBMITTED as i32,
                |w| {
                    w(&self.queue.as_ref().unwrap()).err().map_or(
                        // if there is an error, negate it
                        CL_SUBMITTED as i32,
                        |e| e,
                    )
                },
            );
            self.status.store(new, Ordering::Relaxed);
            new
        } else {
            status
        }
    }
}

// TODO worker thread per device
// Condvar to wait on new events to work on
// notify condvar when flushing queue events to worker
// attach fence to flushed events on context->flush
// store "newest" event for in-order queues per queue
// reordering/graph building done in worker
