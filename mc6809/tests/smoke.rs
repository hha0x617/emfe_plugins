// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Integration smoke test for emfe_plugin_mc6809 via the extern "C" API.
// MC6850 ACIA register layout (at ConsoleBase, default $FF00):
//   base+0: CR (write) / SR (read)
//   base+1: TDR (write) / RDR (read)

use emfe_plugin_mc6809::*;
use std::ffi::{c_char, c_void};
use std::ptr;

const ACIA_SR: u64 = 0xFF00;
const ACIA_CR: u64 = 0xFF00;
const ACIA_RDR: u64 = 0xFF01;
const ACIA_TDR: u64 = 0xFF01;

#[test]
fn negotiate_ok() {
    let info = EmfeNegotiateInfo {
        api_version_major: EMFE_API_VERSION_MAJOR,
        api_version_minor: EMFE_API_VERSION_MINOR,
        flags: 0,
    };
    assert_eq!(emfe_negotiate(&info), EmfeResult::Ok);
}

#[test]
fn negotiate_wrong_major() {
    let info = EmfeNegotiateInfo {
        api_version_major: 99,
        api_version_minor: 0,
        flags: 0,
    };
    assert_eq!(emfe_negotiate(&info), EmfeResult::ErrUnsupported);
}

#[test]
fn board_info_has_mc6809() {
    let mut info = EmfeBoardInfo {
        board_name: ptr::null(),
        cpu_name: ptr::null(),
        description: ptr::null(),
        version: ptr::null(),
        capabilities: 0,
    };
    assert_eq!(emfe_get_board_info(&mut info), EmfeResult::Ok);
    let name = unsafe { std::ffi::CStr::from_ptr(info.board_name) }
        .to_string_lossy()
        .into_owned();
    assert_eq!(name, "MC6809");
}

#[test]
fn create_and_destroy() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    assert!(!h.is_null());
    assert_eq!(emfe_destroy(h), EmfeResult::Ok);
}

#[test]
fn step_nop() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);

    unsafe {
        // MC6809 NOP = $12
        assert_eq!(emfe_poke_byte(h, 0x0100, 0x12), EmfeResult::Ok);
        let mut v = EmfeRegValue {
            reg_id: 7, // RegId::PC
            value: EmfeRegValueUnion { u64_: 0x0100 },
        };
        assert_eq!(emfe_set_registers(h, &v, 1), EmfeResult::Ok);
        assert_eq!(emfe_step(h), EmfeResult::Ok);
        assert_eq!(emfe_get_registers(h, &mut v, 1), EmfeResult::Ok);
        assert_eq!(v.value.u64_, 0x0101);
        assert_eq!(emfe_get_instruction_count(h), 1);
        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn memory_peek_poke_word_bigendian() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        assert_eq!(emfe_poke_word(h, 0x1000, 0x1234), EmfeResult::Ok);
        assert_eq!(emfe_peek_byte(h, 0x1000), 0x12);
        assert_eq!(emfe_peek_byte(h, 0x1001), 0x34);
        assert_eq!(emfe_peek_word(h, 0x1000), 0x1234);
        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

// Helper: run the typical MC6850 init sequence then leave CPU at a NOP to
// consume cycles while the test driver pokes at registers.
unsafe fn init_acia_and_prep_cpu(h: EmfeInstance) {
    // Master reset (CR = $03), then configure 8N1 / divide-16 / no IRQs (CR = $15).
    // Program @ $0100:
    //   LDA #$03 / STA $FF00   (master reset)
    //   LDA #$15 / STA $FF00   (8N1)
    let code: &[(u64, u8)] = &[
        (0x0100, 0x86),
        (0x0101, 0x03),
        (0x0102, 0xB7),
        (0x0103, 0xFF),
        (0x0104, 0x00),
        (0x0105, 0x86),
        (0x0106, 0x15),
        (0x0107, 0xB7),
        (0x0108, 0xFF),
        (0x0109, 0x00),
    ];
    for (addr, v) in code {
        assert_eq!(emfe_poke_byte(h, *addr, *v), EmfeResult::Ok);
    }
    let pc = EmfeRegValue {
        reg_id: 7,
        value: EmfeRegValueUnion { u64_: 0x0100 },
    };
    assert_eq!(emfe_set_registers(h, &pc, 1), EmfeResult::Ok);
    for _ in 0..4 {
        assert_eq!(emfe_step(h), EmfeResult::Ok);
    }
}

// Serialize tests that share the global UART_BUF + global data_dir setting —
// those pieces of plugin state are process-wide, so two tests running in
// parallel would clobber each other's expectations.
static TEST_SERIAL: std::sync::Mutex<()> = std::sync::Mutex::new(());

static mut UART_BUF: Vec<u8> = Vec::new();
extern "C" fn tx_cb(_user: *mut c_void, ch: c_char) {
    unsafe {
        UART_BUF.push(ch as u8);
    }
}

#[test]
fn acia_tx_via_tdr_after_master_reset() {
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );

        init_acia_and_prep_cpu(h);

        // Now: LDA #'H' / STA $FF01  (TDR write)
        let code: &[(u64, u8)] = &[
            (0x010A, 0x86),
            (0x010B, 0x48), // LDA #'H'
            (0x010C, 0xB7),
            (0x010D, 0xFF),
            (0x010E, 0x01), // STA $FF01
        ];
        for (a, v) in code {
            assert_eq!(emfe_poke_byte(h, *a, *v), EmfeResult::Ok);
        }

        assert_eq!(emfe_step(h), EmfeResult::Ok); // LDA
        assert_eq!(emfe_step(h), EmfeResult::Ok); // STA (TDR write)

        assert!(
            UART_BUF.contains(&b'H'),
            "ACIA TDR write should fire TX callback"
        );
        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn acia_rx_ready_status_and_rdr_read() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        init_acia_and_prep_cpu(h);
        assert_eq!(emfe_send_char(h, b'K' as c_char), EmfeResult::Ok);

        // Program: LDA $FF00 (SR) — should have bit0 (RDRF) set.
        //          LDA $FF01 (RDR) — should pop 'K'.
        let code: &[(u64, u8)] = &[
            (0x010A, 0xB6),
            (0x010B, 0xFF),
            (0x010C, 0x00), // LDA $FF00 (SR)
            (0x010D, 0xB6),
            (0x010E, 0xFF),
            (0x010F, 0x01), // LDA $FF01 (RDR)
        ];
        for (a, v) in code {
            assert_eq!(emfe_poke_byte(h, *a, *v), EmfeResult::Ok);
        }

        assert_eq!(emfe_step(h), EmfeResult::Ok); // LDA SR
        let mut a = EmfeRegValue {
            reg_id: 0,
            value: EmfeRegValueUnion { u64_: 0 },
        };
        assert_eq!(emfe_get_registers(h, &mut a, 1), EmfeResult::Ok);
        assert_eq!(a.value.u64_ & 0x01, 0x01, "RDRF bit should be set");
        // TDRE should also be set (ACIA ready to transmit after config).
        assert_eq!(
            a.value.u64_ & 0x02,
            0x02,
            "TDRE bit should be set after init"
        );

        assert_eq!(emfe_step(h), EmfeResult::Ok); // LDA RDR
        assert_eq!(emfe_get_registers(h, &mut a, 1), EmfeResult::Ok);
        assert_eq!(
            a.value.u64_ as u8, b'K',
            "RDR should deliver the queued byte"
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn acia_master_reset_clears_rdrf() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        init_acia_and_prep_cpu(h);
        assert_eq!(emfe_send_char(h, b'X' as c_char), EmfeResult::Ok);

        // Master reset: LDA #$03 / STA $FF00
        let code: &[(u64, u8)] = &[
            (0x010A, 0x86),
            (0x010B, 0x03),
            (0x010C, 0xB7),
            (0x010D, 0xFF),
            (0x010E, 0x00),
            // Read SR after master reset: LDA $FF00
            (0x010F, 0xB6),
            (0x0110, 0xFF),
            (0x0111, 0x00),
        ];
        for (a, v) in code {
            assert_eq!(emfe_poke_byte(h, *a, *v), EmfeResult::Ok);
        }
        assert_eq!(emfe_step(h), EmfeResult::Ok); // LDA #$03
        assert_eq!(emfe_step(h), EmfeResult::Ok); // STA -> master reset
        assert_eq!(emfe_step(h), EmfeResult::Ok); // LDA SR
        let mut a = EmfeRegValue {
            reg_id: 0,
            value: EmfeRegValueUnion { u64_: 0 },
        };
        assert_eq!(emfe_get_registers(h, &mut a, 1), EmfeResult::Ok);
        assert_eq!(
            a.value.u64_ & 0x03,
            0x00,
            "RDRF and TDRE should be clear while in master reset"
        );
        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

// ---------------------------------------------------------------------------
// End-to-end: load examples/hello/hello.s19 via emfe_load_srec, run it, and
// verify UART produces "Hello, MC6809!\r\n". Skipped unless the env var
// EMFE_MC6809_EXAMPLES_DIR is set to the examples/ directory.
// ---------------------------------------------------------------------------

static mut HELLO_BUF: Vec<u8> = Vec::new();
extern "C" fn hello_tx_cb(_user: *mut c_void, ch: c_char) {
    unsafe {
        HELLO_BUF.push(ch as u8);
    }
}

#[test]
fn hello_srec_end_to_end() {
    let dir = match std::env::var("EMFE_MC6809_EXAMPLES_DIR") {
        Ok(d) => d,
        Err(_) => {
            eprintln!("SKIP: EMFE_MC6809_EXAMPLES_DIR not set");
            return;
        }
    };
    let path = std::path::PathBuf::from(dir)
        .join("hello")
        .join("hello.s19");
    let path_c = std::ffi::CString::new(path.to_string_lossy().into_owned()).unwrap();

    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        HELLO_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(hello_tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );

        assert_eq!(emfe_load_srec(h, path_c.as_ptr()), EmfeResult::Ok);

        // The program ends with SWI. Step many times and check the buffer.
        // 16 bytes of text × ~15 cycles each + init ~40 instructions. Loop
        // until we see the full text or hit the step limit.
        for _ in 0..5_000 {
            if HELLO_BUF.ends_with(b"\r\n") {
                break;
            }
            assert_eq!(emfe_step(h), EmfeResult::Ok);
        }

        let got = std::string::String::from_utf8_lossy(&HELLO_BUF).into_owned();
        assert!(
            got.contains("Hello, MC6809!\r\n"),
            "expected \"Hello, MC6809!\\r\\n\" in UART output, got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn settings_default_and_set() {
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        let key = std::ffi::CString::new("BoardType").unwrap();
        let val_ptr = emfe_get_setting(h, key.as_ptr());
        let val = std::ffi::CStr::from_ptr(val_ptr)
            .to_string_lossy()
            .into_owned();
        assert_eq!(val, "Generic");
        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn echo_s19_prints_banner() {
    // Load the real echo.s19 sample and run enough instructions for the
    // banner to reach the tx callback. Reproduces the user-reported bug
    // where no output appears.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );

        let path = std::ffi::CString::new("examples/echo/echo.s19").unwrap();
        let r = emfe_load_srec(h, path.as_ptr());
        assert_eq!(r, EmfeResult::Ok, "load_srec failed");

        // PC should be at $0100 after S9 record.
        let mut pc = EmfeRegValue {
            reg_id: 7, // RegId::PC
            value: EmfeRegValueUnion { u64_: 0 },
        };
        assert_eq!(emfe_get_registers(h, &mut pc, 1), EmfeResult::Ok);
        assert_eq!(pc.value.u64_, 0x0100, "PC should be set to S9 entry $0100");

        // Single-step for many instructions and watch for "MC6809" in TX output.
        for _ in 0..2000 {
            if emfe_step(h) != EmfeResult::Ok {
                break;
            }
            if UART_BUF.len() >= 6 {
                break;
            }
        }

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.starts_with("MC6809"),
            "Expected banner to start with 'MC6809', got {:?} (len={})",
            got,
            UART_BUF.len()
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn hello_s19_prints_once_and_halts() {
    // hello.s19 now halts with BRA * after printing "Hello, MC6809!\r\n".
    // Regression: earlier SWI-based halt walked through uninitialized SWI
    // vector and restarted the program, producing endless output.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );

        let path = std::ffi::CString::new("examples/hello/hello.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);

        assert_eq!(emfe_run(h), EmfeResult::Ok);
        // Give the CPU plenty of time; output is bounded (16 bytes).
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert_eq!(
            got,
            "Hello, MC6809!\r\n",
            "hello.s19 must print the message exactly once; got {:?} (len={})",
            got,
            UART_BUF.len()
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn echo_s19_via_run_produces_banner() {
    // Exercise the worker-thread emfe_run path end-to-end (not single-step).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );

        let path = std::ffi::CString::new("examples/echo/echo.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);

        assert_eq!(emfe_run(h), EmfeResult::Ok);
        // Wait up to ~500 ms for the banner to appear in UART_BUF.
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 6 {
                break;
            }
        }
        assert_eq!(emfe_stop(h), EmfeResult::Ok);
        // Join the worker thread via destroy.

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.starts_with("MC6809"),
            "emfe_run path should produce banner; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn step_over_skips_bsr_subroutine() {
    // Program:
    //   $0100: BSR +$02      ; call subroutine at $0104
    //   $0102: NOP           ; return target — step_over should land here
    //   $0103: NOP
    //   $0104: RTS           ; subroutine body (just returns)
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        let code: &[(u64, u8)] = &[
            (0x0100, 0x8D),
            (0x0101, 0x02), // BSR $0104
            (0x0102, 0x12), // NOP
            (0x0103, 0x12), // NOP
            (0x0104, 0x39), // RTS
        ];
        for (a, v) in code {
            assert_eq!(emfe_poke_byte(h, *a, *v), EmfeResult::Ok);
        }

        let pc = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0x0100 },
        };
        assert_eq!(emfe_set_registers(h, &pc, 1), EmfeResult::Ok);
        // Also set S so RTS has something to pop (value doesn't matter — BSR
        // pushes the actual return address before RTS reads it).
        let s = EmfeRegValue {
            reg_id: 6, // RegId::S
            value: EmfeRegValueUnion { u64_: 0xFF00 },
        };
        assert_eq!(emfe_set_registers(h, &s, 1), EmfeResult::Ok);

        assert_eq!(emfe_step_over(h), EmfeResult::Ok);

        let mut got = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0 },
        };
        assert_eq!(emfe_get_registers(h, &mut got, 1), EmfeResult::Ok);
        assert_eq!(
            got.value.u64_, 0x0102,
            "step_over must land on the instruction after BSR"
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn step_out_returns_to_caller() {
    // The plugin's step_out delegates to em6809::Cpu::step_out, which
    // uses the topmost shadow-frame's recorded return_addr as the
    // target. So the test has to actually *make* a call (BSR) to
    // populate the shadow stack — a synthetic "fake return address
    // pre-loaded on S" scenario would correctly hit `EmptyStack` and
    // not run anywhere, which matches gdb's `finish` semantics ("no
    // frame to return out of").
    //
    //   $0200  8D 02     BSR  +2 -> $0204    (caller)
    //   $0202  12        NOP                 (return target)
    //   $0203  39        RTS                 (caller's RTS, never run)
    //   $0204  12        NOP                 (callee body)
    //   $0205  12        NOP
    //   $0206  39        RTS
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        let code: &[(u64, u8)] = &[
            (0x0200, 0x8D),
            (0x0201, 0x02), // BSR +2 -> $0204
            (0x0202, 0x12), // NOP (return target)
            (0x0203, 0x39), // RTS (caller's, never reached)
            (0x0204, 0x12), // NOP (callee body)
            (0x0205, 0x12), // NOP
            (0x0206, 0x39), // RTS
        ];
        for (a, v) in code {
            assert_eq!(emfe_poke_byte(h, *a, *v), EmfeResult::Ok);
        }

        let pc = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0x0200 },
        };
        assert_eq!(emfe_set_registers(h, &pc, 1), EmfeResult::Ok);
        let s = EmfeRegValue {
            reg_id: 6,
            value: EmfeRegValueUnion { u64_: 0xFE00 },
        };
        assert_eq!(emfe_set_registers(h, &s, 1), EmfeResult::Ok);

        // Take one step into the callee so the shadow stack has a
        // recorded frame for step_out to consume.
        assert_eq!(emfe_step(h), EmfeResult::Ok);

        let mut pc_now = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0 },
        };
        assert_eq!(emfe_get_registers(h, &mut pc_now, 1), EmfeResult::Ok);
        assert_eq!(pc_now.value.u64_, 0x0204, "BSR landed inside callee");

        assert_eq!(emfe_step_out(h), EmfeResult::Ok);

        assert_eq!(emfe_get_registers(h, &mut pc_now, 1), EmfeResult::Ok);
        assert_eq!(
            pc_now.value.u64_, 0x0202,
            "step_out must land on the return address recorded by BSR"
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn call_stack_tracks_bsr_and_pops_on_rts() {
    // Nested BSR depth-2; verify shadow stack reports both frames innermost-first,
    // and that RTS pops entries correctly.
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        // $0100: BSR sub1      (to $0104)
        // $0102: NOP / NOP
        // $0104: BSR sub2      (to $0108)  -- sub1
        // $0106: NOP / RTS
        // $0108: NOP           -- sub2: here we observe call stack
        // $0109: RTS
        let code: &[(u64, u8)] = &[
            (0x0100, 0x8D),
            (0x0101, 0x02), // BSR $0104
            (0x0102, 0x12),
            (0x0103, 0x12), // NOP, NOP
            (0x0104, 0x8D),
            (0x0105, 0x02), // BSR $0108   (sub1)
            (0x0106, 0x12),
            (0x0107, 0x39), // NOP, RTS
            (0x0108, 0x12), // NOP          (sub2 body)
            (0x0109, 0x39), // RTS
        ];
        for (a, v) in code {
            assert_eq!(emfe_poke_byte(h, *a, *v), EmfeResult::Ok);
        }

        let pc = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0x0100 },
        };
        assert_eq!(emfe_set_registers(h, &pc, 1), EmfeResult::Ok);
        let s = EmfeRegValue {
            reg_id: 6,
            value: EmfeRegValueUnion { u64_: 0xFF00 },
        };
        assert_eq!(emfe_set_registers(h, &s, 1), EmfeResult::Ok);

        // Execute: BSR $0104; BSR $0108 → now inside sub2 with depth 2.
        assert_eq!(emfe_step(h), EmfeResult::Ok); // BSR
        assert_eq!(emfe_step(h), EmfeResult::Ok); // BSR

        let mut frames = [EmfeCallStackEntry {
            call_pc: 0,
            target_pc: 0,
            return_pc: 0,
            frame_pointer: 0,
            kind: EmfeCallStackKind::Call,
            label: ptr::null(),
        }; 4];
        let n = emfe_get_call_stack(h, frames.as_mut_ptr(), 4);
        assert_eq!(n, 2, "expected 2 frames after nested BSR");
        // Innermost first (top of stack).
        assert_eq!(frames[0].call_pc, 0x0104, "sub2 caller is at $0104");
        assert_eq!(frames[0].target_pc, 0x0108);
        assert_eq!(frames[0].return_pc, 0x0106);
        assert_eq!(frames[1].call_pc, 0x0100, "sub1 caller is at $0100");
        assert_eq!(frames[1].target_pc, 0x0104);
        assert_eq!(frames[1].return_pc, 0x0102);

        // Now step through sub2's NOP + RTS — one frame should pop.
        assert_eq!(emfe_step(h), EmfeResult::Ok); // NOP at $0108
        assert_eq!(emfe_step(h), EmfeResult::Ok); // RTS
        let n = emfe_get_call_stack(h, frames.as_mut_ptr(), 4);
        assert_eq!(n, 1, "one frame left after RTS");
        assert_eq!(frames[0].call_pc, 0x0100);

        // Through sub1's NOP + RTS — stack should empty.
        assert_eq!(emfe_step(h), EmfeResult::Ok); // NOP at $0106
        assert_eq!(emfe_step(h), EmfeResult::Ok); // RTS
        let n = emfe_get_call_stack(h, frames.as_mut_ptr(), 4);
        assert_eq!(n, 0, "stack empty after outer RTS");

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn breakpoint_condition_skips_when_false_via_run() {
    // Reproduces the user-reported pattern: two BPs in the same
    // straight-line code path. The earlier BP carries a condition
    // that's false at the moment we reach it, so we expect the
    // run loop to silently skip it and stop only at the later
    // unguarded BP.
    //
    //   $0100  86 03    LDA #$03      A <- $03
    //   $0102  86 15    LDA #$15      A <- $15
    //   $0104  12       NOP           (BP target #2)
    //   $0105  20 FE    BRA -2 (loop) (safety: trap if we run past)
    //
    // BP1 @ $0102  with cond "b == $01"   -> FALSE (B starts at 0)
    // BP2 @ $0104  no condition           -> always halts
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        for (a, v) in [
            (0x0100u64, 0x86u8),
            (0x0101, 0x03),
            (0x0102, 0x86),
            (0x0103, 0x15),
            (0x0104, 0x12),
            (0x0105, 0x20),
            (0x0106, 0xFE),
        ] {
            assert_eq!(emfe_poke_byte(h, a, v), EmfeResult::Ok);
        }
        let pc = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0x0100 },
        };
        assert_eq!(emfe_set_registers(h, &pc, 1), EmfeResult::Ok);

        // BP1: false condition. Add then attach condition.
        assert_eq!(emfe_add_breakpoint(h, 0x0102), EmfeResult::Ok);
        let cond = std::ffi::CString::new("b == $01").unwrap();
        assert_eq!(
            emfe_set_breakpoint_condition(h, 0x0102, cond.as_ptr()),
            EmfeResult::Ok
        );
        // BP2: unguarded.
        assert_eq!(emfe_add_breakpoint(h, 0x0104), EmfeResult::Ok);

        assert_eq!(emfe_run(h), EmfeResult::Ok);
        // Wait up to 2s for the worker to halt.
        let mut tries = 0;
        while emfe_get_state(h) == EmfeState::Running && tries < 200 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            tries += 1;
        }
        assert!(emfe_get_state(h) != EmfeState::Running, "run never halted");

        // Should be stopped at BP2 ($0104), not BP1 ($0102).
        let mut pc_now = EmfeRegValue {
            reg_id: 7,
            value: EmfeRegValueUnion { u64_: 0 },
        };
        assert_eq!(emfe_get_registers(h, &mut pc_now, 1), EmfeResult::Ok);
        assert_eq!(
            pc_now.value.u64_, 0x0104,
            "false-condition BP should be skipped; expected halt at $0104"
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_colon_define_and_call() {
    // Define `: double dup + ;`, then evaluate `3 double .` — should print "6 ".
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let line = b": DOUBLE DUP + ;\r";
        for ch in line {
            assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
            std::thread::sleep(std::time::Duration::from_millis(15));
        }
        // Wait for the first "ok".
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if String::from_utf8_lossy(&UART_BUF).matches("ok").count() >= 1 {
                break;
            }
        }
        let line = b"3 DOUBLE .\r";
        for ch in line {
            assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
            std::thread::sleep(std::time::Duration::from_millis(15));
        }
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if String::from_utf8_lossy(&UART_BUF).matches("ok").count() >= 2 {
                break;
            }
        }
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("6  ok"),
            "colon-defined word should produce \"6  ok\"; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_if_then_and_begin_until() {
    // Define `: ABS DUP 0< IF NEGATE THEN ;` and verify `-5 ABS .` prints "5 ".
    // Then define a countdown loop using BEGIN/UNTIL.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }

        let send = |line: &[u8], want_ok: usize| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(12));
            }
            for _ in 0..200 {
                std::thread::sleep(std::time::Duration::from_millis(10));
                if String::from_utf8_lossy(&UART_BUF).matches("ok").count() >= want_ok {
                    break;
                }
            }
        };

        send(b": ABS DUP 0< IF NEGATE THEN ;\r", 1);
        send(b"-5 ABS .\r", 2);
        send(b"7 ABS .\r", 3);
        // Minimal BEGIN/UNTIL: emit one '*' then exit on -1.
        send(b": ONCE BEGIN 42 EMIT -1 UNTIL ;\r", 4);
        send(b"ONCE\r", 5);

        assert_eq!(emfe_stop(h), EmfeResult::Ok);
        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("5  ok"),
            "ABS of -5 should print 5; got {:?}",
            got
        );
        assert!(
            got.contains("7  ok"),
            "ABS of 7 should print 7; got {:?}",
            got
        );
        assert!(
            got.contains("* ok"),
            "ONCE should emit a single '*' then ok; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase1_echo() {
    // Phase 1 Tiny Lisp: reader + printer + echo.
    // Feed several S-expressions and verify they come back formatted correctly.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Self-evaluating values:
        send(b"42\r");
        send(b"-17\r");
        send(b"T\r");
        send(b"NIL\r");
        // QUOTE (Phase 2): unevaluated S-expressions.
        send(b"'foo\r");
        send(b"(quote (1 2 3))\r");
        send(b"(quote (a b (c d) e))\r");
        std::thread::sleep(std::time::Duration::from_millis(400));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("\n42\r\n"),
            "42 should eval to 42; got {:?}",
            got
        );
        assert!(
            got.contains("\n-17\r\n"),
            "-17 should eval to -17; got {:?}",
            got
        );
        assert!(got.contains("\nT\r\n"), "T eval; got {:?}", got);
        assert!(got.contains("\nNIL\r\n"), "NIL eval; got {:?}", got);
        assert!(got.contains("\nFOO\r\n"), "'foo → FOO; got {:?}", got);
        assert!(
            got.contains("\n(1 2 3)\r\n"),
            "(quote (1 2 3)) → (1 2 3); got {:?}",
            got
        );
        assert!(
            got.contains("\n(A B (C D) E)\r\n"),
            "quoted list; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase2_eval() {
    // Phase 2: IF / DEFVAR / symbol lookup.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(if t 1 2)\r");
        send(b"(if nil 1 2)\r");
        send(b"(defvar x 42)\r");
        send(b"x\r");
        send(b"(if (quote something) x (quote skipped))\r");
        std::thread::sleep(std::time::Duration::from_millis(400));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(got.contains("\n1\r\n"), "(if t 1 2) → 1; got {:?}", got);
        assert!(got.contains("\n2\r\n"), "(if nil 1 2) → 2; got {:?}", got);
        assert!(got.contains("\nX\r\n"), "(defvar x 42) → X; got {:?}", got);
        assert!(got.contains("\n42\r\n"), "x → 42; got {:?}", got);
        // The (if ...) that returns x should have printed 42 again (counted in
        // occurrence count above but let's ensure it's still right).
        assert!(
            got.matches("\n42\r\n").count() >= 2,
            "x should print 42 twice (direct + via if); got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase3_primitives() {
    // Phase 3: CONS/CAR/CDR/ATOM/EQ/NULL/+/-/<
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(cons 1 2)\r");
        send(b"(car (cons 3 4))\r");
        send(b"(cdr (cons 5 6))\r");
        send(b"(atom 7)\r");
        send(b"(atom (cons 1 2))\r");
        send(b"(null nil)\r");
        send(b"(null 0)\r");
        send(b"(eq 1 1)\r");
        send(b"(eq 1 2)\r");
        send(b"(+ 3 4)\r");
        send(b"(- 10 6)\r");
        send(b"(+ (+ 1 2) (- 10 6))\r"); // 3 + 4 = 7
        send(b"(< 3 5)\r");
        send(b"(< 5 3)\r");
        std::thread::sleep(std::time::Duration::from_millis(400));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(got.contains("\n(1 . 2)\r\n"), "cons; got {:?}", got);
        assert!(got.contains("\n3\r\n"), "car; got {:?}", got);
        assert!(got.contains("\n6\r\n"), "cdr; got {:?}", got);
        assert!(
            got.contains("(atom 7)\r\nT\r\n"),
            "atom of atom; got {:?}",
            got
        );
        assert!(
            got.contains("(atom (cons 1 2))\r\nNIL\r\n"),
            "atom of pair; got {:?}",
            got
        );
        assert!(
            got.contains("(null nil)\r\nT\r\n"),
            "null nil; got {:?}",
            got
        );
        assert!(got.contains("(null 0)\r\nNIL\r\n"), "null 0; got {:?}", got);
        assert!(got.contains("(eq 1 1)\r\nT\r\n"), "eq equal; got {:?}", got);
        assert!(
            got.contains("(eq 1 2)\r\nNIL\r\n"),
            "eq unequal; got {:?}",
            got
        );
        assert!(got.contains("(+ 3 4)\r\n7\r\n"), "+; got {:?}", got);
        assert!(got.contains("(- 10 6)\r\n4\r\n"), "-; got {:?}", got);
        assert!(
            got.contains("(+ (+ 1 2) (- 10 6))\r\n7\r\n"),
            "nested arith; got {:?}",
            got
        );
        assert!(got.contains("(< 3 5)\r\nT\r\n"), "< true; got {:?}", got);
        assert!(got.contains("(< 5 3)\r\nNIL\r\n"), "< false; got {:?}", got);

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase4_lambda_defun() {
    // Phase 4: LAMBDA / DEFUN / function application / recursion
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(lambda (x) x)\r"); // #<CLOSURE>
        send(b"((lambda (x) (+ x 1)) 41)\r"); // 42
        send(b"((lambda (x y) (+ x y)) 3 4)\r"); // 7
        send(b"(defun inc (x) (+ x 1))\r"); // INC
        send(b"(inc 10)\r"); // 11
        send(b"(defun add (x y) (+ x y))\r"); // ADD
        send(b"(add 100 23)\r"); // 123
        send(b"(defun fact (n) (if (< n 2) 1 (* n (fact (- n 1)))))\r");
        // No * yet — use a simpler recursion: countdown via subtraction.
        send(b"(defun sum (n) (if (< n 1) 0 (+ n (sum (- n 1)))))\r");
        send(b"(sum 5)\r"); // 15 = 1+2+3+4+5
        std::thread::sleep(std::time::Duration::from_millis(800));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(lambda (x) x)\r\n#<CLOSURE>\r\n"),
            "lambda; got {:?}",
            got
        );
        assert!(
            got.contains("((lambda (x) (+ x 1)) 41)\r\n42\r\n"),
            "applied lambda; got {:?}",
            got
        );
        assert!(
            got.contains("((lambda (x y) (+ x y)) 3 4)\r\n7\r\n"),
            "2-arg lambda; got {:?}",
            got
        );
        assert!(
            got.contains("(defun inc (x) (+ x 1))\r\nINC\r\n"),
            "defun return name; got {:?}",
            got
        );
        assert!(
            got.contains("(inc 10)\r\n11\r\n"),
            "defun'd call; got {:?}",
            got
        );
        assert!(
            got.contains("(add 100 23)\r\n123\r\n"),
            "2-arg defun; got {:?}",
            got
        );
        assert!(
            got.contains("(sum 5)\r\n15\r\n"),
            "recursive defun; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase5_cond_let_setq() {
    // Phase 5: COND / LET / SETQ
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(cond ((< 1 2) 'yes) (t 'no))\r");
        send(b"(cond ((< 2 1) 'yes) (t 'no))\r");
        send(b"(cond ((< 2 1) 'a) ((< 3 4) 'b) (t 'c))\r");
        send(b"(cond ((< 2 1) 'a) ((< 4 3) 'b))\r"); // no match → NIL
        send(b"(let ((x 3)) (+ x 10))\r");
        send(b"(let ((x 3) (y 4)) (+ x y))\r");
        send(b"(let ((x 3)) (let ((y 4)) (+ x y)))\r");
        send(b"(defvar x 10)\r");
        send(b"(setq x 20)\r");
        send(b"x\r");
        send(b"(defun bump () (setq x (+ x 1)))\r");
        send(b"(bump)\r");
        send(b"(bump)\r");
        send(b"x\r");
        std::thread::sleep(std::time::Duration::from_millis(900));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(cond ((< 1 2) 'yes) (t 'no))\r\nYES\r\n"),
            "cond yes; got {:?}",
            got
        );
        assert!(
            got.contains("(cond ((< 2 1) 'yes) (t 'no))\r\nNO\r\n"),
            "cond no; got {:?}",
            got
        );
        assert!(
            got.contains("(cond ((< 2 1) 'a) ((< 3 4) 'b) (t 'c))\r\nB\r\n"),
            "cond middle; got {:?}",
            got
        );
        assert!(
            got.contains("(cond ((< 2 1) 'a) ((< 4 3) 'b))\r\nNIL\r\n"),
            "cond fallthrough; got {:?}",
            got
        );
        assert!(
            got.contains("(let ((x 3)) (+ x 10))\r\n13\r\n"),
            "let single; got {:?}",
            got
        );
        assert!(
            got.contains("(let ((x 3) (y 4)) (+ x y))\r\n7\r\n"),
            "let two; got {:?}",
            got
        );
        assert!(
            got.contains("(let ((x 3)) (let ((y 4)) (+ x y)))\r\n7\r\n"),
            "nested let; got {:?}",
            got
        );
        assert!(
            got.contains("(setq x 20)\r\n20\r\n"),
            "setq returns new value; got {:?}",
            got
        );
        // After (setq x 20) evaluation of x should give 20
        assert!(
            got.contains("(setq x 20)\r\n20\r\n> x\r\n20\r\n"),
            "setq persisted; got {:?}",
            got
        );
        // After two (bump) calls x should be 22
        assert!(got.contains("(bump)\r\n21\r\n"), "bump 1; got {:?}", got);
        assert!(got.contains("(bump)\r\n22\r\n"), "bump 2; got {:?}", got);

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_primitives_first_class() {
    // Primitives (CAR/CDR/+/...) are bound in global_env as built-in values
    // and should be first-class: storable, conditionally dispatchable, and
    // passable as arguments to user-defined higher-order functions.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"car\r"); // #<BUILTIN>
        send(b"(defvar f car)\r"); // F
        send(b"(f '(1 2))\r"); // 1
        send(b"((if t car cdr) '(1 2))\r"); // 1 (dynamic dispatch)
        send(b"((if nil car cdr) '(1 2))\r"); // (2)
        send(b"(defun my-map (f xs) (if (null xs) nil (cons (f (car xs)) (my-map f (cdr xs)))))\r");
        send(b"(my-map car '((1 2) (3 4) (5 6)))\r"); // (1 3 5)
        send(b"(my-map cdr '((1 2) (3 4) (5 6)))\r"); // ((2) (4) (6))
        std::thread::sleep(std::time::Duration::from_millis(900));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("> car\r\n#<BUILTIN>\r\n"),
            "car as value; got {:?}",
            got
        );
        assert!(
            got.contains("(f '(1 2))\r\n1\r\n"),
            "stored-primitive call; got {:?}",
            got
        );
        assert!(
            got.contains("((if t car cdr) '(1 2))\r\n1\r\n"),
            "dynamic-dispatch car; got {:?}",
            got
        );
        assert!(
            got.contains("((if nil car cdr) '(1 2))\r\n(2)\r\n"),
            "dynamic-dispatch cdr; got {:?}",
            got
        );
        assert!(
            got.contains("(my-map car '((1 2) (3 4) (5 6)))\r\n(1 3 5)\r\n"),
            "higher-order with car; got {:?}",
            got
        );
        assert!(
            got.contains("(my-map cdr '((1 2) (3 4) (5 6)))\r\n((2) (4) (6))\r\n"),
            "higher-order with cdr; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase6_gc() {
    // Phase 6: mark-sweep GC.  (gc) returns the number of freed pair cells
    // as a fixnum.  After GC, allocation must still work (free-list reuse).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"gc\r"); // #<BUILTIN>
        send(b"(cons 1 2)\r"); // (1 . 2)
        send(b"(gc)\r"); // some non-negative number
        send(b"(cons 3 4)\r"); // must still allocate → (3 . 4)
                               // Survival test: define a list, GC, it should still be there.
        send(b"(defvar keep '(10 20 30))\r"); // KEEP
        send(b"(gc)\r"); // some number
        send(b"keep\r"); // (10 20 30)
        send(b"(car keep)\r"); // 10
        send(b"(car (cdr keep))\r"); // 20
        std::thread::sleep(std::time::Duration::from_millis(900));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("> gc\r\n#<BUILTIN>\r\n"),
            "gc as value; got {:?}",
            got
        );
        assert!(
            got.contains("(cons 1 2)\r\n(1 . 2)\r\n"),
            "cons before gc; got {:?}",
            got
        );
        assert!(
            got.contains("(cons 3 4)\r\n(3 . 4)\r\n"),
            "cons after gc (free-list reuse); got {:?}",
            got
        );
        assert!(
            got.contains("keep\r\n(10 20 30)\r\n"),
            "root survived GC; got {:?}",
            got
        );
        assert!(
            got.contains("(car keep)\r\n10\r\n"),
            "root car; got {:?}",
            got
        );
        assert!(
            got.contains("(car (cdr keep))\r\n20\r\n"),
            "root cadr; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase7_stdlib() {
    // Phase 7: PROGN / AND / OR / * primitives plus Tier-1 and Tier-2
    // standard-library functions loaded from a ROM-resident Lisp source.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        // Bootstrap takes longer than before (stdlib load), so wait longer.
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // PROGN / AND / OR
        send(b"(progn 1 2 3)\r");
        send(b"(and 1 2 3)\r");
        send(b"(and 1 nil 3)\r");
        send(b"(or nil nil 7)\r");
        send(b"(or nil nil nil)\r");
        // *
        send(b"(* 6 7)\r");
        send(b"(* -3 4)\r");
        send(b"(* 0 100)\r");
        // Tier 1
        send(b"(not nil)\r");
        send(b"(not 5)\r");
        send(b"(zerop 0)\r");
        send(b"(inc 10)\r");
        send(b"(dec 10)\r");
        send(b"(> 5 3)\r");
        send(b"(cadr '(10 20 30))\r");
        send(b"(abs -7)\r");
        send(b"(max 3 9)\r");
        send(b"(min 3 9)\r");
        // Tier 2
        send(b"(length '(a b c d))\r");
        send(b"(append '(1 2) '(3 4))\r");
        send(b"(reverse '(1 2 3 4))\r");
        send(b"(nth 2 '(10 20 30 40))\r");
        send(b"(last '(1 2 3))\r");
        send(b"(member 3 '(1 2 3 4))\r");
        send(b"(mapcar inc '(1 2 3 4))\r");
        send(b"(filter zerop '(0 1 0 2 0))\r");
        send(b"(reduce + 0 '(1 2 3 4 5))\r");
        send(b"(any zerop '(1 2 0 3))\r");
        send(b"(all zerop '(0 0 0))\r");
        send(b"(equal '(1 2 3) '(1 2 3))\r");
        send(b"(equal '(1 2 3) '(1 2 4))\r");
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        // Special forms
        assert!(
            got.contains("(progn 1 2 3)\r\n3\r\n"),
            "progn; got {:?}",
            got
        );
        assert!(got.contains("(and 1 2 3)\r\n3\r\n"), "and; got {:?}", got);
        assert!(
            got.contains("(and 1 nil 3)\r\nNIL\r\n"),
            "and short; got {:?}",
            got
        );
        assert!(got.contains("(or nil nil 7)\r\n7\r\n"), "or; got {:?}", got);
        assert!(
            got.contains("(or nil nil nil)\r\nNIL\r\n"),
            "or all nil; got {:?}",
            got
        );
        // *
        assert!(got.contains("(* 6 7)\r\n42\r\n"), "mul; got {:?}", got);
        assert!(
            got.contains("(* -3 4)\r\n-12\r\n"),
            "mul neg; got {:?}",
            got
        );
        assert!(
            got.contains("(* 0 100)\r\n0\r\n"),
            "mul zero; got {:?}",
            got
        );
        // Tier 1
        assert!(got.contains("(not nil)\r\nT\r\n"), "not nil; got {:?}", got);
        assert!(got.contains("(not 5)\r\nNIL\r\n"), "not nz; got {:?}", got);
        assert!(got.contains("(zerop 0)\r\nT\r\n"), "zerop; got {:?}", got);
        assert!(got.contains("(inc 10)\r\n11\r\n"), "inc; got {:?}", got);
        assert!(got.contains("(dec 10)\r\n9\r\n"), "dec; got {:?}", got);
        assert!(got.contains("(> 5 3)\r\nT\r\n"), ">; got {:?}", got);
        assert!(
            got.contains("(cadr '(10 20 30))\r\n20\r\n"),
            "cadr; got {:?}",
            got
        );
        assert!(got.contains("(abs -7)\r\n7\r\n"), "abs; got {:?}", got);
        assert!(got.contains("(max 3 9)\r\n9\r\n"), "max; got {:?}", got);
        assert!(got.contains("(min 3 9)\r\n3\r\n"), "min; got {:?}", got);
        // Tier 2
        assert!(
            got.contains("(length '(a b c d))\r\n4\r\n"),
            "length; got {:?}",
            got
        );
        assert!(
            got.contains("(append '(1 2) '(3 4))\r\n(1 2 3 4)\r\n"),
            "append; got {:?}",
            got
        );
        assert!(
            got.contains("(reverse '(1 2 3 4))\r\n(4 3 2 1)\r\n"),
            "reverse; got {:?}",
            got
        );
        assert!(
            got.contains("(nth 2 '(10 20 30 40))\r\n30\r\n"),
            "nth; got {:?}",
            got
        );
        assert!(
            got.contains("(last '(1 2 3))\r\n3\r\n"),
            "last; got {:?}",
            got
        );
        assert!(
            got.contains("(member 3 '(1 2 3 4))\r\n(3 4)\r\n"),
            "member; got {:?}",
            got
        );
        assert!(
            got.contains("(mapcar inc '(1 2 3 4))\r\n(2 3 4 5)\r\n"),
            "mapcar; got {:?}",
            got
        );
        assert!(
            got.contains("(filter zerop '(0 1 0 2 0))\r\n(0 0 0)\r\n"),
            "filter; got {:?}",
            got
        );
        assert!(
            got.contains("(reduce + 0 '(1 2 3 4 5))\r\n15\r\n"),
            "reduce; got {:?}",
            got
        );
        assert!(
            got.contains("(any zerop '(1 2 0 3))\r\nT\r\n"),
            "any; got {:?}",
            got
        );
        assert!(
            got.contains("(all zerop '(0 0 0))\r\nT\r\n"),
            "all; got {:?}",
            got
        );
        assert!(
            got.contains("(equal '(1 2 3) '(1 2 3))\r\nT\r\n"),
            "equal true; got {:?}",
            got
        );
        assert!(
            got.contains("(equal '(1 2 3) '(1 2 4))\r\nNIL\r\n"),
            "equal false; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_phase8_apply_letstar_letrec() {
    // Phase 8: APPLY primitive + LET* (sequential) / LETREC (mutual recursion).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // APPLY — builtin operator
        send(b"(apply cons '(1 2))\r"); // (1 . 2)
        send(b"(apply + '(3 4))\r"); // 7
        send(b"(apply car '((10 20 30)))\r"); // 10
                                              // APPLY — closure operator (user-defined)
        send(b"(defun add3 (a b c) (+ a (+ b c)))\r"); // ADD3
        send(b"(apply add3 '(10 20 30))\r"); // 60
                                             // LET* — sequential bindings (y refers to x)
        send(b"(let* ((x 3) (y (+ x 1))) (* x y))\r"); // 12
                                                       // No LIST primitive yet — build result via cons chain.
        send(b"(let* ((x 5) (y (+ x 1)) (z (+ y 1))) (cons x (cons y (cons z nil))))\r");
        // LETREC — mutually recursive even? / odd?
        send(b"(letrec ((ev (lambda (n) (if (eq n 0) t (od (- n 1))))) (od (lambda (n) (if (eq n 0) nil (ev (- n 1)))))) (ev 4))\r");
        send(b"(letrec ((ev (lambda (n) (if (eq n 0) t (od (- n 1))))) (od (lambda (n) (if (eq n 0) nil (ev (- n 1)))))) (ev 5))\r");
        // LETREC — self-recursion (factorial)
        send(b"(letrec ((f (lambda (n) (if (< n 2) 1 (* n (f (- n 1))))))) (f 5))\r");
        std::thread::sleep(std::time::Duration::from_millis(1000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        // APPLY tests
        assert!(
            got.contains("(apply cons '(1 2))\r\n(1 . 2)\r\n"),
            "apply cons; got {:?}",
            got
        );
        assert!(
            got.contains("(apply + '(3 4))\r\n7\r\n"),
            "apply +; got {:?}",
            got
        );
        assert!(
            got.contains("(apply car '((10 20 30)))\r\n10\r\n"),
            "apply car; got {:?}",
            got
        );
        assert!(
            got.contains("(apply add3 '(10 20 30))\r\n60\r\n"),
            "apply closure; got {:?}",
            got
        );
        // LET*
        assert!(
            got.contains("(let* ((x 3) (y (+ x 1))) (* x y))\r\n12\r\n"),
            "let* sequential; got {:?}",
            got
        );
        assert!(
            got.contains("(cons x (cons y (cons z nil))))\r\n(5 6 7)\r\n"),
            "let* 3-step; got {:?}",
            got
        );
        // LETREC mutual recursion
        assert!(
            got.contains("(ev 4))\r\nT\r\n"),
            "letrec even 4; got {:?}",
            got
        );
        assert!(
            got.contains("(ev 5))\r\nNIL\r\n"),
            "letrec even 5 = odd; got {:?}",
            got
        );
        // LETREC self-recursion
        assert!(
            got.contains("(f 5))\r\n120\r\n"),
            "letrec factorial 5; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_usability_pack() {
    // Usability pack: list / print / newline / assoc / = / multi-body / ; comments
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // list (varargs)
        send(b"(list)\r"); // NIL
        send(b"(list 1 2 3)\r"); // (1 2 3)
        send(b"(list 'a 'b 'c)\r"); // (A B C)
                                    // =
        send(b"(= 5 5)\r"); // T
        send(b"(= 5 6)\r"); // NIL
                            // print / newline
        send(b"(print 42)\r"); // prints 42 then returns 42
        send(b"(newline)\r"); // prints newline, returns NIL
                              // assoc
        send(b"(assoc 'b '((a . 1) (b . 2) (c . 3)))\r"); // (B . 2)
        send(b"(assoc 'z '((a . 1) (b . 2)))\r"); // NIL
                                                  // Line comments (single-line only — REPL reads one line at a time)
        send(b"42 ; this is a comment\r");
        send(b"(+ 1 2) ; trailing comment\r");
        // Multi-body defun (implicit PROGN)
        send(b"(defun seq () (print 1) (print 2) 3)\r");
        send(b"(seq)\r"); // prints 1, 2, returns 3
                          // Multi-body let
        send(b"(let ((x 10)) (print x) (+ x 5))\r"); // prints 10, returns 15
        std::thread::sleep(std::time::Duration::from_millis(1000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        // list
        assert!(
            got.contains("(list)\r\nNIL\r\n"),
            "list empty; got {:?}",
            got
        );
        assert!(
            got.contains("(list 1 2 3)\r\n(1 2 3)\r\n"),
            "list 3; got {:?}",
            got
        );
        assert!(
            got.contains("(list 'a 'b 'c)\r\n(A B C)\r\n"),
            "list syms; got {:?}",
            got
        );
        // =
        assert!(got.contains("(= 5 5)\r\nT\r\n"), "= true; got {:?}", got);
        assert!(got.contains("(= 5 6)\r\nNIL\r\n"), "= false; got {:?}", got);
        // print returns its arg
        assert!(
            got.contains("(print 42)\r\n42\r\n42\r\n"),
            "print; got {:?}",
            got
        );
        // newline returns NIL
        assert!(
            got.contains("(newline)\r\n\r\nNIL\r\n"),
            "newline; got {:?}",
            got
        );
        // assoc
        assert!(
            got.contains("(assoc 'b '((a . 1) (b . 2) (c . 3)))\r\n(B . 2)\r\n"),
            "assoc hit; got {:?}",
            got
        );
        assert!(
            got.contains("(assoc 'z '((a . 1) (b . 2)))\r\nNIL\r\n"),
            "assoc miss; got {:?}",
            got
        );
        // comment (single-line): 42 ; foo → 42
        assert!(
            got.contains("42 ; this is a comment\r\n42\r\n"),
            "line comment; got {:?}",
            got
        );
        assert!(
            got.contains("(+ 1 2) ; trailing comment\r\n3\r\n"),
            "trailing comment; got {:?}",
            got
        );
        // Multi-body defun: (seq) prints 1, 2, returns 3
        assert!(
            got.contains("(seq)\r\n1\r\n2\r\n3\r\n"),
            "multi-body defun; got {:?}",
            got
        );
        // Multi-body let
        assert!(
            got.contains("(let ((x 10)) (print x) (+ x 5))\r\n10\r\n15\r\n"),
            "multi-body let; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_set_bang_alias() {
    // SET! is recognised as a Scheme-style alias for SETQ at the eval
    // dispatcher.  Both forms must mutate the same binding identically;
    // (set! x v) inside a let, dolist, or top-level should behave the
    // same as (setq x v).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Top-level mutation of a global.
        send(b"(defvar x 10)\r"); // X
        send(b"(set! x 99)\r"); // 99
        send(b"x\r"); // 99
        // Mutation inside a let — should affect the let-bound binding.
        send(b"(let ((y 1)) (set! y 7) y)\r"); // 7
        // SET! and SETQ on the same global agree.
        send(b"(setq x 1) (set! x 2) x\r"); // 2
        // SET! works inside dolist (which uses gensym + setq internally).
        send(b"(defvar tot 0)\r");
        send(b"(dolist (n '(1 2 3 4)) (set! tot (+ tot n)))\r");
        send(b"tot\r"); // 10
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(set! x 99)\r\n99\r\n"),
            "set! returns new value; got {:?}",
            got
        );
        // Order in transcript: defvar X => 10 (newly bound), set! returns 99,
        // then bare x echoes 99.
        assert!(
            got.contains("\r\n99\r\n> x\r\n99\r\n"),
            "set! mutated global; got {:?}",
            got
        );
        assert!(
            got.contains("(let ((y 1)) (set! y 7) y)\r\n7\r\n"),
            "set! inside let; got {:?}",
            got
        );
        assert!(
            got.contains("(dolist (n '(1 2 3 4)) (set! tot (+ tot n)))\r\nNIL\r\n"),
            "set! inside dolist; got {:?}",
            got
        );
        assert!(
            got.contains("> tot\r\n10\r\n"),
            "dolist accumulator total; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_predicate_aliases() {
    // Scheme-style ?-suffix aliases for the CL-style bare predicates.
    // Verifies that NULL?, ATOM?, EQ?, ZERO? evaluate to the same callable
    // values as NULL / ATOM / EQ / ZEROP and produce identical results.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(null? nil)\r"); // T
        send(b"(null? '(1 2))\r"); // NIL
        send(b"(atom? 42)\r"); // T
        send(b"(atom? '(1 2))\r"); // NIL
        send(b"(eq? 'a 'a)\r"); // T
        send(b"(eq? 'a 'b)\r"); // NIL
        send(b"(zero? 0)\r"); // T
        send(b"(zero? 5)\r"); // NIL
        // Higher-order use confirms the alias is a callable value, not just
        // a special-form keyword.
        send(b"(filter zero? '(0 1 0 2 0))\r"); // (0 0 0)
        send(b"(any null? '(1 nil 2))\r"); // T
        std::thread::sleep(std::time::Duration::from_millis(1000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(null? nil)\r\nT\r\n"),
            "null? on nil; got {:?}",
            got
        );
        assert!(
            got.contains("(null? '(1 2))\r\nNIL\r\n"),
            "null? on pair; got {:?}",
            got
        );
        assert!(
            got.contains("(atom? 42)\r\nT\r\n"),
            "atom? on fixnum; got {:?}",
            got
        );
        assert!(
            got.contains("(atom? '(1 2))\r\nNIL\r\n"),
            "atom? on pair; got {:?}",
            got
        );
        assert!(
            got.contains("(eq? 'a 'a)\r\nT\r\n"),
            "eq? same; got {:?}",
            got
        );
        assert!(
            got.contains("(eq? 'a 'b)\r\nNIL\r\n"),
            "eq? diff; got {:?}",
            got
        );
        assert!(
            got.contains("(zero? 0)\r\nT\r\n"),
            "zero? on 0; got {:?}",
            got
        );
        assert!(
            got.contains("(zero? 5)\r\nNIL\r\n"),
            "zero? on 5; got {:?}",
            got
        );
        assert!(
            got.contains("(filter zero? '(0 1 0 2 0))\r\n(0 0 0)\r\n"),
            "zero? higher-order; got {:?}",
            got
        );
        assert!(
            got.contains("(any null? '(1 nil 2))\r\nT\r\n"),
            "null? higher-order; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_macros_phase_ab() {
    // Phase A (defmacro + macro dispatch) and Phase B (quasi-quote reader
    // and QUASIQUOTE expansion).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Plain quasi-quote (no unquote) — behaves like quote.
        send(b"`(a b c)\r"); // (A B C)
                             // Quasi-quote with unquote.
        send(b"(let ((x 10)) `(a ,x c))\r"); // (A 10 C)
                                             // Unquote-splicing.
        send(b"(let ((xs '(1 2 3))) `(a ,@xs b))\r"); // (A 1 2 3 B)
                                                      // Bare backquote on atom/expression.
        send(b"`,(+ 1 2)\r"); // 3

        // Phase A — defmacro + macro dispatch.  Macro written without
        // quasi-quote (manual list construction).
        send(b"(defmacro my-when (test body) (list 'if test body 'nil))\r");
        send(b"(my-when (< 1 2) 'yes)\r"); // YES
        send(b"(my-when (< 2 1) 'yes)\r"); // NIL

        // Phase B — macro with quasi-quote body.
        send(b"(defmacro unless (test body) `(if ,test nil ,body))\r");
        send(b"(unless (< 1 2) 'ran)\r"); // NIL
        send(b"(unless (< 2 1) 'ran)\r"); // RAN

        // Quasi-quote + unquote-splicing in a macro body (classic when).
        send(b"(defmacro when2 (test . body) `(if ,test (progn ,@body) nil))\r");
        // Our reader doesn't support dotted params, so fall back to explicit list.
        send(b"(defmacro when3 (test body) `(if ,test (progn ,body) nil))\r");
        send(b"(when3 (< 1 2) (print 'running))\r");
        // Macro value appears as #<MACRO>.
        send(b"unless\r");
        std::thread::sleep(std::time::Duration::from_millis(1000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("`(a b c)\r\n(A B C)\r\n"),
            "plain qq; got {:?}",
            got
        );
        assert!(
            got.contains("(let ((x 10)) `(a ,x c))\r\n(A 10 C)\r\n"),
            "qq + unquote; got {:?}",
            got
        );
        assert!(
            got.contains("(let ((xs '(1 2 3))) `(a ,@xs b))\r\n(A 1 2 3 B)\r\n"),
            "qq + splice; got {:?}",
            got
        );
        assert!(
            got.contains("`,(+ 1 2)\r\n3\r\n"),
            "bare unquote; got {:?}",
            got
        );

        assert!(
            got.contains("(my-when (< 1 2) 'yes)\r\nYES\r\n"),
            "macro true; got {:?}",
            got
        );
        assert!(
            got.contains("(my-when (< 2 1) 'yes)\r\nNIL\r\n"),
            "macro false; got {:?}",
            got
        );
        assert!(
            got.contains("(unless (< 1 2) 'ran)\r\nNIL\r\n"),
            "unless false; got {:?}",
            got
        );
        assert!(
            got.contains("(unless (< 2 1) 'ran)\r\nRAN\r\n"),
            "unless true; got {:?}",
            got
        );
        assert!(
            got.contains("unless\r\n#<MACRO>\r\n"),
            "macro printed; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_varargs_and_gensym() {
    // Dotted params `(x . rest)` and pure-varargs `args` lambdas + GENSYM.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Reader recognises `.` as dotted-tail marker.
        send(b"'(a . b)\r"); // (A . B)
                             // Dotted params in defun: one fixed + rest.
        send(b"(defun head-tail (x . rest) (list x rest))\r");
        send(b"(head-tail 1 2 3 4)\r"); // (1 (2 3 4))
        send(b"(head-tail 'alone)\r"); // (ALONE NIL)
                                       // Pure varargs lambda.
        send(b"(defun my-list args args)\r"); // defun with symbol params
        send(b"(my-list 10 20 30)\r"); // (10 20 30)
        send(b"(my-list)\r"); // NIL
                              // Variadic macro with `. body`.
        send(b"(defmacro when (test . body) `(if ,test (progn ,@body) nil))\r");
        send(b"(when (< 1 2) (print 'a) (print 'b) 'done)\r");
        // GENSYM returns a unique symbol; two calls differ.
        send(b"(eq (gensym) (gensym))\r"); // NIL — must differ
                                           // gensym's symbol is interned but unique per call.
        send(b"(defvar g1 (gensym))\r");
        send(b"(defvar g2 (gensym))\r");
        send(b"(eq g1 g1)\r"); // T — same binding
        send(b"(eq g1 g2)\r"); // NIL — distinct
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("'(a . b)\r\n(A . B)\r\n"),
            "dotted literal; got {:?}",
            got
        );
        assert!(
            got.contains("(head-tail 1 2 3 4)\r\n(1 (2 3 4))\r\n"),
            "dotted rest; got {:?}",
            got
        );
        assert!(
            got.contains("(head-tail 'alone)\r\n(ALONE NIL)\r\n"),
            "dotted rest empty; got {:?}",
            got
        );
        assert!(
            got.contains("(my-list 10 20 30)\r\n(10 20 30)\r\n"),
            "pure varargs; got {:?}",
            got
        );
        assert!(
            got.contains("(my-list)\r\nNIL\r\n"),
            "pure varargs empty; got {:?}",
            got
        );
        assert!(
            got.contains("(when (< 1 2) (print 'a) (print 'b) 'done)\r\nA\r\nB\r\nDONE\r\n"),
            "variadic macro; got {:?}",
            got
        );
        assert!(
            got.contains("(eq (gensym) (gensym))\r\nNIL\r\n"),
            "gensym unique; got {:?}",
            got
        );
        assert!(
            got.contains("(eq g1 g1)\r\nT\r\n"),
            "gensym stable; got {:?}",
            got
        );
        assert!(
            got.contains("(eq g1 g2)\r\nNIL\r\n"),
            "gensym distinct; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_nested_quasiquote() {
    // Proper depth tracking: UNQUOTE at depth 1 evaluates, at depth > 1
    // stays literal.  QUASIQUOTE increments depth; UNQUOTE decrements.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Depth 1: unquote fires.
        send(b"(let ((x 10)) `(a ,x c))\r"); // (A 10 C)
                                             // Depth 2: inner unquote does NOT fire — template is preserved.
        send(b"(let ((x 10)) `(a `(b ,x c)))\r");
        // Depth 2 with ,, : outer double-unquote resolves at depth 1 only.
        send(b"(let ((x 10)) `(a `(b ,,x c)))\r");
        // Plain nested literal.
        send(b"`(x `(y z))\r"); // (X (QUASIQUOTE (Y Z)))
        std::thread::sleep(std::time::Duration::from_millis(1000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(let ((x 10)) `(a ,x c))\r\n(A 10 C)\r\n"),
            "depth-1 unquote; got {:?}",
            got
        );
        // Depth-2 unquote should be preserved literal:
        // `(a `(b ,x c)) → (A (QUASIQUOTE (B (UNQUOTE X) C)))
        assert!(
            got.contains("(let ((x 10)) `(a `(b ,x c)))\r\n(A (QUASIQUOTE (B (UNQUOTE X) C)))\r\n"),
            "depth-2 unquote preserved; got {:?}",
            got
        );
        // Depth-2 with ,,: outer , binds at outer depth (1), inner , stays.
        // `(a `(b ,,x c)) → (A (QUASIQUOTE (B (UNQUOTE 10) C)))
        assert!(
            got.contains(
                "(let ((x 10)) `(a `(b ,,x c)))\r\n(A (QUASIQUOTE (B (UNQUOTE 10) C)))\r\n"
            ),
            "depth-2 double-unquote; got {:?}",
            got
        );
        assert!(
            got.contains("`(x `(y z))\r\n(X (QUASIQUOTE (Y Z)))\r\n"),
            "nested literal; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_hygiene_stdlib() {
    // Hygienic macros defined in the ROM stdlib using gensym:
    //   when / unless / swap / with-gensyms / while / dolist
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // when / unless — simple body splicing via ,@
        send(b"(when (< 1 2) (print 'a) (print 'b) 'done)\r"); // A B DONE
        send(b"(unless (< 2 1) 'ok)\r"); // OK
        send(b"(unless (< 1 2) 'ok)\r"); // NIL
                                         // swap — verify both values change, and hygiene against `tmp`
        send(b"(defvar x 1)\r");
        send(b"(defvar y 2)\r");
        send(b"(swap x y)\r"); // 1  (value of ,b tmp)
        send(b"x\r"); // 2
        send(b"y\r"); // 1
                      // Hygiene: ユーザ変数が `tmp` という名前でも壊れない
        send(b"(defvar tmp 100)\r");
        send(b"(defvar other 200)\r");
        send(b"(swap tmp other)\r");
        send(b"tmp\r"); // 200
        send(b"other\r"); // 100
                          // with-gensyms — explicit usage in a macro we define inline
        send(
            b"(defmacro dup-print (e) (with-gensyms (v) `(let ((,v ,e)) (print ,v) (print ,v))))\r",
        );
        send(b"(dup-print (+ 1 2))\r"); // 3 3 (evaluated once)
                                        // while — loop
        send(b"(defvar n 0)\r");
        send(b"(defvar sum 0)\r");
        send(b"(while (< n 5) (setq sum (+ sum n)) (setq n (+ n 1)))\r");
        send(b"sum\r"); // 10 (0+1+2+3+4)
                        // dolist — iteration
        send(b"(defvar acc 0)\r");
        send(b"(dolist (i '(10 20 30)) (setq acc (+ acc i)))\r");
        send(b"acc\r"); // 60
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(when (< 1 2) (print 'a) (print 'b) 'done)\r\nA\r\nB\r\nDONE\r\n"),
            "when body splice; got {:?}",
            got
        );
        assert!(
            got.contains("(unless (< 2 1) 'ok)\r\nOK\r\n"),
            "unless true; got {:?}",
            got
        );
        assert!(
            got.contains("(unless (< 1 2) 'ok)\r\nNIL\r\n"),
            "unless false; got {:?}",
            got
        );
        assert!(
            got.contains("(swap x y)\r\n1\r\n> x\r\n2\r\n> y\r\n1\r\n"),
            "swap x y; got {:?}",
            got
        );
        assert!(
            got.contains("(swap tmp other)\r\n"),
            "swap executed; got {:?}",
            got
        );
        assert!(
            got.contains("> tmp\r\n200\r\n"),
            "swap tmp captured; got {:?}",
            got
        );
        assert!(
            got.contains("> other\r\n100\r\n"),
            "swap other captured; got {:?}",
            got
        );
        assert!(
            got.contains("(dup-print (+ 1 2))\r\n3\r\n3\r\n"),
            "with-gensyms + once-eval; got {:?}",
            got
        );
        assert!(got.contains("> sum\r\n10\r\n"), "while sum; got {:?}", got);
        assert!(got.contains("> acc\r\n60\r\n"), "dolist sum; got {:?}", got);

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_strings() {
    // String literals "..." + primitives: string-length / string= /
    // string-append / string-ref / string->list / list->string.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"\"hello\"\r"); // "hello"
        send(b"(string-length \"hello\")\r"); // 5
        send(b"(string-length \"\")\r"); // 0
        send(b"(string= \"abc\" \"abc\")\r"); // T
        send(b"(string= \"abc\" \"abd\")\r"); // NIL
        send(b"(string= \"abc\" \"ab\")\r"); // NIL
        send(b"(string-append \"foo\" \"bar\")\r"); // "foobar"
        send(b"(string-ref \"abc\" 0)\r"); // 97 (ASCII 'a')
        send(b"(string-ref \"abc\" 2)\r"); // 99 (ASCII 'c')
        send(b"(string->list \"ab\")\r"); // (97 98)
        send(b"(list->string '(72 73 33))\r"); // "HI!"
        send(b"(defvar s \"persist\")\r");
        send(b"(string-length s)\r"); // 7
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("\"hello\"\r\n\"hello\"\r\n"),
            "literal; got {:?}",
            got
        );
        assert!(
            got.contains("(string-length \"hello\")\r\n5\r\n"),
            "length; got {:?}",
            got
        );
        assert!(
            got.contains("(string-length \"\")\r\n0\r\n"),
            "empty length; got {:?}",
            got
        );
        assert!(
            got.contains("(string= \"abc\" \"abc\")\r\nT\r\n"),
            "streq same; got {:?}",
            got
        );
        assert!(
            got.contains("(string= \"abc\" \"abd\")\r\nNIL\r\n"),
            "streq diff; got {:?}",
            got
        );
        assert!(
            got.contains("(string= \"abc\" \"ab\")\r\nNIL\r\n"),
            "streq len; got {:?}",
            got
        );
        assert!(
            got.contains("(string-append \"foo\" \"bar\")\r\n\"foobar\"\r\n"),
            "append; got {:?}",
            got
        );
        assert!(
            got.contains("(string-ref \"abc\" 0)\r\n97\r\n"),
            "ref 0; got {:?}",
            got
        );
        assert!(
            got.contains("(string-ref \"abc\" 2)\r\n99\r\n"),
            "ref 2; got {:?}",
            got
        );
        assert!(
            got.contains("(string->list \"ab\")\r\n(97 98)\r\n"),
            "to-list; got {:?}",
            got
        );
        assert!(
            got.contains("(list->string '(72 73 33))\r\n\"HI!\"\r\n"),
            "from-list; got {:?}",
            got
        );
        assert!(
            got.contains("(string-length s)\r\n7\r\n"),
            "defvar string; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_funcall_numeric() {
    // FUNCALL macro + / + MOD primitives + <= and >= (stdlib).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // funcall
        send(b"(funcall + 3 4)\r"); // 7
        send(b"(funcall car '(10 20 30))\r"); // 10
        send(b"(defun add3 (a b c) (+ a (+ b c)))\r");
        send(b"(funcall add3 1 2 3)\r"); // 6
                                         // / — diagnostic small cases
        send(b"(/ 6 2)\r"); // 3
        send(b"(/ 7 2)\r"); // 3
        send(b"(/ 15 3)\r"); // 5
        send(b"(/ 100 7)\r"); // 14
        send(b"(/ 100 10)\r"); // 10
        send(b"(/ -17 5)\r"); // -3 (truncation)
        send(b"(/ 17 -5)\r"); // -3
        send(b"(/ -17 -5)\r"); // 3
                               // mod
        send(b"(mod 17 5)\r"); // 2
        send(b"(mod 100 10)\r"); // 0
        send(b"(mod -17 5)\r"); // -2 (sign of dividend)
                                // <= / >=
        send(b"(<= 3 3)\r"); // T
        send(b"(<= 3 4)\r"); // T
        send(b"(<= 4 3)\r"); // NIL
        send(b"(>= 3 3)\r"); // T
        send(b"(>= 4 3)\r"); // T
        send(b"(>= 3 4)\r"); // NIL
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(funcall + 3 4)\r\n7\r\n"),
            "fc+; got {:?}",
            got
        );
        assert!(
            got.contains("(funcall car '(10 20 30))\r\n10\r\n"),
            "fccar; got {:?}",
            got
        );
        assert!(
            got.contains("(funcall add3 1 2 3)\r\n6\r\n"),
            "fcclos; got {:?}",
            got
        );
        assert!(got.contains("(/ 100 7)\r\n14\r\n"), "div; got {:?}", got);
        assert!(
            got.contains("(/ 100 10)\r\n10\r\n"),
            "div exact; got {:?}",
            got
        );
        assert!(
            got.contains("(/ -17 5)\r\n-3\r\n"),
            "div negA; got {:?}",
            got
        );
        assert!(
            got.contains("(/ 17 -5)\r\n-3\r\n"),
            "div negB; got {:?}",
            got
        );
        assert!(
            got.contains("(/ -17 -5)\r\n3\r\n"),
            "div negAB; got {:?}",
            got
        );
        assert!(
            got.contains("(mod 17 5)\r\n2\r\n"),
            "mod pos; got {:?}",
            got
        );
        assert!(
            got.contains("(mod 100 10)\r\n0\r\n"),
            "mod 0; got {:?}",
            got
        );
        assert!(
            got.contains("(mod -17 5)\r\n-2\r\n"),
            "mod neg; got {:?}",
            got
        );
        assert!(got.contains("(<= 3 3)\r\nT\r\n"), "le eq; got {:?}", got);
        assert!(got.contains("(<= 3 4)\r\nT\r\n"), "le lt; got {:?}", got);
        assert!(got.contains("(<= 4 3)\r\nNIL\r\n"), "le gt; got {:?}", got);
        assert!(got.contains("(>= 3 3)\r\nT\r\n"), "ge eq; got {:?}", got);
        assert!(got.contains("(>= 4 3)\r\nT\r\n"), "ge gt; got {:?}", got);
        assert!(got.contains("(>= 3 4)\r\nNIL\r\n"), "ge lt; got {:?}", got);

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_errors() {
    // (error msg) unwinds to REPL with message.
    // (catch tag body...) installs a handler; (throw tag value) returns
    // through the matching catch with the thrown value.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // (error msg) unwinds; subsequent expressions still work.
        send(b"(error \"kaboom\")\r"); // ERROR: "kaboom"
        send(b"(+ 1 2)\r"); // 3 — REPL recovered
                            // Catch with no throw returns body value.
        send(b"(catch 'foo (+ 10 20))\r"); // 30
                                           // Catch + throw from direct body.
        send(b"(catch 'foo (throw 'foo 42))\r"); // 42
                                                 // Catch + throw from nested function.
        send(b"(defun leaper () (throw 'foo 99))\r");
        send(b"(catch 'foo (+ 1 (leaper)))\r"); // 99 — bails out of +
                                                // Non-matching throw tag propagates to outer catch (or REPL).
        send(b"(catch 'outer (catch 'inner (throw 'outer 7)))\r"); // 7
                                                                   // Uncaught throw prints UNCAUGHT and unwinds.
        send(b"(throw 'zz 1)\r"); // UNCAUGHT THROW: ZZ
        send(b"(+ 5 5)\r"); // 10 — recovered
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(error \"kaboom\")\r\nERROR: \"kaboom\"\r\n"),
            "error; got {:?}",
            got
        );
        assert!(
            got.contains("ERROR: \"kaboom\"\r\n> (+ 1 2)\r\n3\r\n"),
            "error recovery; got {:?}",
            got
        );
        assert!(
            got.contains("(catch 'foo (+ 10 20))\r\n30\r\n"),
            "catch no throw; got {:?}",
            got
        );
        assert!(
            got.contains("(catch 'foo (throw 'foo 42))\r\n42\r\n"),
            "catch+throw direct; got {:?}",
            got
        );
        assert!(
            got.contains("(catch 'foo (+ 1 (leaper)))\r\n99\r\n"),
            "catch from fn; got {:?}",
            got
        );
        assert!(
            got.contains("(catch 'outer (catch 'inner (throw 'outer 7)))\r\n7\r\n"),
            "nested catch outer match; got {:?}",
            got
        );
        assert!(
            got.contains("(throw 'zz 1)\r\nUNCAUGHT THROW: ZZ\r\n"),
            "uncaught; got {:?}",
            got
        );
        assert!(
            got.contains("UNCAUGHT THROW: ZZ\r\n> (+ 5 5)\r\n10\r\n"),
            "uncaught recovery; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_bignum() {
    // 32-bit int auto-promotion: reader detects large literals, + - * return
    // boxes on overflow, = / < compare values (not identity) for boxes.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Reader recognises large literals.
        send(b"100000\r"); // 100000 (printed from box)
        send(b"-100000\r"); // -100000
        send(b"(+ 100000 0)\r"); // 100000 (result box)
        send(b"(+ 100000 1)\r"); // 100001
        send(b"(- 100000 0)\r"); // 100000
        send(b"(- 100001 1)\r"); // 100000 (currently broken)
                                 // Addition overflow: 16000 + 500 = 16500 (>15-bit).
        send(b"(+ 16000 500)\r"); // 16500
                                  // Multiplication overflow: 200 * 200 = 40000.
        send(b"(* 200 200)\r"); // 40000
                                // Mixed: fixnum + box.
        send(b"(+ 1 100000)\r"); // 100001
                                 // Large value round-trip.
        send(b"(* 1000 1000)\r"); // 1000000
                                  // = compares value (not box identity).
        send(b"(= 100000 100000)\r"); // T
        send(b"(= 100000 100001)\r"); // NIL
                                      // < for mixed types.
        send(b"(< 100 100000)\r"); // T
        send(b"(< 100000 100)\r"); // NIL
                                   // eq on boxed values is IDENTITY — same value different boxes → NIL.
        send(b"(eq 100000 100000)\r"); // NIL (distinct boxes)
        send(b"(defvar n 100000)\r");
        send(b"(eq n n)\r"); // T (same box)
                             // Result stays in valid range after demotion potential: (- 100001 1)
                             // still returns a box (no demotion); printed same as fixnum.
        send(b"(- 100001 1)\r"); // 100000
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("> 100000\r\n100000\r\n"),
            "big literal; got {:?}",
            got
        );
        assert!(
            got.contains("> -100000\r\n-100000\r\n"),
            "neg big literal; got {:?}",
            got
        );
        assert!(
            got.contains("(+ 16000 500)\r\n16500\r\n"),
            "add overflow; got {:?}",
            got
        );
        assert!(
            got.contains("(* 200 200)\r\n40000\r\n"),
            "mul overflow; got {:?}",
            got
        );
        assert!(
            got.contains("(+ 1 100000)\r\n100001\r\n"),
            "mixed add; got {:?}",
            got
        );
        assert!(
            got.contains("(* 1000 1000)\r\n1000000\r\n"),
            "big mul; got {:?}",
            got
        );
        assert!(
            got.contains("(= 100000 100000)\r\nT\r\n"),
            "value eq; got {:?}",
            got
        );
        assert!(
            got.contains("(= 100000 100001)\r\nNIL\r\n"),
            "value neq; got {:?}",
            got
        );
        assert!(
            got.contains("(< 100 100000)\r\nT\r\n"),
            "lt mixed; got {:?}",
            got
        );
        assert!(
            got.contains("(< 100000 100)\r\nNIL\r\n"),
            "gt mixed; got {:?}",
            got
        );
        assert!(
            got.contains("(eq n n)\r\nT\r\n"),
            "eq identity; got {:?}",
            got
        );
        assert!(
            got.contains("(- 100001 1)\r\n100000\r\n"),
            "sub to big; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_chars_vectors() {
    // Character type (#\c literals, char->integer, integer->char, char?)
    // and vector primitives (make-vector, vector-length, vector-ref,
    // vector-set!, vector->list, list->vector, vector?).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Char literals + conversions.
        send(b"#\\a\r"); // #\a
        send(b"(char->integer #\\a)\r"); // 97
        send(b"(integer->char 65)\r"); // #\A
        send(b"(char? #\\a)\r"); // T
        send(b"(char? 65)\r"); // NIL
        send(b"(char? 'foo)\r"); // NIL
                                 // Vectors.
        send(b"(make-vector 3 0)\r"); // #(0 0 0)
        send(b"(make-vector 5 'x)\r"); // #(X X X X X)
        send(b"(vector-length (make-vector 7 nil))\r"); // 7
        send(b"(defvar v (make-vector 3 10))\r");
        send(b"(vector-ref v 0)\r"); // 10
        send(b"(vector-ref v 2)\r"); // 10
        send(b"(vector-set! v 1 99)\r"); // 99
        send(b"(vector-ref v 1)\r"); // 99
        send(b"v\r"); // #(10 99 10)
        send(b"(list->vector '(1 2 3))\r"); // #(1 2 3)
        send(b"(vector->list (list->vector '(a b c)))\r"); // (A B C)
        send(b"(vector? v)\r"); // T
        send(b"(vector? '(1 2 3))\r"); // NIL
        send(b"(vector-length (list->vector '()))\r"); // 0
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        // Char
        assert!(
            got.contains("> #\\a\r\n#\\a\r\n"),
            "char literal; got {:?}",
            got
        );
        assert!(
            got.contains("(char->integer #\\a)\r\n97\r\n"),
            "c->i; got {:?}",
            got
        );
        assert!(
            got.contains("(integer->char 65)\r\n#\\A\r\n"),
            "i->c; got {:?}",
            got
        );
        assert!(
            got.contains("(char? #\\a)\r\nT\r\n"),
            "charp yes; got {:?}",
            got
        );
        assert!(
            got.contains("(char? 65)\r\nNIL\r\n"),
            "charp no int; got {:?}",
            got
        );
        assert!(
            got.contains("(char? 'foo)\r\nNIL\r\n"),
            "charp no sym; got {:?}",
            got
        );
        // Vectors
        assert!(
            got.contains("(make-vector 3 0)\r\n#(0 0 0)\r\n"),
            "mkvec; got {:?}",
            got
        );
        assert!(
            got.contains("(make-vector 5 'x)\r\n#(X X X X X)\r\n"),
            "mkvec sym; got {:?}",
            got
        );
        assert!(
            got.contains("(vector-length (make-vector 7 nil))\r\n7\r\n"),
            "veclen; got {:?}",
            got
        );
        assert!(
            got.contains("(vector-ref v 0)\r\n10\r\n"),
            "vecref 0; got {:?}",
            got
        );
        assert!(
            got.contains("(vector-set! v 1 99)\r\n99\r\n"),
            "vecset; got {:?}",
            got
        );
        assert!(
            got.contains("(vector-ref v 1)\r\n99\r\n"),
            "vecref mod; got {:?}",
            got
        );
        assert!(
            got.contains("> v\r\n#(10 99 10)\r\n"),
            "vec display; got {:?}",
            got
        );
        assert!(
            got.contains("(list->vector '(1 2 3))\r\n#(1 2 3)\r\n"),
            "l->v; got {:?}",
            got
        );
        assert!(
            got.contains("(vector->list (list->vector '(a b c)))\r\n(A B C)\r\n"),
            "roundtrip; got {:?}",
            got
        );
        assert!(
            got.contains("(vector? v)\r\nT\r\n"),
            "vecp yes; got {:?}",
            got
        );
        assert!(
            got.contains("(vector? '(1 2 3))\r\nNIL\r\n"),
            "vecp no; got {:?}",
            got
        );
        assert!(
            got.contains("(vector-length (list->vector '()))\r\n0\r\n"),
            "empty; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_logical_ops() {
    // Bitwise logical primitives: LOGAND / LOGIOR / LOGXOR / LOGNOT.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // AND
        send(b"(logand 12 10)\r"); // 1100 & 1010 = 1000 = 8
        send(b"(logand -1 255)\r"); // -1 is all bits; 255 = 0xFF
                                    // OR
        send(b"(logior 1 2)\r"); // 3
        send(b"(logior 0 0)\r"); // 0
                                 // XOR
        send(b"(logxor 12 10)\r"); // 1100 ^ 1010 = 0110 = 6
        send(b"(logxor 5 5)\r"); // 0
                                 // NOT
        send(b"(lognot 0)\r"); // -1
        send(b"(lognot -1)\r"); // 0
                                // int32 range: works across 32-bit values.
        send(b"(logand 100000 65535)\r"); // 100000 & 0xFFFF = 100000 mod 65536 = 34464
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(logand 12 10)\r\n8\r\n"),
            "logand; got {:?}",
            got
        );
        assert!(
            got.contains("(logand -1 255)\r\n255\r\n"),
            "logand -1; got {:?}",
            got
        );
        assert!(
            got.contains("(logior 1 2)\r\n3\r\n"),
            "logior; got {:?}",
            got
        );
        assert!(
            got.contains("(logior 0 0)\r\n0\r\n"),
            "logior 0 0; got {:?}",
            got
        );
        assert!(
            got.contains("(logxor 12 10)\r\n6\r\n"),
            "logxor; got {:?}",
            got
        );
        assert!(
            got.contains("(logxor 5 5)\r\n0\r\n"),
            "logxor 5 5; got {:?}",
            got
        );
        assert!(
            got.contains("(lognot 0)\r\n-1\r\n"),
            "lognot 0; got {:?}",
            got
        );
        assert!(
            got.contains("(lognot -1)\r\n0\r\n"),
            "lognot -1; got {:?}",
            got
        );
        assert!(
            got.contains("(logand 100000 65535)\r\n34464\r\n"),
            "logand int32; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_ash() {
    // Arithmetic shift (Common Lisp `ash`): positive shift = left, negative = right.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(ash 1 3)\r"); // 8
        send(b"(ash 1 15)\r"); // 32768 (needs int32)
        send(b"(ash 8 -2)\r"); // 2
        send(b"(ash -8 -1)\r"); // -4 (arithmetic)
        send(b"(ash 0 5)\r"); // 0
        send(b"(ash 100 0)\r"); // 100 (shift 0)
        send(b"(ash 1 32)\r"); // 0 (saturate left)
        send(b"(ash -1 -32)\r"); // -1 (saturate right, negative stays)
        send(b"(ash 100000 -10)\r"); // 97 (100000 / 1024 = 97)
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(got.contains("(ash 1 3)\r\n8\r\n"), "ash 1 3; got {:?}", got);
        assert!(
            got.contains("(ash 1 15)\r\n32768\r\n"),
            "ash 1 15; got {:?}",
            got
        );
        assert!(
            got.contains("(ash 8 -2)\r\n2\r\n"),
            "ash 8 -2; got {:?}",
            got
        );
        assert!(
            got.contains("(ash -8 -1)\r\n-4\r\n"),
            "ash -8 -1; got {:?}",
            got
        );
        assert!(got.contains("(ash 0 5)\r\n0\r\n"), "ash 0; got {:?}", got);
        assert!(
            got.contains("(ash 100 0)\r\n100\r\n"),
            "ash shift 0; got {:?}",
            got
        );
        assert!(
            got.contains("(ash 1 32)\r\n0\r\n"),
            "ash saturate L; got {:?}",
            got
        );
        assert!(
            got.contains("(ash -1 -32)\r\n-1\r\n"),
            "ash saturate R; got {:?}",
            got
        );
        assert!(
            got.contains("(ash 100000 -10)\r\n97\r\n"),
            "ash int32; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_type_string_conversions() {
    // NUMBER->STRING / STRING->NUMBER / SYMBOL->STRING / STRING->SYMBOL.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // number->string
        send(b"(number->string 0)\r"); // "0"
        send(b"(number->string 42)\r"); // "42"
        send(b"(number->string -7)\r"); // "-7"
        send(b"(number->string 100000)\r"); // "100000" (int32)
                                            // string->number
        send(b"(string->number \"123\")\r"); // 123
        send(b"(string->number \"-9\")\r"); // -9
        send(b"(string->number \"0\")\r"); // 0
        send(b"(string->number \"abc\")\r"); // NIL (not a number)
        send(b"(string->number \"\")\r"); // NIL
        send(b"(string->number \"-\")\r"); // NIL
                                           // Round trip
        send(b"(string->number (number->string 9999))\r"); // 9999
                                                           // symbol->string
        send(b"(symbol->string 'foo)\r"); // "FOO"
                                          // string->symbol
        send(b"(string->symbol \"BAR\")\r"); // BAR
        send(b"(eq 'baz (string->symbol \"BAZ\"))\r"); // T
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(number->string 0)\r\n\"0\"\r\n"),
            "n2s 0; got {:?}",
            got
        );
        assert!(
            got.contains("(number->string 42)\r\n\"42\"\r\n"),
            "n2s 42; got {:?}",
            got
        );
        assert!(
            got.contains("(number->string -7)\r\n\"-7\"\r\n"),
            "n2s -7; got {:?}",
            got
        );
        assert!(
            got.contains("(number->string 100000)\r\n\"100000\"\r\n"),
            "n2s int32; got {:?}",
            got
        );
        assert!(
            got.contains("(string->number \"123\")\r\n123\r\n"),
            "s2n 123; got {:?}",
            got
        );
        assert!(
            got.contains("(string->number \"-9\")\r\n-9\r\n"),
            "s2n -9; got {:?}",
            got
        );
        assert!(
            got.contains("(string->number \"0\")\r\n0\r\n"),
            "s2n 0; got {:?}",
            got
        );
        assert!(
            got.contains("(string->number \"abc\")\r\nNIL\r\n"),
            "s2n bad; got {:?}",
            got
        );
        assert!(
            got.contains("(string->number \"\")\r\nNIL\r\n"),
            "s2n empty; got {:?}",
            got
        );
        assert!(
            got.contains("(string->number \"-\")\r\nNIL\r\n"),
            "s2n bareneg; got {:?}",
            got
        );
        assert!(got.contains("9999\r\n"), "roundtrip 9999; got {:?}", got);
        assert!(
            got.contains("(symbol->string 'foo)\r\n\"FOO\"\r\n"),
            "sym2str; got {:?}",
            got
        );
        assert!(
            got.contains("(string->symbol \"BAR\")\r\nBAR\r\n"),
            "str2sym; got {:?}",
            got
        );
        assert!(
            got.contains("(eq 'baz (string->symbol \"BAZ\"))\r\nT\r\n"),
            "intern-id; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_eval_read_string() {
    // EVAL and READ-STRING primitives.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(eval '(+ 1 2))\r"); // 3
        send(b"(eval 42)\r"); // 42 (self-eval)
        send(b"(defvar x 7)\r");
        send(b"(eval 'x)\r"); // 7
        send(b"(read-string \"123\")\r"); // 123
        send(b"(read-string \"(+ 1 2)\")\r"); // (+ 1 2) — NOT evaluated
        send(b"(eval (read-string \"(* 3 4)\"))\r"); // 12
        send(b"(read-string \"foo\")\r"); // FOO (symbol)
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(eval '(+ 1 2))\r\n3\r\n"),
            "eval form; got {:?}",
            got
        );
        assert!(
            got.contains("(eval 42)\r\n42\r\n"),
            "eval self; got {:?}",
            got
        );
        assert!(
            got.contains("(eval 'x)\r\n7\r\n"),
            "eval sym; got {:?}",
            got
        );
        assert!(
            got.contains("(read-string \"123\")\r\n123\r\n"),
            "rds num; got {:?}",
            got
        );
        assert!(
            got.contains("(read-string \"(+ 1 2)\")\r\n(+ 1 2)\r\n"),
            "rds list; got {:?}",
            got
        );
        assert!(
            got.contains("(eval (read-string \"(* 3 4)\"))\r\n12\r\n"),
            "eval(rds); got {:?}",
            got
        );
        assert!(
            got.contains("(read-string \"foo\")\r\nFOO\r\n"),
            "rds sym; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_case_macro() {
    // case stdlib macro: match key against value lists, fall through to t-clause.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(case 'red ((red green) 1) ((blue) 2) (t 0))\r"); // 1
        send(b"(case 'blue ((red green) 1) ((blue) 2) (t 0))\r"); // 2
        send(b"(case 'yellow ((red green) 1) ((blue) 2) (t 0))\r"); // 0 (default)
        send(b"(case 'unknown ((red) 1) ((blue) 2))\r"); // NIL (no t-clause)
        send(b"(defun classify (n) (case n ((0) 'zero) ((1 2 3) 'small) (t 'big)))\r");
        send(b"(classify 0)\r"); // ZERO
        send(b"(classify 2)\r"); // SMALL
        send(b"(classify 42)\r"); // BIG
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("((red green) 1) ((blue) 2) (t 0))\r\n1\r\n"),
            "case red; got {:?}",
            got
        );
        assert!(
            got.contains("((blue) 2) (t 0))\r\n2\r\n"),
            "case blue; got {:?}",
            got
        );
        assert!(
            got.contains("'yellow ((red green) 1) ((blue) 2) (t 0))\r\n0\r\n"),
            "case default; got {:?}",
            got
        );
        assert!(
            got.contains("(case 'unknown ((red) 1) ((blue) 2))\r\nNIL\r\n"),
            "case no-match; got {:?}",
            got
        );
        assert!(
            got.contains("(classify 0)\r\nZERO\r\n"),
            "classify 0; got {:?}",
            got
        );
        assert!(
            got.contains("(classify 2)\r\nSMALL\r\n"),
            "classify 2; got {:?}",
            got
        );
        assert!(
            got.contains("(classify 42)\r\nBIG\r\n"),
            "classify 42; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_load_memory() {
    // (LOAD-MEMORY addr): the test harness pokes a Lisp source into a
    // free RAM region after the code image, then tells the REPL to load
    // and evaluate it from memory.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        // Pre-stage a Lisp script at $3E00 (free area between the code
        // image and the pair pool at $4000).  NUL-terminated.
        let script: &[u8] = b"(defvar gx 42)\n(defvar gy (+ gx 1))\n(defun gdouble (n) (* n 2))\n";
        let base: u64 = 0x3E00;
        for (i, &b) in script.iter().enumerate() {
            assert_eq!(emfe_poke_byte(h, base + i as u64, b), EmfeResult::Ok);
        }
        assert_eq!(
            emfe_poke_byte(h, base + script.len() as u64, 0),
            EmfeResult::Ok
        );
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(load-memory 15872)\r"); // 15872 = 0x3E00
        send(b"gx\r"); // 42
        send(b"gy\r"); // 43
        send(b"(gdouble 21)\r"); // 42
        std::thread::sleep(std::time::Duration::from_millis(1200));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(load-memory 15872)\r\n"),
            "load call echoed; got {:?}",
            got
        );
        assert!(
            got.contains("> gx\r\n42\r\n"),
            "gx should be 42; got {:?}",
            got
        );
        assert!(
            got.contains("> gy\r\n43\r\n"),
            "gy should be 43; got {:?}",
            got
        );
        assert!(
            got.contains("(gdouble 21)\r\n42\r\n"),
            "gdouble 21 -> 42; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_format() {
    // format: ~A/~D/~S display a value, ~% newline, ~~ literal tilde.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Basic substitution.  `~` consumes the next directive char and
        // emits one argument via DISPLAY.
        send(b"(format \"x=~D\" 42)\r"); // x=42
        send(b"(format \"~A+~A=~A\" 1 2 3)\r"); // 1+2=3
                                                // Strings print without quotes in display.
        send(b"(format \"hi ~A!\" \"Bob\")\r"); // hi Bob!
                                                // display primitive directly.
        send(b"(display \"raw\")\r"); // raw (no quotes)
        send(b"(newline)\r");
        // putchar primitive.
        send(b"(putchar 65)\r"); // A
        send(b"(newline)\r");
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(format \"x=~D\" 42)\r\nx=42"),
            "fmt D; got {:?}",
            got
        );
        assert!(
            got.contains("(format \"~A+~A=~A\" 1 2 3)\r\n1+2=3"),
            "fmt AAA; got {:?}",
            got
        );
        assert!(
            got.contains("(format \"hi ~A!\" \"Bob\")\r\nhi Bob!"),
            "fmt str; got {:?}",
            got
        );
        assert!(
            got.contains("(display \"raw\")\r\nraw"),
            "display; got {:?}",
            got
        );
        assert!(got.contains("(putchar 65)\r\nA"), "putchar; got {:?}", got);

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_tco() {
    // Tail-call optimization: deep self-recursion and mutual recursion
    // should not overflow the return stack.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Deep self-recursion: count down to zero.  Without TCO, 1000
        // iterations would blow the stack.
        send(b"(defun cd (n) (if (< n 1) 'done (cd (- n 1))))\r");
        send(b"(cd 1000)\r"); // DONE
                              // Mutual recursion via tail calls.
        send(b"(defun e? (n) (if (= n 0) t (o? (- n 1))))\r");
        send(b"(defun o? (n) (if (= n 0) nil (e? (- n 1))))\r");
        send(b"(e? 100)\r"); // T
        send(b"(o? 101)\r"); // T
                             // Sanity: non-tail recursion still works (just shallow).
        send(b"(defun sum (n) (if (< n 1) 0 (+ n (sum (- n 1)))))\r");
        send(b"(sum 10)\r"); // 55
        std::thread::sleep(std::time::Duration::from_millis(2500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(cd 1000)\r\nDONE\r\n"),
            "tail self-rec 1000; got {:?}",
            got
        );
        assert!(
            got.contains("(e? 100)\r\nT\r\n"),
            "tail mutual even; got {:?}",
            got
        );
        assert!(
            got.contains("(o? 101)\r\nT\r\n"),
            "tail mutual odd; got {:?}",
            got
        );
        assert!(
            got.contains("(sum 10)\r\n55\r\n"),
            "non-tail sanity; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_trace() {
    // trace/untrace: wrap a function to log entry/exit, restore via setq.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(defun sq (n) (* n n))\r");
        send(b"(sq 5)\r"); // 25 (untracked)
        send(b"(trace 'sq)\r");
        send(b"(sq 5)\r"); // ENTER / EXIT messages + 25
        send(b"(sq 7)\r"); // ENTER / EXIT + 49
        send(b"(untrace 'sq)\r");
        send(b"(sq 5)\r"); // 25 (no trace output)
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(got.contains("ENTER SQ (5)"), "trace enter 5; got {:?}", got);
        assert!(
            got.contains("EXIT  SQ -> 25"),
            "trace exit 25; got {:?}",
            got
        );
        assert!(got.contains("ENTER SQ (7)"), "trace enter 7; got {:?}", got);
        assert!(
            got.contains("EXIT  SQ -> 49"),
            "trace exit 49; got {:?}",
            got
        );
        // After untrace, a plain call should not produce ENTER/EXIT lines for this invocation.
        // We check that the final `(sq 5)` output is just "25" on its own line.
        let idx_untrace = got.find("(untrace 'sq)").expect("untrace echo");
        let after = &got[idx_untrace..];
        assert!(
            !after.contains("ENTER"),
            "no ENTER after untrace; got {:?}",
            after
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_defstruct() {
    // defstruct macro: vector-backed records with constructor, predicate,
    // field accessors, and setters.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(defstruct point x y)\r");
        send(b"(defvar p (make-point 3 4))\r");
        send(b"(point-x p)\r"); // 3
        send(b"(point-y p)\r"); // 4
        send(b"(point? p)\r"); // T
        send(b"(point? 42)\r"); // NIL
        send(b"(point? '(1 2 3))\r"); // NIL
        send(b"(set-point-x p 99)\r");
        send(b"(point-x p)\r"); // 99
                                // Nested fields.
        send(b"(defstruct line a b)\r");
        send(b"(defvar L (make-line 11 22))\r");
        send(b"(line-a L)\r"); // 11
        send(b"(line-b L)\r"); // 22
        send(b"(line? L)\r"); // T
        send(b"(point? L)\r"); // NIL
        std::thread::sleep(std::time::Duration::from_millis(2000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(point-x p)\r\n3\r\n"),
            "accessor x; got {:?}",
            got
        );
        assert!(
            got.contains("(point-y p)\r\n4\r\n"),
            "accessor y; got {:?}",
            got
        );
        assert!(
            got.contains("(point? p)\r\nT\r\n"),
            "pred yes; got {:?}",
            got
        );
        assert!(
            got.contains("(point? 42)\r\nNIL\r\n"),
            "pred int; got {:?}",
            got
        );
        assert!(
            got.contains("(point? '(1 2 3))\r\nNIL\r\n"),
            "pred list; got {:?}",
            got
        );
        assert!(
            got.contains("(point-x p)\r\n99\r\n"),
            "setter x; got {:?}",
            got
        );
        assert!(
            got.contains("(line-a L)\r\n11\r\n"),
            "line a; got {:?}",
            got
        );
        assert!(
            got.contains("(line-b L)\r\n22\r\n"),
            "line b; got {:?}",
            got
        );
        assert!(
            got.contains("(line? L)\r\nT\r\n"),
            "line pred; got {:?}",
            got
        );
        assert!(
            got.contains("(point? L)\r\nNIL\r\n"),
            "point? L; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_q88_fixed_point() {
    // Q8.8 fixed-point stdlib (re-enabled with 2 KB sym pool + auto-GC).
    // Raw value is int32 interpreted as value/256.  + and - work directly
    // on raw values; q* / q/ scale by 256.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(q-from 3)\r"); // 768  (3.0)
        send(b"(q-to (q-from 7))\r"); // 7
        send(b"(q* (q-from 2) (q-from 3))\r"); // 1536 (6.0)
        send(b"(q-to (q* (q-from 2) (q-from 3)))\r"); // 6
        send(b"(q-to (q/ (q-from 10) (q-from 4)))\r"); // 2  (truncated)
        send(b"(q-to (+ (q-from 1) (q-from 2)))\r"); // 3  (raw + works directly)
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(q-from 3)\r\n768\r\n"),
            "q-from; got {:?}",
            got
        );
        assert!(
            got.contains("(q-to (q-from 7))\r\n7\r\n"),
            "q-to roundtrip; got {:?}",
            got
        );
        assert!(
            got.contains("(q* (q-from 2) (q-from 3))\r\n1536\r\n"),
            "q*; got {:?}",
            got
        );
        assert!(
            got.contains("(q-to (q* (q-from 2) (q-from 3)))\r\n6\r\n"),
            "q* roundtrip; got {:?}",
            got
        );
        assert!(
            got.contains("(q-to (q/ (q-from 10) (q-from 4)))\r\n2\r\n"),
            "q/ roundtrip; got {:?}",
            got
        );
        assert!(
            got.contains("(q-to (+ (q-from 1) (q-from 2)))\r\n3\r\n"),
            "q+ roundtrip; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_gc_stress() {
    // Stress test: many macro expansions in one expression should force
    // alloc-time GC (the Hybrid-GC hook) to reclaim transient garbage
    // mid-evaluation, rather than exhausting the pair pool.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Deep mutual recursion that previously exhausted the pool after
        // ~500 iterations (each call allocates new bindings).
        send(b"(defun e? (n) (if (= n 0) t (o? (- n 1))))\r");
        send(b"(defun o? (n) (if (= n 0) nil (e? (- n 1))))\r");
        send(b"(e? 2000)\r"); // T (2000 deep mutual)
                              // Heavy case-macro invocation on a single line — each call rebuilds
                              // the expansion tree fresh.
        send(b"(defun cls (n) (case n ((0 1 2 3 4) 'low) ((5 6 7 8 9) 'mid) (t 'hi)))\r");
        send(b"(cls 2)\r"); // LOW
        send(b"(cls 7)\r"); // MID
        send(b"(cls 42)\r"); // HI
        send(b"(cls 1)\r"); // LOW
        send(b"(cls 8)\r"); // MID
        send(b"(cls 99)\r"); // HI
                             // Flood the pool with garbage from repeated cons, no explicit gc.
        send(b"(defun churn (n) (if (< n 1) 'done (progn (cons n n) (churn (- n 1)))))\r");
        send(b"(churn 500)\r"); // DONE — auto-GC must kick in
        std::thread::sleep(std::time::Duration::from_millis(3000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(e? 2000)\r\nT\r\n"),
            "mutual 2000; got {:?}",
            got
        );
        assert!(got.contains("(cls 2)\r\nLOW\r\n"), "cls 2; got {:?}", got);
        assert!(got.contains("(cls 7)\r\nMID\r\n"), "cls 7; got {:?}", got);
        assert!(got.contains("(cls 42)\r\nHI\r\n"), "cls 42; got {:?}", got);
        assert!(got.contains("(cls 99)\r\nHI\r\n"), "cls 99; got {:?}", got);
        assert!(
            got.contains("(churn 500)\r\nDONE\r\n"),
            "churn 500; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_hashtable() {
    // Hashtable stdlib: 8-bucket vector-backed alists.  Shadow-put semantics:
    // ht-put prepends; repeated puts of the same key leave old entries in
    // the bucket list (but auto-GC reclaims them between REPL lines).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(defvar H (make-ht))\r");
        send(b"(ht-put H 'alice 1)\r"); // 1
        send(b"(ht-put H 'bob 2)\r"); // 2
        send(b"(ht-put H 'carol 3)\r"); // 3
        send(b"(ht-get H 'alice)\r"); // 1
        send(b"(ht-get H 'bob)\r"); // 2
        send(b"(ht-get H 'carol)\r"); // 3
        send(b"(ht-get H 'dave)\r"); // NIL (missing key)
                                     // Shadow-put: latest wins.
        send(b"(ht-put H 'alice 99)\r"); // 99
        send(b"(ht-get H 'alice)\r"); // 99
                                      // Many entries to test bucket distribution.
        send(b"(ht-put H 'foo 10)\r");
        send(b"(ht-put H 'bar 20)\r");
        send(b"(ht-put H 'baz 30)\r");
        send(b"(ht-put H 'qux 40)\r");
        send(b"(+ (ht-get H 'foo) (+ (ht-get H 'bar) (+ (ht-get H 'baz) (ht-get H 'qux))))\r"); // 100
        std::thread::sleep(std::time::Duration::from_millis(2000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(ht-get H 'alice)\r\n1\r\n"),
            "ht-get alice; got {:?}",
            got
        );
        assert!(
            got.contains("(ht-get H 'bob)\r\n2\r\n"),
            "ht-get bob; got {:?}",
            got
        );
        assert!(
            got.contains("(ht-get H 'carol)\r\n3\r\n"),
            "ht-get carol; got {:?}",
            got
        );
        assert!(
            got.contains("(ht-get H 'dave)\r\nNIL\r\n"),
            "ht-get missing; got {:?}",
            got
        );
        assert!(
            got.contains("(ht-get H 'alice)\r\n99\r\n"),
            "ht-get shadowed; got {:?}",
            got
        );
        assert!(got.contains("\r\n100\r\n"), "sum 100; got {:?}", got);

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_rand_seed() {
    // (rand) / (seed n): xorshift32 PRNG.  Returns 0..16383 fixnum.
    // (seed n) is deterministic — same seed yields same sequence.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Seed and capture two values.
        send(b"(seed 42)\r"); // 42
        send(b"(defvar a1 (rand))\r");
        send(b"(defvar a2 (rand))\r");
        // Re-seed and verify same sequence.
        send(b"(seed 42)\r");
        send(b"(eq (rand) a1)\r"); // T
        send(b"(eq (rand) a2)\r"); // T
                                   // Different seed → different sequence.
        send(b"(seed 43)\r");
        send(b"(eq (rand) a1)\r"); // NIL (most likely)
                                   // Values fall in 0..16383.
        send(b"(seed 1)\r");
        send(b"(defvar r1 (rand))\r");
        send(b"(< r1 16384)\r"); // T
        send(b"(< -1 r1)\r"); // T
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(eq (rand) a1)\r\nT\r\n"),
            "reproducible a1; got {:?}",
            got
        );
        assert!(
            got.contains("(eq (rand) a2)\r\nT\r\n"),
            "reproducible a2; got {:?}",
            got
        );
        assert!(
            got.contains("(eq (rand) a1)\r\nNIL\r\n"),
            "different seed differs; got {:?}",
            got
        );
        assert!(
            got.contains("(< r1 16384)\r\nT\r\n"),
            "range upper; got {:?}",
            got
        );
        assert!(
            got.contains("(< -1 r1)\r\nT\r\n"),
            "range lower; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_tick() {
    // (tick): reads the low 14 bits of the host cycle counter via MMIO at
    // $FF02-$FF03.  Successive calls produce different values as the CPU
    // advances.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        send(b"(defvar t1 (tick))\r");
        send(b"(defvar t2 (tick))\r");
        // Successive ticks differ (cycles accumulated between the two reads).
        send(b"(eq t1 t2)\r"); // NIL (almost certainly)
                               // Seeding with tick.
        send(b"(seed (tick))\r");
        send(b"(defvar r (rand))\r");
        send(b"(< r 16384)\r"); // T
        send(b"(< -1 r)\r"); // T
        std::thread::sleep(std::time::Duration::from_millis(1000));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("(eq t1 t2)\r\nNIL\r\n"),
            "tick advances; got {:?}",
            got
        );
        assert!(
            got.contains("(< r 16384)\r\nT\r\n"),
            "rand range; got {:?}",
            got
        );
        assert!(
            got.contains("(< -1 r)\r\nT\r\n"),
            "rand non-neg; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn lisp_multiline_input() {
    // Multi-line REPL input: when the prompt sees unbalanced parens at end
    // of a line, it continues with `>> ` and reads more input. Also checks
    // that `;` comments terminate at the line boundary and that strings
    // with inner `)` don't break the balance count.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/lisp/lisp.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(20));
            if String::from_utf8_lossy(&UART_BUF).contains("> ") {
                break;
            }
        }
        let send = |line: &[u8]| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        };
        // Simple two-line expression.
        send(b"(+ 1\r");
        send(b"2)\r"); // -> 3
                       // defun split across three lines.
        send(b"(defun sq (n)\r");
        send(b"  (* n n))\r"); // -> SQ
        send(b"(sq 9)\r"); // -> 81
                           // Comment on the first line; body on the second line.
        send(b"(+ 10 ; trailing comment\r");
        send(b"20)\r"); // -> 30
                        // A string containing `)` should not unbalance.
        send(b"(string-length\r");
        send(b"  \"a)b)c\")\r"); // -> 5
        std::thread::sleep(std::time::Duration::from_millis(1500));
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains(">> "),
            "should emit >> continuation prompt; got {:?}",
            got
        );
        assert!(got.contains("\n3\r\n"), "(+ 1 2) multi-line; got {:?}", got);
        assert!(got.contains("\nSQ\r\n"), "defun multi-line; got {:?}", got);
        assert!(got.contains("\n81\r\n"), "(sq 9) -> 81; got {:?}", got);
        assert!(
            got.contains("\n30\r\n"),
            "comment across lines; got {:?}",
            got
        );
        assert!(
            got.contains("\n5\r\n"),
            "string with inner ) ; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_arithmetic() {
    // Spot-check each primitive arithmetic op from the REPL.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        let send = |line: &[u8], want_ok: usize| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(12));
            }
            for _ in 0..200 {
                std::thread::sleep(std::time::Duration::from_millis(10));
                if String::from_utf8_lossy(&UART_BUF).matches("ok").count() >= want_ok {
                    break;
                }
            }
        };
        send(b"3 4 + .\r", 1);
        send(b"10 3 - .\r", 2);
        assert_eq!(emfe_stop(h), EmfeResult::Ok);
        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("7  ok"),
            "3 4 + . should print 7; got {:?}",
            got
        );
        assert!(
            got.contains("7  ok\r\n") && !got.contains("1927"),
            "10 3 - . should print 7 (not 1927); got {:?}",
            got
        );
        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_variable_constant_string() {
    // VARIABLE/CONSTANT store-and-fetch, `."`, and `(` comment.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }

        let send = |line: &[u8], want_ok: usize| {
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(12));
            }
            for _ in 0..200 {
                std::thread::sleep(std::time::Duration::from_millis(10));
                if String::from_utf8_lossy(&UART_BUF).matches("ok").count() >= want_ok {
                    break;
                }
            }
        };

        send(b"VARIABLE CNT\r", 1);
        send(b"42 CNT !\r", 2);
        send(b"CNT @ .\r", 3);
        send(b"100 CONSTANT HUN\r", 4);
        send(b"HUN .\r", 5);
        send(b": HI .\" hi world\" ;\r", 6);
        send(b"HI\r", 7);
        send(b"( ignored comment ) 99 .\r", 8);

        assert_eq!(emfe_stop(h), EmfeResult::Ok);
        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("42  ok"),
            "CNT @ . should print 42; got {:?}",
            got
        );
        assert!(
            got.contains("100  ok"),
            "HUN . should print 100; got {:?}",
            got
        );
        assert!(
            got.contains("hi world ok"),
            r#"." should print 'hi world'; got {:?}"#,
            got
        );
        assert!(
            got.contains("99  ok"),
            "99 . should print 99 after comment; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_repl_dot() {
    // REPL: send "42 .\r", expect the echoed line followed by "42  ok\r\n"
    // (print_dec's trailing space + QUIT's " ok" suffix).
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);

        assert_eq!(emfe_run(h), EmfeResult::Ok);
        // Wait for the banner so ACCEPT is ready.
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }
        for ch in b"42 .\r" {
            assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if String::from_utf8_lossy(&UART_BUF).contains("ok") {
                break;
            }
        }
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            got.contains("42 .\r\n42  ok\r\n"),
            "expected REPL to echo \"42 .\" and print \"42  ok\"; got {:?}",
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_kernel_banner() {
    // Load the Tiny Forth kernel, verify the banner appears.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);

        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 28 {
                break;
            }
        }
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let banner = String::from_utf8_lossy(&UART_BUF).into_owned();
        assert!(
            banner.starts_with("Hha Forth"),
            "banner missing; got {:?}",
            banner
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn forth_new_features() {
    // Comprehensive coverage of every feature added beyond the v1 kernel,
    // plus every documented example from docs/USER_GUIDE.md §5.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);
        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..50 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 30 {
                break;
            }
        }

        // send() tracks a running "ok" counter; each line we send should
        // normally produce one " ok" at end-of-line.
        let mut ok_count: usize = 0;
        let mut send = |line: &[u8]| {
            ok_count += 1;
            for ch in line {
                assert_eq!(emfe_send_char(h, *ch as c_char), EmfeResult::Ok);
                std::thread::sleep(std::time::Duration::from_millis(3));
            }
            for _ in 0..500 {
                std::thread::sleep(std::time::Duration::from_millis(8));
                if String::from_utf8_lossy(&UART_BUF).matches("ok").count() >= ok_count {
                    break;
                }
            }
        };

        // ---- 16-bit arithmetic (pre-existing additions) --------------
        send(b"6 4 * .\r"); // 24
        send(b"21 4 / .\r"); // 5
        send(b"21 4 MOD .\r"); // 1
        send(b"21 4 /MOD . .\r"); // 5 1  (quot rem — TOS printed first)
        send(b"5 1+ .\r"); // 6
        send(b"5 1- .\r"); // 4
        send(b"5 2+ .\r"); // 7
        send(b"5 2- .\r"); // 3
        send(b"5 2* .\r"); // 10
        send(b"5 2/ .\r"); // 2
        send(b"-7 ABS .\r"); // 7
        send(b"3 7 MIN .\r"); // 3
        send(b"3 7 MAX .\r"); // 7
        send(b"0 NOT .\r"); // -1
        send(b"1 NOT .\r"); // 0
        send(b"1 2 <> .\r"); // -1
        send(b"1 1 <> .\r"); // 0
        send(b"3 2 > .\r"); // -1
        send(b"2 3 > .\r"); // 0

        // ---- Stack operations ---------------------------------------
        send(b"5 ?DUP + .\r"); // 10
        send(b"0 ?DUP 99 .\r"); // 99 (0 not duped)
        send(b"1 2 3 NIP .\r"); // 3
        send(b"1 2 TUCK . . .\r"); // "2 1 2"
        send(b"11 22 33 44 2 PICK .\r"); // 22
        send(b"11 22 33 44 0 PICK .\r"); // 44 (= DUP)
        send(b"1 2 2DUP + . + .\r"); // 3 3  (2DUP→(1 2 1 2), +→(1 2 3), .→3, +→3, .→3) ... actually let me retrace
                                     // Trace: stack (1 2). 2DUP→(1 2 1 2). +→(1 2 3). .→prints "3 ",pops→(1 2). +→(3). .→prints "3 ",pops→().
        send(b"1 2 3 4 2DROP . .\r"); // 2 1
        send(b"1 2 3 4 2SWAP . . . .\r"); // 2 1 4 3  (after 2SWAP stack=(3 4 1 2), pops: 2,1,4,3)
        send(b"1 2 3 4 2OVER . . . . . .\r"); // 2 1 4 3 2 1  (stack becomes (1 2 3 4 1 2), pops top first)

        // ---- Newly added FORTH-83 words ------------------------------
        send(b"1 2 3 -ROT . . .\r"); // -ROT: (1 2 3) → (3 1 2); . . . pops 2 1 3
        send(b"11 22 33 44 55 2 ROLL . . . . .\r"); // ROLL 2: moves xN at depth 2 to top. Expected: 33 55 44 22 11
        send(b"1 2 3 DEPTH .\r"); // depth counts cells before DEPTH ran: 3
        send(b"DROP DROP DROP DROP\r"); // clean
        send(b"5 0> .\r"); // -1
        send(b"-5 0> .\r"); // 0
        send(b"0 0> .\r"); // 0
        send(b"VARIABLE SBUF 6 ALLOT\r");
        send(b": DO-CMOVE S\" HELLO!\" SBUF SWAP CMOVE SBUF 6 TYPE ; DO-CMOVE\r");
        send(b"VARIABLE MBUF 10 ALLOT\r");
        send(b": DO-MOVE S\" ABCDEF\" MBUF SWAP MOVE MBUF 6 TYPE ; DO-MOVE\r");
        send(b": USEPLUS 3 4 ['] + EXECUTE ; USEPLUS .\r"); // 7
        send(b"1 2 3 4 5 ABORT\r"); // stack cleared; banner reprints, REPL continues
        send(b"123 .\r"); // should still work after ABORT

        // ---- CREATE / DOES> -------------------------------------------
        // CREATE with ALLOT for a counted buffer
        send(b"CREATE BYTES 4 ALLOT\r");
        send(b"BYTES 4 65 FILL BYTES 4 TYPE\r"); // fills with 'A' and prints "AAAA"
                                                 // CREATE / DOES> defining a parameterised accessor
        send(b": CONST2 CREATE , , DOES> 2@ ;\r");
        send(b"11 22 CONST2 PAIR\r"); // PAIR stores 11, 22; DOES> will read them
        send(b"PAIR . .\r"); // 11 22 (2@: low=22 NOS, high=11 TOS, . pops 11 then 22)
                             // Incremental build-up of an ARRAY defining word.
        send(b": MKBUF CREATE 10 ALLOT ;\r"); // plain CREATE, no DOES>
        send(b"MKBUF XBUF\r");
        send(b"99 XBUF !\r");
        send(b"XBUF @ .\r"); // 99
                             // Simplest possible CREATE/DOES> — no args, hard-coded +1 behavior.
        send(b": ONEX CREATE DOES> 1 + ;\r");
        send(b"ONEX ZZ ZZ ZZ - .\r"); // -1 is expected (addr vs addr+1 from DOES>, but rhs is evaluated first)
        send(b": ARRAY CREATE CELLS ALLOT DOES> SWAP 2 * + ;\r");
        send(b"5 ARRAY BUFX\r");
        send(b"42 0 BUFX !\r");
        send(b"99 4 BUFX !\r");
        send(b"0 BUFX @ . 4 BUFX @ .\r"); // 42 99

        // ---- Pictured numeric output ----------------------------------
        // 1234 as decimal via <# #S #>
        send(b": PNO0 <# #S #> TYPE ;\r");
        send(b"1234 0 PNO0\r"); // "1234"
                                // With sign
        send(b": PNOS DUP ABS 0 <# #S ROT SIGN #> TYPE ;\r");
        send(b"-999 PNOS\r"); // "-999"
                              // HOLD: wrap digits (65 = 'A' in decimal mode, which is the default)
        send(b": PNOH <# 65 HOLD #S 65 HOLD #> TYPE ;\r");
        send(b"77 0 PNOH\r"); // "A77A"

        // D.R: signed double right-justified
        send(b"12345 0 8 D.R 99 .\r"); // "   12345" then "99"
        send(b"-12345 -1 8 D.R 99 .\r"); // "  -12345" then "99"

        // ABORT" that doesn't trigger (flag is 0)
        send(b": SAFE 0 ABORT\" never shown\" 55 . ; SAFE\r"); // "55"
                                                               // ABORT" that triggers: prints message and aborts
        send(b": BOOM 1 ABORT\" fired\" 99 . ; BOOM\r"); // prints "fired" + banner + resumes
        send(b"66 .\r"); // still alive after ABORT"

        // ---- CHAR / [CHAR] --------------------------------------------
        send(b"CHAR A .\r"); // 65
        send(b": EMIT-STAR [CHAR] * EMIT ; EMIT-STAR EMIT-STAR\r"); // "**"

        // ---- String ops (S" is IMMEDIATE — wrap in : definitions) -----
        send(b": CMP1 S\" ABC\" S\" ABC\" COMPARE . ; CMP1\r"); // 0
        send(b": CMP2 S\" ABC\" S\" ABD\" COMPARE . ; CMP2\r"); // -1
        send(b": CMP3 S\" ABD\" S\" ABC\" COMPARE . ; CMP3\r"); // 1
        send(b": SKIP2 S\" HELLO\" 2 /STRING TYPE ; SKIP2\r"); // "LLO"
                                                               // -TRAILING: requires a buffer with actual trailing spaces
        send(b"VARIABLE SSB 8 ALLOT\r");
        send(b": MKSSB S\" AB   \" SSB SWAP CMOVE ; MKSSB\r");
        send(b"SSB 5 -TRAILING TYPE\r"); // "AB"

        // ---- FORGET / MARKER ------------------------------------------
        send(b"MARKER MK\r");
        send(b": TEMP1 100 ;\r");
        send(b": TEMP2 200 ;\r");
        send(b"TEMP1 TEMP2 + .\r"); // 300
        send(b"MK TEMP1 .\r"); // TEMP1 gone → "TEMP1?"
        send(b": KEEPME 777 ;\r");
        send(b"KEEPME .\r"); // 777
        send(b"FORGET KEEPME KEEPME .\r"); // "KEEPME?" after forget

        // ---- POSTPONE -------------------------------------------------
        send(b": MY-IF POSTPONE IF ; IMMEDIATE\r"); // wraps IF
        send(b": TEST-IF DUP 0< MY-IF NEGATE THEN ; -9 TEST-IF .\r"); // 9

        // ---- SM/REM / FM/MOD ------------------------------------------
        // SM/REM: -7 2 SM/REM → rem=-1 quot=-3 (truncation)
        send(b"-7 -1 2 SM/REM . .\r"); // quot=-3 rem=-1 → prints "-3 -1"
                                       // FM/MOD: -7 2 FM/MOD → rem=1 quot=-4 (floor)
        send(b"-7 -1 2 FM/MOD . .\r"); // quot=-4 rem=1 → prints "-4 1"

        // ---- M+ ------------------------------------------------------
        send(b"1000 0 500 M+ D.\r"); // 1500
        send(b"1000 0 -500 M+ D.\r"); // 500 (sign-extended negation)

        // ---- ERASE / BLANK -------------------------------------------
        send(b"VARIABLE EB 8 ALLOT\r");
        send(b"EB 10 0 FILL EB 10 ERASE EB C@ .\r"); // 0 (ERASE wrote 0 over FILL's 0s — still 0)
        send(b"EB 10 255 FILL EB 10 ERASE EB C@ .\r"); // 0 (ERASE overwrote 255 with 0)
        send(b"EB 10 0 FILL EB 10 BLANK EB C@ .\r"); // 32 (space)

        // ---- LSHIFT / RSHIFT ------------------------------------------
        send(b"1 4 LSHIFT .\r"); // 16
        send(b"256 8 RSHIFT .\r"); // 1
        send(b"-1 1 RSHIFT .\r"); // 32767 (zero-fill, not sign-extend)

        // ---- ALIGN / ALIGNED (trivial on byte-addressable kernel) -----
        // ALIGNED round up odd addresses
        send(b"101 ALIGNED .\r"); // 102
        send(b"100 ALIGNED .\r"); // 100 (already even)

        // ---- WORD ----------------------------------------------------
        // WORD uses a char delimiter.  Test inside a colon def so >IN is
        // advanced reliably.  Parse "HELLO" using ' ' (32) as delim.
        send(b": TESTW BL WORD COUNT TYPE ; TESTW HELLO\r"); // "HELLO"

        // ---- SP@ / SP! / RP@ / RP! -----------------------------------
        // SP@ pushes the current stack pointer (before pushing the result).
        send(b"1 2 3 SP@ 4 + SP@ - .\r"); // 4 + SP@_after - SP@_before = … actually tricky
                                          // Depth diff across SP@ itself.  First SP@ reads U_orig, push.
                                          // Second SP@ reads U_orig-2, push.  `-` does NOS - TOS = 2.
        send(b"SP@ SP@ - .\r"); // 2

        // ---- M/ ------------------------------------------------------
        send(b"-7 -1 2 M/ .\r"); // -4 (floored)
        send(b"7 0 2 M/ .\r"); // 3
        send(b"-1 -1 3 M/ .\r"); // ? -1 / 3 floored = -1... actually d = 0xFFFF_FFFF = -1 as double; -1/3 floored = -1

        // ---- SPAN / EXPECT / QUERY are hard to test automatically
        // because they read additional input lines; skip.
        // Just verify SPAN at least returns an address (not 0).
        send(b"SPAN 0= .\r"); // 0 (SPAN returns non-zero addr)

        // ---- FIND ----------------------------------------------------
        // FIND takes a counted string. Easiest to build via HERE.
        // Write a counted name "DUP" at HERE transiently, then FIND it.
        send(b": TEST-FIND HERE 3 , CHAR D C, CHAR U C, CHAR P C, HERE 8 - FIND . . ; TEST-FIND\r");
        // Actually easier: use WORD which leaves counted string at HERE.
        send(b": FFIND BL WORD FIND SWAP DROP . ; FFIND DUP\r"); // finds DUP → flag=1
        send(b": FMISS BL WORD FIND SWAP DROP . ; FMISS NOPE\r"); // NOPE not found → 0

        // ---- VOCABULARY / DEFINITIONS / ONLY / FORTH ------------------
        send(b"VOCABULARY MYVOC\r");
        send(b"MYVOC\r"); // switch to MYVOC
        send(b": PRIVATE 42 ;\r"); // defined in MYVOC
        send(b"PRIVATE .\r"); // 42 — MYVOC has PRIVATE
        send(b"FORTH\r"); // back to FORTH
        send(b"PRIVATE .\r"); // PRIVATE? — gone from FORTH's search
        send(b"MYVOC PRIVATE .\r"); // 42 again in MYVOC
        send(b"FORTH\r");
        // CONTEXT / CURRENT return addresses (non-zero)
        send(b"CONTEXT 0= .\r"); // 0
        send(b"CURRENT 0= .\r"); // 0
        send(b"DEFINITIONS 55 .\r"); // 55 (DEFINITIONS no-op)
        send(b"ONLY 66 .\r"); // 66 (ONLY switches to FORTH)

        // ---- Comparisons ---------------------------------------------
        send(b"3 5 U< .\r"); // -1
        send(b"5 3 U< .\r"); // 0
        send(b"5 3 U> .\r"); // -1
        send(b"3 5 U> .\r"); // 0
        send(b"-1 1 U< .\r"); // 0  (-1 = 0xFFFF > 1 unsigned)

        // ---- Memory --------------------------------------------------
        send(b"VARIABLE CNT 0 CNT ! 7 CNT +! 5 CNT +! CNT @ .\r"); // 12
        send(b"10 CELLS .\r"); // 20
        send(b"HERE CELL+ HERE - .\r"); // 2
                                        // CMOVE + FILL + 2@ + 2!
        send(b"VARIABLE BUF 30 ALLOT\r");
        send(b"BUF 32 64 FILL BUF C@ .\r"); // 64 (= '@')
        send(b"BUF 32 0 FILL BUF C@ .\r"); // 0
        send(b"42 17 BUF 2! BUF 2@ . .\r"); // 17 42  (TOS=hi=17 printed first, then lo=42)

        // ---- Constants ------------------------------------------------
        send(b"TRUE .\r"); // -1
        send(b"FALSE .\r"); // 0
        send(b"BL .\r"); // 32

        // ---- Number formatting & BASE --------------------------------
        send(b"HEX FF . DECIMAL\r"); // "FF"
        send(b"HEX ABCD . DECIMAL\r"); // "ABCD"
        send(b"HEX abcd . DECIMAL\r"); // lowercase digits accepted
        send(b"-1 U.\r"); // 65535
        send(b"5 4 .R 99 .\r"); // "   5" then "99" (width=4 right-justified)
        send(b"65000 6 U.R 99 .\r"); // " 65000" then "99"
        send(b"3 SPACES 99 .\r"); // "   " then "99"

        // ---- DO / LOOP / +LOOP / I / J / LEAVE -----------------------
        send(b": SUM10 0 10 0 DO I + LOOP ; SUM10 .\r"); // 45
        send(b": TEN 10 0 DO I . LOOP ; TEN\r"); // 0 1 2 3 4 5 6 7 8 9
        send(b": CD10 0 10 DO I . -1 +LOOP ; CD10\r"); // 10 9 8 7 6 5 4 3 2 1
        send(b": FIVE 10 0 DO I . I 4 = IF LEAVE THEN LOOP ; FIVE\r"); // 0 1 2 3 4
        send(b": NEST 3 1 DO 3 1 DO J I 10 * + . LOOP LOOP ;\r");
        send(b"NEST\r"); // 11 12 21 22

        // ---- BEGIN / WHILE / REPEAT ----------------------------------
        send(b": CDW 5 BEGIN DUP 0> WHILE DUP . 1- REPEAT DROP ; CDW\r"); // 5 4 3 2 1

        // ---- S" / TYPE -----------------------------------------------
        send(b": SHOUT S\" HELLO\" TYPE ; SHOUT 42 .\r"); // HELLO then 42

        // ---- Mixed precision / double --------------------------------
        // Use operand < 32768 so M* treats it as positive 16-bit signed.
        send(b"20000 7 M* D.\r"); // 140000
        send(b"-20000 7 M* D.\r"); // -140000
        send(b"1000 1000 UM* SWAP . .\r"); // 16960 15
        send(b"0 1 100 UM/MOD SWAP . .\r"); // rem=36 quot=655 → prints "655 36"
        send(b"65000 0 100 0 D+ D.\r"); // 65100
        send(b"100 0 50 0 D- D.\r"); // 50
        send(b"-1 -1 DABS D.\r"); // 1
        send(b"100 0 DNEGATE D.\r"); // -100
        send(b"12345 37 100 */ .\r"); // 4567
        send(b"100 7 3 */MOD . .\r"); // quot=233 rem=1 → prints "233 1"

        // ---- Compile-time tools --------------------------------------
        send(b"3 4 ' + EXECUTE .\r"); // 7
        send(b": FACT DUP 1 > IF DUP 1- RECURSE * THEN ; 6 FACT .\r"); // 720
        send(b"( this is a comment ) 88 .\r"); // 88
        send(b"55 \\ trailing line comment\r"); // 55 lands on stack, \ skips rest
        send(b"DROP 77 .\r"); // pop the 55 then 77

        // ---- Debug (output is noisy so we just ensure they don't hang)
        send(b"1 2 3 .S DROP DROP DROP\r");
        send(b"HEX C000 20 DUMP DECIMAL\r");
        send(b"WORDS\r");

        assert_eq!(emfe_stop(h), EmfeResult::Ok);
        let got = String::from_utf8_lossy(&UART_BUF).into_owned();

        // Per-line spot checks.  `.` emits "<value> " (trailing space),
        // and QUIT appends " ok".  We look for the combined pattern where
        // convenient, otherwise just for the phrase.
        let expect: &[&str] = &[
            // arithmetic
            "24  ok",  // 6 4 *
            "5  ok",   // 21 4 /
            "1  ok",   // 21 4 MOD  (also used elsewhere — OK as long as present)
            "5 1  ok", // 21 4 /MOD . .  → TOS (quot=5) first, then rem=1
            "6  ok",   // 1+
            "4  ok",   // 1-
            "7  ok",   // 2+
            "3  ok",   // 2-, and also NIP example
            "10  ok",  // 2* 5 and ?DUP+ 5
            "2  ok",   // 2/ 5, and HERE CELL+ diff
            "7  ok",   // ABS -7, also 3 7 MAX? No max gives 7 too
            "-1  ok",  // NOT 0, or <>
            "0  ok",   // NOT 1, etc
            // stack
            "2 1 3  ok", // -ROT on (1 2 3): stack → (3 1 2), . . . pops 2, 1, 3 → "2 1 3"
            "33 55 44 22 11  ok", // ROLL 2 on (11 22 33 44 55): moves 33 to top → (11 22 44 55 33), . . . . . pops 33,55,44,22,11
            "3  ok",              // DEPTH after 1 2 3 → 3 (depth counts the 3 cells already there)
            "HELLO! ok",          // CMOVE result "HELLO!" via TYPE (TYPE has no trailing space)
            "ABCDEF ok",          // MOVE result "ABCDEF" via TYPE
            "7  ok",              // USEPLUS: [' ] + compiled + EXECUTE → 7
            "123  ok",            // REPL alive after ABORT
            // CREATE / DOES>
            "AAAA ok",   // CREATE + ALLOT + FILL + TYPE
            "11 22  ok", // CONST2 PAIR: `, ,` pops 22 then 11 → mem low=22, high=11.  PAIR 2@ → (22 11), . . prints 11 22.
            "42 99  ok", // ARRAY indexed cells
            // Pictured numeric output
            "1234 ok",         // PNO0 (TYPE has no trailing space)
            "-999 ok",         // PNOS
            "A77A ok",         // PNOH
            "   1234599  ok",  // D.R has no trailing space, so "   12345" then "99 " from the `. `
            "  -1234599  ok",  // D.R negative
            "55  ok",          // SAFE with flag=0 doesn't abort
            "fired",           // BOOM fired its message
            "66  ok",          // REPL alive after ABORT"
            "65  ok",          // CHAR A → 65
            "**",              // EMIT-STAR twice
            "0  ok",           // COMPARE ABC ABC
            "-1  ok",          // COMPARE ABC ABD
            "1  ok",           // COMPARE ABD ABC
            "LLO ok",          // /STRING skip 2
            "AB ok",           // -TRAILING
            "300  ok",         // TEMP1 + TEMP2
            "TEMP1?",          // after MK: TEMP1 should be gone
            "777  ok",         // KEEPME defined after MK
            "KEEPME?",         // FORGET KEEPME removed it
            "9  ok",           // TEST-IF with POSTPONE-wrapped IF
            "-3 -1  ok",       // SM/REM -7 2 → quot -3, rem -1
            "-4 1  ok",        // FM/MOD -7 2 → quot -4, rem 1
            "1500  ok",        // M+ 1000 + 500
            "500  ok",         // M+ 1000 + (-500) using sign-extension
            "16  ok",          // 1 LSHIFT 4
            "1  ok",           // 256 RSHIFT 8 (appears many times in output, OK)
            "32767  ok",       // -1 RSHIFT 1 (zero-fill)
            "102  ok",         // 101 ALIGNED
            "100  ok",         // 100 ALIGNED (already even)
            "HELLO ok",        // WORD parses "HELLO" + COUNT + TYPE — appears elsewhere too
            "32  ok",          // BLANK result (BL also prints 32, present already)
            "2  ok",           // SP@ SP@ - → NOS(=U_orig) - TOS(=U_orig-2) = 2
            "-4  ok",          // M/ floored: -7/2 = -4
            "3  ok",           // M/: 7/2 = 3 (already present elsewhere)
            "1  ok",           // FFIND finds DUP → flag=1
            "0  ok",           // FMISS NOPE not found → flag=0 (already present)
            "42  ok",          // PRIVATE in MYVOC returns 42
            "PRIVATE?",        // PRIVATE is not in FORTH → undefined
            "55  ok",          // DEFINITIONS no-op; 55 . still works
            "66  ok",          // ONLY switches to FORTH; 66 . still works
            "99  ok",          // many 99 . markers
            "22  ok",          // PICK 2
            "44  ok",          // PICK 0
            "2 1 2  ok",       // TUCK
            "2 1  ok",         // 2DROP leftover
            "2 1 4 3  ok",     // 2SWAP
            "2 1 4 3 2 1  ok", // 2OVER
            // comparisons
            // memory
            "12  ok",    // +! cumulative
            "20  ok",    // 10 CELLS
            "64  ok",    // FILL with '@'
            "17 42  ok", // 2@
            // constants
            "32  ok", // BL
            // formatting
            "FF  ok",
            "-5433  ok", // HEX ABCD . treats 0xABCD as signed -21555 → "-5433" in hex
            "65535  ok",
            "   599  ok", // .R width=4 + 99 . — no space between (.R has no trailing space)
            " 6500099  ok", // U.R width=6 + 99 .
            "   99  ok",  // 3 SPACES + 99 .
            // loops
            "45  ok",                     // SUM10
            "0 1 2 3 4 5 6 7 8 9  ok",    // TEN
            "10 9 8 7 6 5 4 3 2 1 0  ok", // CD10 (+LOOP): step=-1 stops when index<limit=0 → prints 0 too
            "0 1 2 3 4  ok",              // FIVE (LEAVE)
            "11 21 12 22  ok", // NEST: `J I 10 * +` = J + I*10, so I (inner) is the tens digit
            "5 4 3 2 1  ok",   // BEGIN/WHILE/REPEAT
            // strings
            "HELLO42  ok", // SHOUT then 42 . (no CR between)
            // mixed / double
            "140000  ok",
            "-140000  ok",
            "16960 15  ok",
            "36 655  ok", // UM/MOD leaves ( rem quot ) with quot on TOS; . . pops rem first then quot → "rem quot" reversed? actually TOS first: quot then rem
            "65100  ok",
            "50  ok",
            "1  ok", // DABS of -1_-1 double = 1
            "-100  ok",
            "4567  ok",
            "233 1  ok", // */MOD: quot=233 rem=1, . . prints quot then rem → "233 1"
            // compile-time
            "720  ok", // FACT 6
            "88  ok",  // comment
            "77  ok",  // line-comment survivors
        ];
        let mut failed = Vec::<&&str>::new();
        for pat in expect.iter() {
            if !got.contains(pat) {
                failed.push(pat);
            }
        }
        assert!(
            failed.is_empty(),
            "missing {} patterns:\n  {:?}\n\n--- full output ---\n{}",
            failed.len(),
            failed,
            got
        );

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn stack_s19_emits_descent_and_ascent_markers() {
    // stack.s19 recurses 6 levels deep and emits "EN\r\n" going in and
    // "XN\r\n" coming out. Verifies both the ACIA pipeline and the shadow
    // call stack tolerate deep BSR/RTS chains.
    let _guard = TEST_SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut h: EmfeInstance = ptr::null_mut();
    assert_eq!(emfe_create(&mut h), EmfeResult::Ok);
    unsafe {
        UART_BUF.clear();
        assert_eq!(
            emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut()),
            EmfeResult::Ok
        );
        let path = std::ffi::CString::new("examples/stack/stack.s19").unwrap();
        assert_eq!(emfe_load_srec(h, path.as_ptr()), EmfeResult::Ok);

        assert_eq!(emfe_run(h), EmfeResult::Ok);
        for _ in 0..100 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            if UART_BUF.len() >= 48 {
                break;
            }
        }
        assert_eq!(emfe_stop(h), EmfeResult::Ok);

        let got = String::from_utf8_lossy(&UART_BUF).into_owned();
        let expected = "E6\r\nE5\r\nE4\r\nE3\r\nE2\r\nE1\r\nX1\r\nX2\r\nX3\r\nX4\r\nX5\r\nX6\r\n";
        assert_eq!(got, expected, "stack.s19 output mismatch");

        assert_eq!(emfe_destroy(h), EmfeResult::Ok);
    }
}

#[test]
fn ffi_catches_panic_on_null_handle_passthrough() {
    // Verify that functions guarding against null handle via ErrInvalid still
    // work — this is the simplest check that the ffi_catch wrapper doesn't
    // break the happy path.
    assert_eq!(emfe_destroy(ptr::null_mut()), EmfeResult::ErrInvalid);
    assert_eq!(
        unsafe { emfe_poke_byte(ptr::null_mut(), 0, 0) },
        EmfeResult::ErrInvalid
    );
    assert_eq!(
        unsafe { emfe_step(ptr::null_mut()) },
        EmfeResult::ErrInvalid
    );
}

#[test]
fn settings_save_load_roundtrip() {
    // Stage a change, apply+save, destroy, recreate, load, and verify that
    // the committed value survives across instance lifetimes and that
    // `applied` is synced so no pending indicator would fire.
    let tmp = std::env::temp_dir().join("emfe_plugin_mc6809_test_roundtrip");
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).unwrap();

    let dir_c = std::ffi::CString::new(tmp.to_string_lossy().as_bytes()).unwrap();
    let key_base = std::ffi::CString::new("ConsoleBase").unwrap();
    let val_new = std::ffi::CString::new("0xE000").unwrap();
    let key_applied = std::ffi::CString::new("ConsoleBase").unwrap();

    unsafe {
        assert_eq!(emfe_set_data_dir(dir_c.as_ptr()), EmfeResult::Ok);

        let mut h1: EmfeInstance = ptr::null_mut();
        assert_eq!(emfe_create(&mut h1), EmfeResult::Ok);
        assert_eq!(
            emfe_set_setting(h1, key_base.as_ptr(), val_new.as_ptr()),
            EmfeResult::Ok
        );
        assert_eq!(emfe_apply_settings(h1), EmfeResult::Ok);
        assert_eq!(emfe_save_settings(h1), EmfeResult::Ok);
        assert_eq!(emfe_destroy(h1), EmfeResult::Ok);

        let mut h2: EmfeInstance = ptr::null_mut();
        assert_eq!(emfe_create(&mut h2), EmfeResult::Ok);
        assert_eq!(emfe_load_settings(h2), EmfeResult::Ok);

        let staged_ptr = emfe_get_setting(h2, key_base.as_ptr());
        let staged = std::ffi::CStr::from_ptr(staged_ptr)
            .to_string_lossy()
            .into_owned();
        assert_eq!(staged, "0xE000");

        let applied_ptr = emfe_get_applied_setting(h2, key_applied.as_ptr());
        let applied = std::ffi::CStr::from_ptr(applied_ptr)
            .to_string_lossy()
            .into_owned();
        assert_eq!(applied, "0xE000", "applied must match committed after load");

        assert_eq!(emfe_destroy(h2), EmfeResult::Ok);
    }

    let _ = std::fs::remove_dir_all(&tmp);
}
