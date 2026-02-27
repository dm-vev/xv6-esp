#![no_std]
#![no_main]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

unsafe extern "C" {
  fn __xv6_host_printf(fmt: *const c_char, ...) -> c_int;
}

const MSG_BANNER: &[u8] = b"rust_poc: rust applet proof-of-idea\n\0";
const MSG_ARG: &[u8] = b"rust_poc: argc=%d first_arg=%s\n\0";
const MSG_NOARG: &[u8] = b"rust_poc: argc=%d\n\0";

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
  loop {}
}

#[no_mangle]
pub extern "C" fn main(argc: c_int, argv: *const *const c_char) -> c_int {
  unsafe {
    __xv6_host_printf(MSG_BANNER.as_ptr().cast());
    if argc > 1 && !argv.is_null() {
      let first_arg = *argv.add(1);
      if !first_arg.is_null() {
        __xv6_host_printf(MSG_ARG.as_ptr().cast(), argc, first_arg);
        return 0;
      }
    }
    __xv6_host_printf(MSG_NOARG.as_ptr().cast(), argc);
  }
  0
}
