extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::core::context::*;
use crate::core::device::*;

use crate::decl_cl_type;
use crate::init_cl_type;

use self::mesa_rust::compiler::clc::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::ffi::CString;
use std::ptr;
use std::sync::Mutex;

decl_cl_type!(_cl_program, CLProgram, CLProgramRef, CL_INVALID_PROGRAM);

struct CLProgramBuild {
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

pub struct CLProgram {
    pub cl: cl_program,
    pub context: CLContextRef,
    pub devs: Vec<CLDeviceRef>,
    pub src: CString,
    pub kernels: Vec<String>,
    builds: HashMap<CLDeviceRef, Mutex<CLProgramBuild>>,
}

impl CLProgram {
    pub fn new(context: &CLContextRef, devs: &Vec<CLDeviceRef>, src: CString) -> CLProgramRef {
        let builds = devs
            .iter()
            .map(|d| {
                (
                    d.clone(),
                    Mutex::new(CLProgramBuild {
                        spirv: None,
                        status: CL_BUILD_NONE,
                        log: String::from(""),
                        options: String::from(""),
                    }),
                )
            })
            .collect();

        let c = Self {
            cl: ptr::null_mut(),
            context: context.clone(),
            devs: devs.clone(),
            src: src,
            kernels: Vec::new(),
            builds: builds,
        };

        init_cl_type!(c, _cl_program)
    }

    pub fn status(&self, dev: &CLDeviceRef) -> cl_build_status {
        self.builds.get(dev).expect("").lock().unwrap().status
    }

    pub fn log(&self, dev: &CLDeviceRef) -> String {
        self.builds.get(dev).expect("").lock().unwrap().log.clone()
    }

    pub fn options(&self, dev: &CLDeviceRef) -> String {
        self.builds
            .get(dev)
            .expect("")
            .lock()
            .unwrap()
            .options
            .clone()
    }

    pub fn compile(
        &self,
        dev: &CLDeviceRef,
        options: String,
        headers: &Vec<spirv::CLCHeader>,
    ) -> bool {
        let mut d = self.builds.get(dev).expect("").lock().unwrap();
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
        context: &CLContextRef,
        devs: &Vec<&CLDeviceRef>,
        progs: Vec<&mut CLProgramRef>,
    ) -> CLProgramRef {
        let devs: Vec<CLDeviceRef> = devs.iter().map(|d| (*d).clone()).collect();
        let mut builds = HashMap::new();
        let mut kernels = Vec::new();

        for d in &devs {
            let mut locks = Vec::new();
            for p in &progs {
                locks.push(p.builds.get(d).unwrap().lock())
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
                d.clone(),
                Mutex::new(CLProgramBuild {
                    spirv: spirv,
                    status: status,
                    log: log,
                    options: String::from(""),
                }),
            );
        }

        kernels.dedup();
        let c = Self {
            cl: ptr::null_mut(),
            context: context.clone(),
            devs: devs,
            src: CString::new("").unwrap(),
            kernels: kernels,
            builds: builds,
        };

        init_cl_type!(c, _cl_program)
    }
}
