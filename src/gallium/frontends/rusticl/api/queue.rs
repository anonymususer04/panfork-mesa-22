extern crate rusticl_opencl_gen;

use crate::api::util::*;
use crate::core::queue::*;

use self::rusticl_opencl_gen::*;

impl CLInfo<cl_command_queue_info> for crate::core::queue::_cl_command_queue {
    fn query(&self, q: cl_command_queue_info) -> Result<Vec<u8>, cl_int> {
        Ok(match q {
            CL_QUEUE_CONTEXT => cl_prop::<cl_context>(self.context.cl),
            CL_QUEUE_DEVICE => cl_prop::<cl_device_id>(self.device.cl),
            CL_QUEUE_PROPERTIES => cl_prop::<cl_command_queue_properties>(self.props),
            CL_QUEUE_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refs()),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

fn valid_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags =
        cl_bitfield::from(CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE);
    return properties & !valid_flags == 0;
}

fn supported_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags = cl_bitfield::from(CL_QUEUE_PROFILING_ENABLE);
    return properties & !valid_flags == 0;
}

pub fn create_command_queue(
    context: cl_context,
    device: cl_device_id,
    properties: cl_command_queue_properties,
) -> Result<cl_command_queue, cl_int> {
    // CL_INVALID_CONTEXT if context is not a valid context.
    let c = context.check()?;

    // CL_INVALID_DEVICE if device is not a valid device
    let d = device.check()?;

    // ... or is not associated with context.
    if !c.devs.contains(&d) {
        return Err(CL_INVALID_DEVICE);
    }

    // CL_INVALID_VALUE if values specified in properties are not valid.
    if !valid_command_queue_properties(properties) {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_QUEUE_PROPERTIES if values specified in properties are valid but are not supported by the device.
    if !supported_command_queue_properties(properties) {
        return Err(CL_INVALID_QUEUE_PROPERTIES);
    }

    Ok(CLQueue::new(c, d, properties)?.cl)
}

pub fn finish_queue(command_queue: cl_command_queue) -> Result<(), cl_int> {
    // CL_INVALID_COMMAND_QUEUE if command_queue is not a valid host command-queue.
    command_queue.check()?;

    Ok(())
}
