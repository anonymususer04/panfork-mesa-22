extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::core::event::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::pipe::context::*;
use self::rusticl_opencl_gen::*;

use std::rc::Rc;
use std::sync::mpsc;
use std::sync::Arc;
use std::sync::Mutex;
use std::thread;
use std::thread::JoinHandle;

#[repr(C)]
pub struct Queue {
    pub base: CLObjectBase<CL_INVALID_COMMAND_QUEUE>,
    pub context: Arc<Context>,
    pub device: CLDeviceRef,
    pub props: cl_command_queue_properties,
    pipe: Rc<PipeContext>,
    pending: Mutex<Vec<Arc<Event>>>,
    _thrd: Option<JoinHandle<()>>,
    chan_in: mpsc::Sender<Vec<Arc<Event>>>,
    chan_out: mpsc::Receiver<bool>,
}

impl_cl_type_trait!(cl_command_queue, Queue, CL_INVALID_COMMAND_QUEUE);

impl Queue {
    pub fn new(
        context: &Arc<Context>,
        device: &CLDeviceRef,
        props: cl_command_queue_properties,
    ) -> Result<Arc<Queue>, i32> {
        let (tx_q, rx_t) = mpsc::channel::<Vec<Arc<Event>>>();
        let (tx_t, rx_q) = mpsc::channel::<bool>();
        Ok(Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            device: device.clone(),
            props: props,
            // we assume that memory allocation is the only possible failure. Any other failure reason
            // should be detected earlier (e.g.: checking for CAPs).
            pipe: device
                .screen()
                .create_context()
                .ok_or(CL_OUT_OF_HOST_MEMORY)?,
            pending: Mutex::new(Vec::new()),
            _thrd: Some(
                thread::Builder::new()
                    .name("rusticl queue thread".into())
                    .spawn(move || loop {
                        let r = rx_t.recv();
                        if r.is_err() {
                            break;
                        }
                        for e in r.unwrap() {
                            e.call();
                        }
                        if tx_t.send(true).is_err() {
                            break;
                        }
                    })
                    .unwrap(),
            ),
            chan_in: tx_q,
            chan_out: rx_q,
        }))
    }

    pub fn context(&self) -> &Rc<PipeContext> {
        &self.pipe
    }

    pub fn queue(&self, e: &Arc<Event>) {
        self.pending.lock().unwrap().push(e.clone());
    }

    // TODO: implement non blocking flush
    pub fn flush(&self, _wait: bool) -> Result<(), cl_int> {
        let mut p = self.pending.lock().unwrap();
        // This should never ever error, but if it does return an error
        self.chan_in
            .send((*p).drain(0..).collect())
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        self.chan_out.recv().unwrap();
        Ok(())
    }
}

impl Drop for Queue {
    fn drop(&mut self) {
        // when deleting the application side object, we have to flush
        // From the OpenCL spec:
        // clReleaseCommandQueue performs an implicit flush to issue any previously queued OpenCL
        // commands in command_queue.
        // TODO: maybe we have to do it on every release?
        let _ = self.flush(true);
    }
}
