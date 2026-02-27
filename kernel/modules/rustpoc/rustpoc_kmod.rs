#![no_std]
#![no_main]

use core::ffi::{c_char, c_int, c_void};
use core::panic::PanicInfo;

const KMOD_MODULE_ABI_VER: u32 = 1;
const XV6_MODULE_SYMBOL_EXTENSION: c_int = 0;
const RUSTPOC_PRIORITY: c_int = 35;

unsafe extern "C" {
  fn __xv6_host_printf(fmt: *const c_char, ...) -> c_int;
}

#[repr(C)]
pub struct Xv6ModuleSymbol {
  name: *const c_char,
  addr: *mut c_void,
  kind: c_int,
  priority: c_int,
}
unsafe impl Sync for Xv6ModuleSymbol {}

#[repr(C)]
pub struct Xv6ModuleDesc {
  abi_ver: u32,
  name: *const c_char,
  default_priority: c_int,
  symbols: *const Xv6ModuleSymbol,
  symbol_count: c_int,
}
unsafe impl Sync for Xv6ModuleDesc {}

const MOD_NAME: &[u8] = b"rustpoc\0";
const SYM_NAME: &[u8] = b"rustpoc_magic\0";
const MSG_INIT: &[u8] = b"rustpoc: init\n\0";
const MSG_FINI: &[u8] = b"rustpoc: fini\n\0";

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
  loop {}
}

#[no_mangle]
pub extern "C" fn rustpoc_magic(value: c_int) -> c_int {
  value ^ 0x5a5a
}

static RUSTPOC_SYMBOLS: [Xv6ModuleSymbol; 1] = [Xv6ModuleSymbol {
  name: SYM_NAME.as_ptr().cast(),
  addr: rustpoc_magic as *const () as *mut c_void,
  kind: XV6_MODULE_SYMBOL_EXTENSION,
  priority: RUSTPOC_PRIORITY,
}];

static RUSTPOC_DESC: Xv6ModuleDesc = Xv6ModuleDesc {
  abi_ver: KMOD_MODULE_ABI_VER,
  name: MOD_NAME.as_ptr().cast(),
  default_priority: RUSTPOC_PRIORITY,
  symbols: RUSTPOC_SYMBOLS.as_ptr(),
  symbol_count: RUSTPOC_SYMBOLS.len() as c_int,
};

#[no_mangle]
pub extern "C" fn xv6_module_init() -> c_int {
  unsafe {
    __xv6_host_printf(MSG_INIT.as_ptr().cast());
  }
  0
}

#[no_mangle]
pub extern "C" fn xv6_module_fini() -> c_int {
  unsafe {
    __xv6_host_printf(MSG_FINI.as_ptr().cast());
  }
  0
}

#[no_mangle]
pub extern "C" fn xv6_module_describe() -> *const Xv6ModuleDesc {
  &RUSTPOC_DESC
}
