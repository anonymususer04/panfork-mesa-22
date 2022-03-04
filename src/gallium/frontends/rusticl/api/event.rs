extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::event::*;

use self::rusticl_opencl_gen::*;

use std::ptr;
use std::sync::Arc;

impl CLInfo<cl_event_info> for cl_event {
    fn query(&self, q: cl_event_info) -> Result<Vec<u8>, cl_int> {
        let event = self.get_ref()?;
        Ok(match q {
            CL_EVENT_COMMAND_EXECUTION_STATUS => cl_prop::<cl_int>(event.status()),
            CL_EVENT_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&event.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_EVENT_COMMAND_QUEUE => {
                let ptr = match event.queue.as_ref() {
                    // Note we use as_ptr here which doesn't increase the reference count.
                    Some(queue) => Arc::as_ptr(queue),
                    None => ptr::null_mut(),
                };
                cl_prop::<cl_command_queue>(cl_command_queue::from_ptr(ptr))
            }
            CL_EVENT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            CL_EVENT_COMMAND_TYPE => cl_prop::<cl_command_type>(event.cmd_type),
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_user_event(context: cl_context) -> Result<cl_event, cl_int> {
    let c = context.get_arc()?;
    Ok(cl_event::from_arc(Event::new_user(c)))
}
