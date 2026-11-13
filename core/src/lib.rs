#![no_std]
mod format;
mod model;
mod deformers;
mod ffi;
mod glue;
mod ids;
mod keyforms;
mod offscreen;
mod visuals;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
