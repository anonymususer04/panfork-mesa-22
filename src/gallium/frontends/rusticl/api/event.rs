extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::event::*;

use self::rusticl_opencl_gen::*;

use std::ptr;

impl CLInfo<cl_event_info> for cl_event {
    fn query(&self, q: cl_event_info) -> Result<Vec<u8>, cl_int> {
        let event = self.get_ref()?;
        Ok(match q {
            CL_EVENT_COMMAND_EXECUTION_STATUS => cl_prop::<cl_int>(event.status()),
            CL_EVENT_CONTEXT => cl_prop::<cl_context>(event.context.cl),
            CL_EVENT_COMMAND_QUEUE => {
                cl_prop::<cl_command_queue>(event.queue.as_ref().map_or(ptr::null_mut(), |q| q.cl))
            }
            CL_EVENT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            CL_EVENT_COMMAND_TYPE => cl_prop::<cl_command_type>(event.cmd_type),
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_user_event(context: cl_context) -> Result<cl_event, cl_int> {
    let c = context.check()?;
    Ok(cl_event::from_arc(Event::new_user(c)))
}
