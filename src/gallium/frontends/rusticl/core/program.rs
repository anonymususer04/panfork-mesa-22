extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::compiler::clc::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::ffi::CString;
use std::sync::Arc;
use std::sync::Mutex;

struct ProgramBuild {
    spirv: Option<spirv::SPIRVBin>,
    status: cl_build_status,
    options: String,
    log: String,
}

fn prepare_options(options: &String) -> Vec<CString> {
    options
        .split_whitespace()
        .map(|a| {
            if a == "-cl-denorms-are-zero" {
                "-fdenormal-fp-math=positive-zero"
            } else {
                a
            }
        })
        .map(CString::new)
        .map(Result::unwrap)
        .collect()
}

#[repr(C)]
pub struct Program {
    pub base: CLObjectBase<CL_INVALID_PROGRAM>,
    pub context: Arc<Context>,
    pub devs: Vec<Arc<Device>>,
    pub src: CString,
    pub kernels: Vec<String>,
    builds: HashMap<*const Device, Mutex<ProgramBuild>>,
}

impl_cl_type_trait!(cl_program, Program, CL_INVALID_PROGRAM);

impl Program {
    pub fn new(context: &Arc<Context>, devs: &Vec<Arc<Device>>, src: CString) -> Arc<Program> {
        let builds = devs
            .iter()
            .map(|d| {
                (
                    Arc::as_ptr(d),
                    Mutex::new(ProgramBuild {
                        spirv: None,
                        status: CL_BUILD_NONE,
                        log: String::from(""),
                        options: String::from(""),
                    }),
                )
            })
            .collect();

        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            devs: devs.clone(),
            src: src,
            kernels: Vec::new(),
            builds: builds,
        })
    }

    pub fn status(&self, dev: &Arc<Device>) -> cl_build_status {
        self.builds
            .get(&Arc::as_ptr(dev))
            .expect("")
            .lock()
            .unwrap()
            .status
    }

    pub fn log(&self, dev: &Arc<Device>) -> String {
        self.builds
            .get(&Arc::as_ptr(dev))
            .expect("")
            .lock()
            .unwrap()
            .log
            .clone()
    }

    pub fn options(&self, dev: &Arc<Device>) -> String {
        self.builds
            .get(&Arc::as_ptr(dev))
            .expect("")
            .lock()
            .unwrap()
            .options
            .clone()
    }

    pub fn compile(
        &self,
        dev: &Arc<Device>,
        options: String,
        headers: &Vec<spirv::CLCHeader>,
    ) -> bool {
        let mut d = self
            .builds
            .get(&Arc::as_ptr(dev))
            .expect("")
            .lock()
            .unwrap();
        let args = prepare_options(&options);

        let (spirv, log) = spirv::SPIRVBin::from_clc(&self.src, &args, headers);

        d.spirv = spirv;
        d.log = log;
        d.options = options;

        if d.spirv.is_some() {
            d.status = CL_BUILD_SUCCESS as cl_build_status;
            true
        } else {
            d.status = CL_BUILD_ERROR;
            false
        }
    }

    pub fn link(
        context: &Arc<Context>,
        devs: &Vec<Arc<Device>>,
        progs: &Vec<Arc<Program>>,
    ) -> Arc<Program> {
        let devs: Vec<Arc<Device>> = devs.iter().map(|d| (*d).clone()).collect();
        let mut builds = HashMap::new();
        let mut kernels = Vec::new();

        for d in &devs {
            let mut locks = Vec::new();
            for p in progs {
                locks.push(p.builds.get(&Arc::as_ptr(d)).unwrap().lock())
            }

            let bins = locks
                .iter()
                .map(|l| l.as_ref().unwrap().spirv.as_ref().unwrap())
                .collect();

            let (spirv, log) = spirv::SPIRVBin::link(&bins, false);
            let status = if let Some(spirv) = &spirv {
                kernels.append(&mut spirv.kernels());
                CL_BUILD_SUCCESS as cl_build_status
            } else {
                CL_BUILD_ERROR
            };

            builds.insert(
                Arc::as_ptr(d),
                Mutex::new(ProgramBuild {
                    spirv: spirv,
                    status: status,
                    log: log,
                    options: String::from(""),
                }),
            );
        }

        kernels.dedup();
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            devs: devs,
            src: CString::new("").unwrap(),
            kernels: kernels,
            builds: builds,
        })
    }
}
