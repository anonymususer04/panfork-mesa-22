extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::context::*;
use crate::core::queue::*;
use crate::impl_cl_type_trait;

use self::rusticl_opencl_gen::*;

use std::convert::TryInto;
use std::slice;
use std::sync::atomic::AtomicI32;
use std::sync::atomic::Ordering;
use std::sync::Arc;

#[repr(C)]
pub struct Event {
    pub base: CLObjectBase<CL_INVALID_EVENT>,
    pub context: CLContextRef,
    pub queue: Option<Arc<Queue>>,
    pub cmd_type: cl_command_type,
    pub deps: Vec<Arc<Event>>,
    // use AtomicI32 instead of cl_int so we can change it without a &mut reference
    status: AtomicI32,
    work: Option<Box<dyn Fn(&Arc<Queue>) -> Result<(), cl_int>>>,
}

impl_cl_type_trait!(cl_event, Event, CL_INVALID_EVENT);

// TODO shouldn't be needed, but... uff C pointers are annoying
unsafe impl Send for Event {}
unsafe impl Sync for Event {}

impl Event {
    pub fn new(
        queue: &Arc<Queue>,
        cmd_type: cl_command_type,
        deps: Vec<Arc<Event>>,
        work: Box<dyn Fn(&Arc<Queue>) -> Result<(), cl_int>>,
    ) -> Arc<Event> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: queue.context.clone(),
            queue: Some(queue.clone()),
            cmd_type: cmd_type,
            deps: deps,
            status: AtomicI32::new(CL_QUEUED as cl_int),
            work: Some(work),
        })
    }

    pub fn new_user(context: &CLContextRef) -> Arc<Event> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            queue: None,
            cmd_type: CL_COMMAND_USER,
            deps: Vec::new(),
            status: AtomicI32::new(CL_SUBMITTED as cl_int),
            work: None,
        })
    }

    pub fn from_cl_arr(events: *const cl_event, num_events: u32) -> Result<Vec<Arc<Event>>, i32> {
        let c = num_events.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let s = unsafe { slice::from_raw_parts(events, c) };

        let mut v = Vec::new();
        for e in s {
            v.push(e.get_arc()?);
        }

        Ok(v)
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
