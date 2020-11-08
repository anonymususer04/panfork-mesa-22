extern crate rusticl_opencl_gen;

use crate::api::util::*;
use crate::core::event::*;

use self::rusticl_opencl_gen::*;

use std::ptr;

impl CLInfo<cl_event_info> for crate::core::event::_cl_event {
    fn query(&self, q: cl_event_info) -> Result<Vec<u8>, cl_int> {
        Ok(match q {
            CL_EVENT_COMMAND_EXECUTION_STATUS => cl_prop::<cl_int>(self.status()),
            CL_EVENT_CONTEXT => cl_prop::<cl_context>(self.context.cl),
            CL_EVENT_COMMAND_QUEUE => {
                cl_prop::<cl_command_queue>(self.queue.as_ref().map_or(ptr::null_mut(), |q| q.cl))
            }
            CL_EVENT_COMMAND_TYPE => cl_prop::<cl_command_type>(self.cmd_type),
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_user_event(context: cl_context) -> Result<cl_event, cl_int> {
    let c = context.check()?;
    Ok(CLEvent::new_user(c).cl)
}
