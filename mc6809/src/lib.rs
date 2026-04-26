// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// emfe_plugin_mc6809/src/lib.rs
//
// MC6809 emfe plugin — wraps the em6809 CPU core + a minimal memory-mapped
// console device, exporting the emfe_plugin.h C ABI via cdylib.
//
// Matches the Phase 1 feature set of emfe_plugin_em8 / emfe_plugin_z8000.

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(clippy::missing_safety_doc)]

use std::any::Any;
use std::collections::VecDeque;
use std::ffi::{c_char, c_void, CStr, CString};
use std::fs;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::thread::JoinHandle;

use em6809::bus::Bus;
use em6809::cpu::Cpu;

// ===========================================================================
// FFI panic-safety wrapper
// ---------------------------------------------------------------------------
// Every `#[no_mangle] pub extern "C" fn` below must be wrapped in
// `ffi_catch!(default, body)` so that a panic inside the plugin (or inside
// `em6809`) is caught at the FFI boundary instead of unwinding across it —
// unwinding across a C ABI is Undefined Behaviour.
//
// On panic, the caller receives the provided `default` value. Host front-ends
// can surface the most recent panic text via `emfe_get_last_error` because
// the macro stores the panic message into `LAST_PANIC_MSG` before returning.
// ===========================================================================

static LAST_PANIC_MSG: OnceLock<Mutex<Option<CString>>> = OnceLock::new();

fn record_panic(msg: &str) {
    let cell = LAST_PANIC_MSG.get_or_init(|| Mutex::new(None));
    if let Ok(mut g) = cell.lock() {
        *g = CString::new(format!("panic: {}", msg)).ok();
    }
}

fn panic_payload_str(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&'static str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    }
}

macro_rules! ffi_catch {
    ($default:expr, $body:block) => {{
        match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| $body)) {
            Ok(v) => v,
            Err(payload) => {
                record_panic(&panic_payload_str(payload));
                $default
            }
        }
    }};
}

// ===========================================================================
// emfe_plugin.h — C ABI types (mirrored in Rust)
// ===========================================================================

pub const EMFE_API_VERSION_MAJOR: u32 = 1;
pub const EMFE_API_VERSION_MINOR: u32 = 0;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeNegotiateInfo {
    pub api_version_major: u32,
    pub api_version_minor: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum EmfeResult {
    Ok = 0,
    ErrInvalid = -1,
    ErrState = -2,
    ErrNotFound = -3,
    ErrIo = -4,
    ErrMemory = -5,
    ErrUnsupported = -6,
}

#[repr(C)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum EmfeState {
    Stopped = 0,
    Running = 1,
    Halted = 2,
    Stepping = 3,
}

#[repr(C)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum EmfeStopReason {
    None = 0,
    User = 1,
    Breakpoint = 2,
    Watchpoint = 3,
    Step = 4,
    Halt = 5,
    Exception = 6,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub enum EmfeRegType {
    Int = 0,
    Float = 1,
    Float80 = 2,
}

// Register flags (bitmask)
const EMFE_REG_FLAG_NONE: u32 = 0;
const EMFE_REG_FLAG_READONLY: u32 = 1 << 0;
const EMFE_REG_FLAG_PC: u32 = 1 << 1;
const EMFE_REG_FLAG_SP: u32 = 1 << 2;
const EMFE_REG_FLAG_FLAGS: u32 = 1 << 3;
const EMFE_REG_FLAG_HIDDEN: u32 = 1 << 6;

const EMFE_SETTING_FLAG_REQUIRES_RESET: u32 = 1 << 0;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeRegisterDef {
    pub reg_id: u32,
    pub name: *const c_char,
    pub group: *const c_char,
    pub type_: EmfeRegType,
    pub bit_width: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeRegFlagBitDef {
    pub bit_index: u8,
    pub label: *const c_char,
}

// EmfeRegFlagBitDef carries a *const c_char which makes the type !Sync by
// default. Each label points to an immutable static byte array (b"E\0"
// etc.), so concurrent reads are safe. Marking Sync explicitly so the
// type can be used in static [EmfeRegFlagBitDef; N] arrays.
unsafe impl Sync for EmfeRegFlagBitDef {}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeRegViewDep {
    pub reg_id: u32,
    pub shift: u8,
    pub width: u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union EmfeRegValueUnion {
    pub u64_: u64,
    pub f64_: f64,
    pub f80: [u8; 10],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeRegValue {
    pub reg_id: u32,
    pub value: EmfeRegValueUnion,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeDisasmLine {
    pub address: u64,
    pub raw_bytes: *const c_char,
    pub mnemonic: *const c_char,
    pub operands: *const c_char,
    pub length: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeBreakpointInfo {
    pub address: u64,
    pub enabled: bool,
    pub condition: *const c_char,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeBoardInfo {
    pub board_name: *const c_char,
    pub cpu_name: *const c_char,
    pub description: *const c_char,
    pub version: *const c_char,
    pub capabilities: u64,
}

// Capability flags — mirror of EMFE_CAP_* in emfe_plugin.h.
pub const EMFE_CAP_LOAD_ELF: u64 = 1 << 0;
pub const EMFE_CAP_LOAD_SREC: u64 = 1 << 1;
pub const EMFE_CAP_LOAD_BINARY: u64 = 1 << 2;
pub const EMFE_CAP_STEP_OVER: u64 = 1 << 3;
pub const EMFE_CAP_STEP_OUT: u64 = 1 << 4;
pub const EMFE_CAP_CALL_STACK: u64 = 1 << 5;
pub const EMFE_CAP_WATCHPOINTS: u64 = 1 << 6;
pub const EMFE_CAP_FRAMEBUFFER: u64 = 1 << 7;
pub const EMFE_CAP_INPUT_KEYBOARD: u64 = 1 << 8;
pub const EMFE_CAP_INPUT_MOUSE: u64 = 1 << 9;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeStateInfo {
    pub state: EmfeState,
    pub stop_reason: EmfeStopReason,
    pub stop_address: u64,
    pub stop_message: *const c_char,
}

#[repr(C)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum EmfeWatchpointSize {
    Byte = 1,
    Word = 2,
    Long = 4,
}

#[repr(C)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum EmfeWatchpointType {
    Read = 0,
    Write = 1,
    ReadWrite = 2,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeWatchpointInfo {
    pub address: u64,
    pub size: EmfeWatchpointSize,
    pub type_: EmfeWatchpointType,
    pub enabled: bool,
    pub condition: *const c_char,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub enum EmfeCallStackKind {
    Call = 0,
    Exception = 1,
    Interrupt = 2,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeCallStackEntry {
    pub call_pc: u64,
    pub target_pc: u64,
    pub return_pc: u64,
    pub frame_pointer: u64,
    pub kind: EmfeCallStackKind,
    pub label: *const c_char,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeFramebufferInfo {
    pub width: u32,
    pub height: u32,
    pub bpp: u32,
    pub stride: u32,
    pub base_address: u64,
    pub pixels: *const u8,
    pub flags: u32,
}

#[repr(C)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum EmfeSettingType {
    Int = 0,
    String = 1,
    Bool = 2,
    Combo = 3,
    File = 4,
    List = 5,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeSettingDef {
    pub key: *const c_char,
    pub label: *const c_char,
    pub group: *const c_char,
    pub type_: EmfeSettingType,
    pub default_value: *const c_char,
    pub constraints: *const c_char,
    pub depends_on: *const c_char,
    pub depends_value: *const c_char,
    pub flags: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct EmfeListItemDef {
    pub key: *const c_char,
    pub label: *const c_char,
    pub type_: EmfeSettingType,
    pub constraints: *const c_char,
}

// Callback types
pub type EmfeConsoleCharCallback = extern "C" fn(*mut c_void, c_char);
pub type EmfeStateChangeCallback = extern "C" fn(*mut c_void, *const EmfeStateInfo);
pub type EmfeDiagnosticCallback = extern "C" fn(*mut c_void, *const c_char);

// ===========================================================================
// Register IDs (match em8/z8000 naming pattern)
// ===========================================================================

#[repr(u32)]
enum RegId {
    A = 0,
    B = 1,
    D = 2,          // concatenation view of A:B
    X = 3,
    Y = 4,
    U = 5,
    S = 6,
    PC = 7,
    DP = 8,
    CC = 9,
    Cycles = 10,
    Instructions = 11,
}

// ===========================================================================
// Board info (static strings with long lifetime)
// ===========================================================================

static BOARD_NAME: &[u8] = b"MC6809\0";
static CPU_NAME: &[u8] = b"MC6809\0";
static DESCRIPTION: &[u8] =
    b"Motorola MC6809 8-bit CPU (em6809 core) with memory-mapped UART\0";
static VERSION: &[u8] = b"0.1.0\0";

// ===========================================================================
// MC6850 ACIA — Motorola Asynchronous Communications Interface Adapter
//
// Two memory-mapped registers (at base+0 and base+1):
//   RS=0 (base+0): read = Status Register (SR)
//                  write = Control Register (CR)
//   RS=1 (base+1): read = Receive Data Register (RDR)
//                  write = Transmit Data Register (TDR)
//
// Status Register bits:
//   bit0 RDRF  Receive Data Register Full
//   bit1 TDRE  Transmit Data Register Empty
//   bit2 DCD   Data Carrier Detect (tied low here)
//   bit3 CTS   Clear To Send       (tied low here)
//   bit4 FE    Framing Error       (always 0)
//   bit5 OVRN  Receive Overrun
//   bit6 PE    Parity Error        (always 0)
//   bit7 IRQ   Interrupt Request active
//
// Control Register bits:
//   bit0-1 CDS  Counter Divide Select (11 = Master Reset)
//   bit2-4 WS   Word Select (format)
//   bit5-6 TC   Transmit Control (bit5:TX IRQ en, bit6:RTS)
//   bit7   RIE  Receive Interrupt Enable
//
// IRQ is asserted (active-low in real hardware, active-high to CPU bus here)
// when (RDRF && RIE) || (TDRE && TX IRQ enabled).
// ===========================================================================

const DEFAULT_CONSOLE_BASE: u16 = 0xFF00;

struct Mc6850 {
    base: u16,

    // Control register fields
    cds: u8,           // bits 0-1
    tx_irq_enable: bool, // decoded from TC (bit5-6 == 01)
    rie: bool,         // bit 7

    // Receive side
    rx_fifo: VecDeque<u8>,
    rdrf: bool,
    rx_overrun: bool,

    // Transmit side. Instant-transmit emulation: TDRE reverts to 1 on the
    // same write because the guest would see the byte go out immediately.
    tdre: bool,

    // Held in reset state until a CR write with CDS != 11 follows a reset.
    in_master_reset: bool,

    // TX callback (filled by PluginInstance when console callback is set)
    tx_cb: Option<EmfeConsoleCharCallback>,
    tx_user: *mut c_void,
}

// SAFETY: the tx_user pointer is opaque, only handed back to the caller-
// supplied callback. Single-threaded emulation access.
unsafe impl Send for Mc6850 {}

impl Mc6850 {
    fn new(base: u16) -> Self {
        Mc6850 {
            base,
            cds: 0b11,
            tx_irq_enable: false,
            rie: false,
            rx_fifo: VecDeque::new(),
            rdrf: false,
            rx_overrun: false,
            tdre: false,
            in_master_reset: true,
            tx_cb: None,
            tx_user: ptr::null_mut(),
        }
    }

    fn hard_reset(&mut self) {
        self.cds = 0b11;
        self.tx_irq_enable = false;
        self.rie = false;
        self.rx_fifo.clear();
        self.rdrf = false;
        self.rx_overrun = false;
        self.tdre = false;
        self.in_master_reset = true;
    }

    fn is_at(&self, addr: u16) -> bool {
        addr == self.base || addr == self.base.wrapping_add(1)
    }

    fn read_sr(&self) -> u8 {
        let mut s = 0u8;
        if self.rdrf { s |= 0x01; }
        if self.tdre { s |= 0x02; }
        // DCD (bit2), CTS (bit3), FE (bit4), PE (bit6) held at 0.
        if self.rx_overrun { s |= 0x20; }
        if self.irq_active() { s |= 0x80; }
        s
    }

    fn read_rdr(&mut self) -> u8 {
        let v = self.rx_fifo.pop_front().unwrap_or(0);
        self.rdrf = !self.rx_fifo.is_empty();
        // Reading RDR clears the overrun flag too.
        self.rx_overrun = false;
        v
    }

    fn write_cr(&mut self, val: u8) {
        let cds = val & 0b11;
        let ws = (val >> 2) & 0b111;      // word-select (ignored — guest chooses 8N1, 7E1, ...)
        let tc = (val >> 5) & 0b11;       // bits 5-6
        let rie = (val & 0x80) != 0;

        self.cds = cds;
        let _ = ws;
        // Transmit Control: 01 = RTS low + TX IRQ enable.
        self.tx_irq_enable = tc == 0b01;
        self.rie = rie;

        if cds == 0b11 {
            // Master reset.
            self.in_master_reset = true;
            self.tdre = false;
            self.rdrf = false;
            self.rx_overrun = false;
            self.rx_fifo.clear();
            return;
        }
        // Coming out of reset: TX is ready.
        if self.in_master_reset {
            self.in_master_reset = false;
            self.tdre = true;
        }
    }

    fn write_tdr(&mut self, val: u8) {
        if self.in_master_reset { return; }
        // Instant transmit: fire the host TX callback and leave TDRE = 1.
        if let Some(cb) = self.tx_cb {
            cb(self.tx_user, val as c_char);
        }
        self.tdre = true;
    }

    /// Called when the host delivers an RX byte (`emfe_send_char`).
    fn receive(&mut self, ch: u8) {
        if self.in_master_reset { return; }
        // Real chip has a 1-byte RX register; extra bytes overflow.
        // We keep a small FIFO for host convenience but flag overrun if it
        // grows too large.
        const RX_FIFO_MAX: usize = 16;
        if self.rx_fifo.len() >= RX_FIFO_MAX {
            self.rx_overrun = true;
            return;
        }
        self.rx_fifo.push_back(ch);
        self.rdrf = true;
    }

    fn irq_active(&self) -> bool {
        (self.rdrf && self.rie) || (self.tdre && self.tx_irq_enable)
    }
}

// ===========================================================================
// Plugin bus: 64 KB RAM + MC6850 ACIA at a configurable base (default $FF00).
// ===========================================================================

struct PluginBus {
    memory: Box<[u8; 0x10000]>,
    acia: Mc6850,
    // Watchpoint support — fired whenever CPU-visible access hits the address.
    read_watch: Vec<u16>,   // sorted, small count assumed
    write_watch: Vec<u16>,
    watch_hit: bool,
    watch_hit_addr: u16,
    // MMIO tick counter at $FF02 (hi byte) and $FF03 (lo byte).  Tracks the
    // low 16 bits of the cumulative CPU cycle count, updated by the host
    // step function after each instruction.  Guest programs can read it to
    // seed PRNGs or measure short intervals.
    tick_word: u16,
}

// SAFETY: raw pointers inside the ACIA are only dereferenced inside callbacks
// we control, and the bus is always used from a single emulation thread.
unsafe impl Send for PluginBus {}

impl PluginBus {
    fn new() -> Self {
        PluginBus {
            memory: Box::new([0u8; 0x10000]),
            acia: Mc6850::new(DEFAULT_CONSOLE_BASE),
            read_watch: Vec::new(),
            write_watch: Vec::new(),
            watch_hit: false,
            watch_hit_addr: 0,
            tick_word: 0,
        }
    }

    fn peek(&self, addr: u16) -> u8 {
        self.memory[addr as usize]
    }

    fn poke(&mut self, addr: u16, v: u8) {
        self.memory[addr as usize] = v;
    }
}

impl Bus for PluginBus {
    fn read8(&mut self, addr: u16) -> u8 {
        if self.read_watch.binary_search(&addr).is_ok() {
            self.watch_hit = true;
            self.watch_hit_addr = addr;
        }
        if self.acia.is_at(addr) {
            return if addr == self.acia.base {
                self.acia.read_sr()
            } else {
                self.acia.read_rdr()
            };
        }
        // Tick-word MMIO at $FF02 (hi byte) and $FF03 (lo byte).
        if addr == 0xFF02 {
            return (self.tick_word >> 8) as u8;
        }
        if addr == 0xFF03 {
            return self.tick_word as u8;
        }
        self.memory[addr as usize]
    }
    fn write8(&mut self, addr: u16, data: u8) {
        if self.write_watch.binary_search(&addr).is_ok() {
            self.watch_hit = true;
            self.watch_hit_addr = addr;
        }
        if self.acia.is_at(addr) {
            if addr == self.acia.base {
                self.acia.write_cr(data);
            } else {
                self.acia.write_tdr(data);
            }
            return;
        }
        self.memory[addr as usize] = data;
    }
    fn irq_lines(&mut self) -> (bool, bool, bool) {
        // Returns (nmi, firq, irq). The ACIA drives IRQ only.
        (false, false, self.acia.irq_active())
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

// ===========================================================================
// Plugin instance
// ===========================================================================

struct Breakpoint {
    enabled: bool,
    condition: Option<CString>,
}

struct Watchpoint {
    size: EmfeWatchpointSize,
    type_: EmfeWatchpointType,
    enabled: bool,
    condition: String,
}

#[derive(Clone, Copy)]
struct ShadowFrame {
    call_pc: u16,     // PC of the JSR/BSR/LBSR instruction
    target_pc: u16,   // address transferred to
    return_pc: u16,   // PC that will be restored by RTS
}

struct PluginInstance {
    cpu: Cpu,
    bus: PluginBus,
    instr_count: AtomicU64,

    state: std::sync::atomic::AtomicU8, // EmfeState as u8
    stop_requested: AtomicBool,
    worker: Mutex<Option<JoinHandle<()>>>,

    console_cb: Option<EmfeConsoleCharCallback>,
    console_user: *mut c_void,
    state_cb: Option<EmfeStateChangeCallback>,
    state_user: *mut c_void,
    diag_cb: Option<EmfeDiagnosticCallback>,
    diag_user: *mut c_void,

    // Breakpoints / watchpoints
    breakpoints: std::collections::BTreeMap<u16, Breakpoint>,
    watchpoints: std::collections::BTreeMap<u16, Watchpoint>,

    // Settings (key -> value)
    settings_defs: Vec<EmfeSettingDef>,
    settings: std::collections::BTreeMap<String, String>,         // committed (after apply)
    staged: std::collections::BTreeMap<String, String>,           // staged (before apply)
    applied: std::collections::BTreeMap<String, String>,          // in effect on hardware
    setting_flags: std::collections::BTreeMap<String, u32>,       // per-key flags
    setting_value_buf: CString,
    applied_setting_value_buf: CString,

    // Strings owned by the plugin but handed out as `const char*`. Kept alive
    // in these vectors so returned pointers remain valid.
    reg_names: Vec<CString>,
    reg_defs: Vec<EmfeRegisterDef>,
    setting_keys: Vec<CString>,
    setting_labels: Vec<CString>,
    setting_groups: Vec<CString>,
    setting_defaults: Vec<CString>,
    setting_constraints: Vec<CString>,
    bp_condition_storage: Vec<CString>,

    disasm_storage: Vec<(CString, CString, CString)>, // (raw, mnem, operands)

    program_start: u16,
    program_end: u16,

    // Shadow call stack — populated by step_one for each BSR/JSR/LBSR and
    // drained for each RTS. Read by emfe_get_call_stack. This is independent
    // of the guest's actual S-stack contents because mc6809 has no
    // software-agnostic frame marker; intercepting call/return opcodes at the
    // plugin level gives reliable frame boundaries for typical user code.
    shadow_stack: Vec<ShadowFrame>,

    last_error: CString,
    stop_reason: AtomicI32,   // EmfeStopReason as i32
    stop_address: AtomicU64,
    instructions: AtomicI64,
    cycles_counter: AtomicI64,
}

// SAFETY: raw pointers are opaque host handles.
unsafe impl Send for PluginInstance {}
unsafe impl Sync for PluginInstance {}

impl PluginInstance {
    fn new() -> Self {
        let mut inst = PluginInstance {
            cpu: Cpu::new(),
            bus: PluginBus::new(),
            instr_count: AtomicU64::new(0),
            state: std::sync::atomic::AtomicU8::new(EmfeState::Stopped as u8),
            stop_requested: AtomicBool::new(false),
            worker: Mutex::new(None),
            console_cb: None,
            console_user: ptr::null_mut(),
            state_cb: None,
            state_user: ptr::null_mut(),
            diag_cb: None,
            diag_user: ptr::null_mut(),
            breakpoints: Default::default(),
            watchpoints: Default::default(),
            settings_defs: Vec::new(),
            settings: Default::default(),
            staged: Default::default(),
            applied: Default::default(),
            setting_flags: Default::default(),
            setting_value_buf: CString::new("").unwrap(),
            applied_setting_value_buf: CString::new("").unwrap(),
            reg_names: Vec::new(),
            reg_defs: Vec::new(),
            setting_keys: Vec::new(),
            setting_labels: Vec::new(),
            setting_groups: Vec::new(),
            setting_defaults: Vec::new(),
            setting_constraints: Vec::new(),
            bp_condition_storage: Vec::new(),
            disasm_storage: Vec::new(),
            program_start: 0,
            program_end: 0,
            shadow_stack: Vec::new(),
            last_error: CString::new("").unwrap(),
            stop_reason: AtomicI32::new(EmfeStopReason::None as i32),
            stop_address: AtomicU64::new(0),
            instructions: AtomicI64::new(0),
            cycles_counter: AtomicI64::new(0),
        };
        inst.build_reg_defs();
        inst.build_setting_defs();
        inst
    }

    fn build_reg_defs(&mut self) {
        let entries: &[(u32, &str, &str, u32, u32)] = &[
            (RegId::A as u32, "A", "CPU", 8, EMFE_REG_FLAG_NONE),
            (RegId::B as u32, "B", "CPU", 8, EMFE_REG_FLAG_NONE),
            // A:B concatenation view. Read-only — users edit A and B
            // individually, and D updates automatically because
            // emfe_get_registers recomputes (A << 8) | B every read.
            (RegId::D as u32, "D", "CPU", 16, EMFE_REG_FLAG_READONLY),
            (RegId::X as u32, "X", "CPU", 16, EMFE_REG_FLAG_NONE),
            (RegId::Y as u32, "Y", "CPU", 16, EMFE_REG_FLAG_NONE),
            (RegId::U as u32, "U", "CPU", 16, EMFE_REG_FLAG_NONE),
            (RegId::S as u32, "S", "CPU", 16, EMFE_REG_FLAG_SP),
            (RegId::PC as u32, "PC", "CPU", 16, EMFE_REG_FLAG_PC),
            (RegId::DP as u32, "DP", "CPU", 8, EMFE_REG_FLAG_NONE),
            (RegId::CC as u32, "CC", "CPU", 8, EMFE_REG_FLAG_FLAGS),
            (
                RegId::Cycles as u32,
                "Cycles",
                "Counters",
                64,
                EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN,
            ),
            (
                RegId::Instructions as u32,
                "Instructions",
                "Counters",
                64,
                EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN,
            ),
        ];
        for (id, name, group, bits, flags) in entries {
            let n = CString::new(*name).unwrap();
            let g = CString::new(*group).unwrap();
            let name_ptr = n.as_ptr();
            let group_ptr = g.as_ptr();
            self.reg_names.push(n);
            self.reg_names.push(g);
            self.reg_defs.push(EmfeRegisterDef {
                reg_id: *id,
                name: name_ptr,
                group: group_ptr,
                type_: EmfeRegType::Int,
                bit_width: *bits,
                flags: *flags,
            });
        }
    }

    fn build_setting_defs(&mut self) {
        const R: u32 = EMFE_SETTING_FLAG_REQUIRES_RESET;
        // (key, label, group, type, default, constraints, flags)
        let settings: Vec<(&str, &str, &str, EmfeSettingType, &str, Option<&str>, u32)> = vec![
            // General tab — cross-plugin shared settings only.
            (
                "BoardType",
                "Target Board",
                "General",
                EmfeSettingType::Combo,
                "Generic",
                Some("Generic"),
                R,
            ),
            (
                "Theme",
                "Theme",
                "General",
                EmfeSettingType::Combo,
                "Dark",
                Some("Dark|Light|System"),
                0,
            ),

            // MC6809 tab — module-specific settings.
            (
                "ConsoleBase",
                "Console MMIO Base",
                "MC6809",
                EmfeSettingType::String,
                "0xFF00",
                None,
                R,
            ),
            (
                "ResetVector",
                "Reset Vector Source",
                "MC6809",
                EmfeSettingType::Combo,
                "Memory",
                Some("Memory|Load Address"),
                R,
            ),
            (
                "InitialStack",
                "Initial S Stack Pointer",
                "MC6809",
                EmfeSettingType::String,
                "0xFF00",
                None,
                R,
            ),

            // Console tab — terminal display preferences.
            (
                "ConsoleScrollbackLines",
                "Scrollback Lines",
                "Console",
                EmfeSettingType::Int,
                "2000",
                Some("0|100000"),
                0,
            ),
            (
                "ConsoleColumns",
                "Columns",
                "Console",
                EmfeSettingType::Int,
                "80",
                Some("40|320"),
                0,
            ),
            (
                "ConsoleRows",
                "Rows",
                "Console",
                EmfeSettingType::Int,
                "25",
                Some("10|100"),
                0,
            ),
        ];

        for (key, label, group, ty, def, constr, flags) in settings {
            let ck = CString::new(key).unwrap();
            let cl = CString::new(label).unwrap();
            let cg = CString::new(group).unwrap();
            let cd = CString::new(def).unwrap();
            let cc = constr.map(|s| CString::new(s).unwrap());

            let key_ptr = ck.as_ptr();
            let label_ptr = cl.as_ptr();
            let group_ptr = cg.as_ptr();
            let def_ptr = cd.as_ptr();
            let constr_ptr = cc.as_ref().map_or(ptr::null(), |c| c.as_ptr());

            self.setting_keys.push(ck);
            self.setting_labels.push(cl);
            self.setting_groups.push(cg);
            self.setting_defaults.push(cd);
            if let Some(c) = cc {
                self.setting_constraints.push(c);
            }

            self.settings_defs.push(EmfeSettingDef {
                key: key_ptr,
                label: label_ptr,
                group: group_ptr,
                type_: ty,
                default_value: def_ptr,
                constraints: constr_ptr,
                depends_on: ptr::null(),
                depends_value: ptr::null(),
                flags,
            });

            self.setting_flags.insert(key.to_string(), flags);
            self.settings.insert(key.to_string(), def.to_string());
            self.staged.insert(key.to_string(), def.to_string());
            self.applied.insert(key.to_string(), def.to_string());
        }
    }

    fn notify_state(&self, state: EmfeState, reason: EmfeStopReason, addr: u64, msg: Option<&str>) {
        self.state.store(state as u8, Ordering::Release);
        if let Some(cb) = self.state_cb {
            let msg_c = msg.map(|s| CString::new(s).unwrap());
            let msg_ptr = msg_c.as_ref().map_or(ptr::null(), |c| c.as_ptr());
            let info = EmfeStateInfo {
                state,
                stop_reason: reason,
                stop_address: addr,
                stop_message: msg_ptr,
            };
            cb(self.state_user, &info);
        }
    }

    fn get_reg_u64(&self, id: u32) -> u64 {
        let r = &self.cpu.r;
        match id {
            x if x == RegId::A as u32 => r.a as u64,
            x if x == RegId::B as u32 => r.b as u64,
            x if x == RegId::D as u32 => ((r.a as u64) << 8) | (r.b as u64),
            x if x == RegId::X as u32 => r.x as u64,
            x if x == RegId::Y as u32 => r.y as u64,
            x if x == RegId::U as u32 => r.u as u64,
            x if x == RegId::S as u32 => r.s as u64,
            x if x == RegId::PC as u32 => r.pc as u64,
            x if x == RegId::DP as u32 => r.dp as u64,
            x if x == RegId::CC as u32 => r.cc as u64,
            x if x == RegId::Cycles as u32 => self.cpu.cycles,
            x if x == RegId::Instructions as u32 => self.instr_count.load(Ordering::Relaxed),
            _ => 0,
        }
    }

    fn set_reg_u64(&mut self, id: u32, val: u64) {
        let r = &mut self.cpu.r;
        match id {
            x if x == RegId::A as u32 => r.a = val as u8,
            x if x == RegId::B as u32 => r.b = val as u8,
            x if x == RegId::D as u32 => {
                r.a = (val >> 8) as u8;
                r.b = (val & 0xFF) as u8;
            }
            x if x == RegId::X as u32 => r.x = val as u16,
            x if x == RegId::Y as u32 => r.y = val as u16,
            x if x == RegId::U as u32 => r.u = val as u16,
            x if x == RegId::S as u32 => r.s = val as u16,
            x if x == RegId::PC as u32 => r.pc = val as u16,
            x if x == RegId::DP as u32 => r.dp = val as u8,
            x if x == RegId::CC as u32 => r.cc = val as u8,
            _ => {}
        }
    }
}

// ===========================================================================
// Instance handle management
// ===========================================================================

// We hand out `EmfeInstance` as a raw opaque pointer (`*mut PluginInstance`
// cast to an abstract type). The C header types it as `EmfeInstance`.
pub type EmfeInstance = *mut c_void;

unsafe fn inst_ref<'a>(handle: EmfeInstance) -> Option<&'a PluginInstance> {
    if handle.is_null() {
        None
    } else {
        Some(&*(handle as *const PluginInstance))
    }
}
unsafe fn inst_mut<'a>(handle: EmfeInstance) -> Option<&'a mut PluginInstance> {
    if handle.is_null() {
        None
    } else {
        Some(&mut *(handle as *mut PluginInstance))
    }
}

// ===========================================================================
// Discovery & lifecycle
// ===========================================================================

#[no_mangle]
pub extern "C" fn emfe_negotiate(info: *const EmfeNegotiateInfo) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
        if info.is_null() {
            return EmfeResult::ErrInvalid;
        }
        let info = unsafe { &*info };
        if info.api_version_major != EMFE_API_VERSION_MAJOR {
            return EmfeResult::ErrUnsupported;
        }
        EmfeResult::Ok
    })
}

#[no_mangle]
pub extern "C" fn emfe_get_board_info(out: *mut EmfeBoardInfo) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    if out.is_null() {
        return EmfeResult::ErrInvalid;
    }
    unsafe {
        (*out).board_name = BOARD_NAME.as_ptr() as *const c_char;
        (*out).cpu_name = CPU_NAME.as_ptr() as *const c_char;
        (*out).description = DESCRIPTION.as_ptr() as *const c_char;
        (*out).version = VERSION.as_ptr() as *const c_char;
        (*out).capabilities =
            EMFE_CAP_LOAD_SREC | EMFE_CAP_LOAD_BINARY | EMFE_CAP_WATCHPOINTS
            | EMFE_CAP_STEP_OVER | EMFE_CAP_STEP_OUT | EMFE_CAP_CALL_STACK;
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub extern "C" fn emfe_create(out_instance: *mut EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    if out_instance.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let boxed = Box::new(PluginInstance::new());
    unsafe {
        *out_instance = Box::into_raw(boxed) as EmfeInstance;
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub extern "C" fn emfe_destroy(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    if instance.is_null() {
        return EmfeResult::ErrInvalid;
    }
    unsafe {
        // Stop the worker thread if it's running.
        let inst_ptr = instance as *mut PluginInstance;
        (*inst_ptr).stop_requested.store(true, Ordering::Release);
        if let Ok(mut g) = (*inst_ptr).worker.lock() {
            if let Some(h) = g.take() {
                drop(g);
                let _ = h.join();
            }
        }
        let _ = Box::from_raw(inst_ptr);
    }
    EmfeResult::Ok
    })
}

// ===========================================================================
// Callbacks
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_set_console_char_callback(
    instance: EmfeInstance,
    cb: Option<EmfeConsoleCharCallback>,
    user: *mut c_void,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.console_cb = cb;
    inst.console_user = user;
    inst.bus.acia.tx_cb = cb;
    inst.bus.acia.tx_user = user;
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_set_state_change_callback(
    instance: EmfeInstance,
    cb: Option<EmfeStateChangeCallback>,
    user: *mut c_void,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.state_cb = cb;
    inst.state_user = user;
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_set_diagnostic_callback(
    instance: EmfeInstance,
    cb: Option<EmfeDiagnosticCallback>,
    user: *mut c_void,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.diag_cb = cb;
    inst.diag_user = user;
    EmfeResult::Ok
    })
}

// ===========================================================================
// Registers
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_get_register_defs(
    instance: EmfeInstance,
    out_defs: *mut *const EmfeRegisterDef,
) -> i32 {
    ffi_catch!(0, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return 0,
    };
    if out_defs.is_null() {
        return 0;
    }
    *out_defs = inst.reg_defs.as_ptr();
    inst.reg_defs.len() as i32
    })
}

// MC6809 CC (Condition Code) register bit decomposition. Order is
// MSB-first (E F H I N Z V C) to match how Motorola data sheets and
// every assembly listing have rendered the register since 1978.
//   bit 0 : C — Carry
//   bit 1 : V — Overflow
//   bit 2 : Z — Zero
//   bit 3 : N — Negative
//   bit 4 : I — IRQ mask
//   bit 5 : H — Half-carry
//   bit 6 : F — FIRQ mask
//   bit 7 : E — Entire flag (full register stack on interrupt)
static CC_FLAG_BITS: [EmfeRegFlagBitDef; 8] = [
    EmfeRegFlagBitDef { bit_index: 7, label: b"E\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 6, label: b"F\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 5, label: b"H\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 4, label: b"I\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 3, label: b"N\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 2, label: b"Z\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 1, label: b"V\0".as_ptr() as *const c_char },
    EmfeRegFlagBitDef { bit_index: 0, label: b"C\0".as_ptr() as *const c_char },
];

#[no_mangle]
pub unsafe extern "C" fn emfe_get_register_flag_defs(
    _instance: EmfeInstance,
    reg_id: u32,
    out_defs: *mut *const EmfeRegFlagBitDef,
) -> i32 {
    ffi_catch!(0, {
    if out_defs.is_null() {
        return 0;
    }
    *out_defs = std::ptr::null();
    if reg_id == RegId::CC as u32 {
        *out_defs = CC_FLAG_BITS.as_ptr();
        return CC_FLAG_BITS.len() as i32;
    }
    0
    })
}

// MC6809 D register is the concatenation of A (high byte) and B (low byte).
// emfe_get_registers already computes (A << 8) | B for us, but during Edit
// mode the frontend recomputes the displayed D from the live A and B
// textbox values via these deps so the user sees the new D the moment
// they finish typing into A or B — before pressing Apply.
static D_VIEW_DEPS: [EmfeRegViewDep; 2] = [
    EmfeRegViewDep { reg_id: RegId::A as u32, shift: 8, width: 8 },
    EmfeRegViewDep { reg_id: RegId::B as u32, shift: 0, width: 8 },
];

#[no_mangle]
pub unsafe extern "C" fn emfe_get_register_view_deps(
    _instance: EmfeInstance,
    reg_id: u32,
    out_deps: *mut *const EmfeRegViewDep,
) -> i32 {
    ffi_catch!(0, {
    if out_deps.is_null() {
        return 0;
    }
    *out_deps = std::ptr::null();
    if reg_id == RegId::D as u32 {
        *out_deps = D_VIEW_DEPS.as_ptr();
        return D_VIEW_DEPS.len() as i32;
    }
    0
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_registers(
    instance: EmfeInstance,
    values: *mut EmfeRegValue,
    count: i32,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if values.is_null() || count <= 0 {
        return EmfeResult::ErrInvalid;
    }
    let slice = std::slice::from_raw_parts_mut(values, count as usize);
    for v in slice.iter_mut() {
        v.value = EmfeRegValueUnion {
            u64_: inst.get_reg_u64(v.reg_id),
        };
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_set_registers(
    instance: EmfeInstance,
    values: *const EmfeRegValue,
    count: i32,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if values.is_null() || count <= 0 {
        return EmfeResult::ErrInvalid;
    }
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    let slice = std::slice::from_raw_parts(values, count as usize);
    for v in slice {
        inst.set_reg_u64(v.reg_id, v.value.u64_);
    }
    EmfeResult::Ok
    })
}

// ===========================================================================
// Memory peek / poke (64 KB linear; MMIO reads in peek bypass side effects).
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_peek_byte(instance: EmfeInstance, addr: u64) -> u8 {
    ffi_catch!(0, {
    match inst_ref(instance) {
        Some(i) => i.bus.peek(addr as u16),
        None => 0,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_peek_word(instance: EmfeInstance, addr: u64) -> u16 {
    ffi_catch!(0, {
    match inst_ref(instance) {
        Some(i) => {
            let a = addr as u16;
            ((i.bus.peek(a) as u16) << 8) | (i.bus.peek(a.wrapping_add(1)) as u16)
        }
        None => 0,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_peek_long(instance: EmfeInstance, addr: u64) -> u32 {
    ffi_catch!(0, {
    match inst_ref(instance) {
        Some(i) => {
            let a = addr as u16;
            (0..4)
                .map(|o| i.bus.peek(a.wrapping_add(o)) as u32)
                .enumerate()
                .fold(0u32, |acc, (idx, b)| acc | (b << ((3 - idx) * 8)))
        }
        None => 0,
    }
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_poke_byte(instance: EmfeInstance, addr: u64, v: u8) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    match inst_mut(instance) {
        Some(i) => {
            i.bus.poke(addr as u16, v);
            EmfeResult::Ok
        }
        None => EmfeResult::ErrInvalid,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_poke_word(instance: EmfeInstance, addr: u64, v: u16) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    match inst_mut(instance) {
        Some(i) => {
            let a = addr as u16;
            i.bus.poke(a, (v >> 8) as u8);
            i.bus.poke(a.wrapping_add(1), v as u8);
            EmfeResult::Ok
        }
        None => EmfeResult::ErrInvalid,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_poke_long(instance: EmfeInstance, addr: u64, v: u32) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    match inst_mut(instance) {
        Some(i) => {
            let a = addr as u16;
            for k in 0..4 {
                let b = (v >> ((3 - k) * 8)) as u8;
                i.bus.poke(a.wrapping_add(k as u16), b);
            }
            EmfeResult::Ok
        }
        None => EmfeResult::ErrInvalid,
    }
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_peek_range(
    instance: EmfeInstance,
    addr: u64,
    out: *mut u8,
    length: u32,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if out.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let dst = std::slice::from_raw_parts_mut(out, length as usize);
    let a = addr as u16;
    for (i, slot) in dst.iter_mut().enumerate() {
        *slot = inst.bus.peek(a.wrapping_add(i as u16));
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub extern "C" fn emfe_get_memory_size(_instance: EmfeInstance) -> u64 {
    ffi_catch!(0, {
    65536
    })
}

// ===========================================================================
// Disassembly
// ===========================================================================

fn disassemble_one_at(inst: &PluginInstance, addr: u16) -> (String, String, String, u32) {
    // Reuse em6809::disasm::disasm_one via a read-only shadow bus so the
    // disassembly doesn't perturb CPU/bus state.
    struct PeekBus<'a> {
        mem: &'a [u8; 0x10000],
    }
    impl<'a> Bus for PeekBus<'a> {
        fn read8(&mut self, addr: u16) -> u8 {
            self.mem[addr as usize]
        }
        fn write8(&mut self, _addr: u16, _data: u8) {}
        fn as_any_mut(&mut self) -> &mut dyn Any {
            unimplemented!()
        }
    }
    let mut peek = PeekBus {
        mem: &inst.bus.memory,
    };
    let (len, text) = em6809::disasm::disasm_one(&mut peek, addr);

    // Raw bytes string: "AB CD EF" from the memory slice.
    let raw = (0..len)
        .map(|i| format!("{:02X}", inst.bus.memory[addr.wrapping_add(i) as usize]))
        .collect::<Vec<_>>()
        .join(" ");

    // Split the disassembly text into mnemonic + operands at the first
    // whitespace run. em6809 outputs e.g. "LDA #$42" or "NOP".
    let (mnem, operands) = match text.find(|c: char| c.is_whitespace()) {
        Some(pos) => (
            text[..pos].to_string(),
            text[pos..].trim_start().to_string(),
        ),
        None => (text, String::new()),
    };

    (raw, mnem, operands, len as u32)
}

#[no_mangle]
pub unsafe extern "C" fn emfe_disassemble_one(
    instance: EmfeInstance,
    addr: u64,
    out: *mut EmfeDisasmLine,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if out.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let (raw, mnem, operands, len) = disassemble_one_at(inst, addr as u16);
    let cr = CString::new(raw).unwrap();
    let cm = CString::new(mnem).unwrap();
    let co = CString::new(operands).unwrap();
    inst.disasm_storage.clear();
    inst.disasm_storage.push((cr, cm, co));
    let (r, m, o) = inst.disasm_storage.last().unwrap();
    (*out).address = addr;
    (*out).raw_bytes = r.as_ptr();
    (*out).mnemonic = m.as_ptr();
    (*out).operands = o.as_ptr();
    (*out).length = len;
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_disassemble_range(
    instance: EmfeInstance,
    start: u64,
    end: u64,
    out: *mut EmfeDisasmLine,
    max: i32,
) -> i32 {
    ffi_catch!(0, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return 0,
    };
    if out.is_null() || max <= 0 {
        return 0;
    }
    inst.disasm_storage.clear();
    inst.disasm_storage.reserve(max as usize);
    let mut addr = start as u16;
    let end_u16 = end as u16;
    let mut count = 0i32;
    let slice = std::slice::from_raw_parts_mut(out, max as usize);
    while addr < end_u16 && count < max {
        let (raw, mnem, operands, len) = disassemble_one_at(inst, addr);
        let cr = CString::new(raw).unwrap();
        let cm = CString::new(mnem).unwrap();
        let co = CString::new(operands).unwrap();
        inst.disasm_storage.push((cr, cm, co));
        let (r, m, o) = inst.disasm_storage.last().unwrap();
        slice[count as usize].address = addr as u64;
        slice[count as usize].raw_bytes = r.as_ptr();
        slice[count as usize].mnemonic = m.as_ptr();
        slice[count as usize].operands = o.as_ptr();
        slice[count as usize].length = len;
        addr = addr.wrapping_add(len as u16);
        count += 1;
    }
    count
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_program_range(
    instance: EmfeInstance,
    out_start: *mut u64,
    out_end: *mut u64,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if !out_start.is_null() {
        *out_start = inst.program_start as u64;
    }
    if !out_end.is_null() {
        *out_end = inst.program_end as u64;
    }
    EmfeResult::Ok
    })
}

// ===========================================================================
// Execution
// ===========================================================================

fn step_one(inst: &mut PluginInstance) -> u32 {
    // Capture pre-step state so we can classify the instruction just executed.
    let pre_pc = inst.cpu.r.pc;
    let op = inst.bus.read8(pre_pc);
    // Distinguish RTS ($39) from calls before stepping because after the step
    // PC/S have already changed.
    let is_rts = op == 0x39;
    let is_call = matches!(op, 0x8D | 0x17 | 0x9D | 0xAD | 0xBD);
    let call_len = if is_call {
        let (len, _) = em6809::disasm::disasm_one(&mut inst.bus, pre_pc);
        len
    } else {
        0
    };

    let cycles = inst.cpu.step(&mut inst.bus, false);
    inst.instr_count.fetch_add(1, Ordering::Relaxed);
    inst.instructions.fetch_add(1, Ordering::Relaxed);
    inst.cycles_counter.fetch_add(cycles as i64, Ordering::Relaxed);
    inst.bus.tick_word = inst.bus.tick_word.wrapping_add(cycles as u16);

    // Shadow call-stack maintenance. Only BSR/LBSR/JSR variants are tracked;
    // SWI/interrupt frames have different push layouts and typically return
    // via RTI, which we do not intercept here.
    if is_call {
        let return_pc = pre_pc.wrapping_add(call_len);
        inst.shadow_stack.push(ShadowFrame {
            call_pc: pre_pc,
            target_pc: inst.cpu.r.pc,
            return_pc,
        });
    } else if is_rts {
        inst.shadow_stack.pop();
    }

    cycles
}

#[no_mangle]
pub unsafe extern "C" fn emfe_step(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    inst.state.store(EmfeState::Stepping as u8, Ordering::Release);
    let _ = step_one(inst);
    let st = EmfeState::Stopped;
    inst.state.store(st as u8, Ordering::Release);
    inst.notify_state(st, EmfeStopReason::Step, inst.cpu.r.pc as u64, None);
    EmfeResult::Ok
    })
}

// MC6809 "call" opcodes:
//   $8D        BSR rel8          (2 bytes)
//   $17        LBSR rel16        (3 bytes)
//   $9D        JSR direct        (2 bytes)
//   $AD pp     JSR indexed       (variable length via postbyte)
//   $BD        JSR extended      (3 bytes)
//   $3F        SWI               (1 byte)
//   $10 $3F    SWI2              (2 bytes)
//   $11 $3F    SWI3              (2 bytes)
// For step_over we treat all of these as calls; instruction length is looked
// up via em6809's disassembler so indexed-JSR is handled correctly.
fn is_call_instruction(bus: &mut PluginBus, pc: u16) -> bool {
    let op = bus.read8(pc);
    match op {
        0x8D | 0x17 | 0x9D | 0xAD | 0xBD | 0x3F => true,
        0x10 | 0x11 => bus.read8(pc.wrapping_add(1)) == 0x3F,
        _ => false,
    }
}

// Bound the step loop so a runaway never hangs the worker thread (e.g. a
// call target that never returns within the expected window).
const STEP_LIMIT: u32 = 2_000_000;

#[no_mangle]
pub unsafe extern "C" fn emfe_step_over(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    inst.state.store(EmfeState::Stepping as u8, Ordering::Release);

    let pc0 = inst.cpu.r.pc;
    let is_call = is_call_instruction(&mut inst.bus, pc0);
    if !is_call {
        // Not a call — behave like plain step.
        let _ = step_one(inst);
    } else {
        // Determine the PC at which control returns after the call.
        let (len, _) = em6809::disasm::disasm_one(&mut inst.bus, pc0);
        let return_target = pc0.wrapping_add(len);

        // Run until we land on the return target or we blow through the limit.
        // The first step always executes (the call itself).
        let mut count = 0u32;
        while count < STEP_LIMIT {
            let _ = step_one(inst);
            count += 1;
            if inst.cpu.r.pc == return_target {
                break;
            }
            // Breakpoint inside the callee halts stepping too.
            if let Some(bp) = inst.breakpoints.get(&inst.cpu.r.pc) {
                if bp.enabled {
                    break;
                }
            }
        }
    }

    inst.state.store(EmfeState::Stopped as u8, Ordering::Release);
    inst.notify_state(EmfeState::Stopped, EmfeStopReason::Step, inst.cpu.r.pc as u64, None);
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_step_out(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    inst.state.store(EmfeState::Stepping as u8, Ordering::Release);

    // Peek the return address at the top of the S stack (hi, lo). This is
    // where the next RTS would transfer control. We stop once PC reaches it.
    let s = inst.cpu.r.s;
    let hi = inst.bus.read8(s) as u16;
    let lo = inst.bus.read8(s.wrapping_add(1)) as u16;
    let return_target = (hi << 8) | lo;

    let mut count = 0u32;
    while count < STEP_LIMIT {
        let _ = step_one(inst);
        count += 1;
        if inst.cpu.r.pc == return_target {
            break;
        }
        if let Some(bp) = inst.breakpoints.get(&inst.cpu.r.pc) {
            if bp.enabled {
                break;
            }
        }
    }

    inst.state.store(EmfeState::Stopped as u8, Ordering::Release);
    inst.notify_state(EmfeState::Stopped, EmfeStopReason::Step, inst.cpu.r.pc as u64, None);
    EmfeResult::Ok
    })
}

// NOTE: emfe_run / emfe_stop implement a simple busy-loop worker thread.
#[no_mangle]
pub unsafe extern "C" fn emfe_run(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    if instance.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let inst_ptr = instance as *mut PluginInstance;
    if (*inst_ptr).state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    // Wait for any previous worker
    {
        let mut g = (*inst_ptr).worker.lock().unwrap();
        if let Some(h) = g.take() {
            drop(g);
            let _ = h.join();
        }
    }
    (*inst_ptr).stop_requested.store(false, Ordering::Release);
    (*inst_ptr).state.store(EmfeState::Running as u8, Ordering::Release);

    let raw = instance as usize;
    let handle = std::thread::spawn(move || {
        let inst_ptr = raw as *mut PluginInstance;
        let inst = &mut *inst_ptr;
        loop {
            if inst.stop_requested.load(Ordering::Acquire) {
                inst.state
                    .store(EmfeState::Stopped as u8, Ordering::Release);
                inst.notify_state(
                    EmfeState::Stopped,
                    EmfeStopReason::User,
                    inst.cpu.r.pc as u64,
                    Some("Stopped by user"),
                );
                break;
            }
            // Breakpoint check (before execute)
            if let Some(bp) = inst.breakpoints.get(&inst.cpu.r.pc) {
                if bp.enabled {
                    inst.state
                        .store(EmfeState::Stopped as u8, Ordering::Release);
                    inst.notify_state(
                        EmfeState::Stopped,
                        EmfeStopReason::Breakpoint,
                        inst.cpu.r.pc as u64,
                        Some("Breakpoint hit"),
                    );
                    break;
                }
            }
            let _ = step_one(inst);
            // Watchpoint check (after execute)
            if inst.bus.watch_hit {
                let a = inst.bus.watch_hit_addr;
                inst.bus.watch_hit = false;
                inst.state
                    .store(EmfeState::Stopped as u8, Ordering::Release);
                inst.notify_state(
                    EmfeState::Stopped,
                    EmfeStopReason::Watchpoint,
                    a as u64,
                    Some("Watchpoint hit"),
                );
                break;
            }
        }
    });
    *(*inst_ptr).worker.lock().unwrap() = Some(handle);
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_stop(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.stop_requested.store(true, Ordering::Release);
    let mut g = inst.worker.lock().unwrap();
    if let Some(h) = g.take() {
        drop(g);
        let _ = h.join();
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_reset(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }

    // Flush deferred REQUIRES_RESET settings now that it's safe to rebuild devices.
    inst.applied = inst.settings.clone();

    // Apply MC6809 tab settings that take effect only at reset.
    let parse_u16 = |s: &str| -> Option<u16> {
        let s = s.trim();
        if let Some(hex) = s.strip_prefix("0x").or(s.strip_prefix("0X")) {
            u16::from_str_radix(hex, 16).ok()
        } else {
            s.parse::<u16>().ok()
        }
    };
    if let Some(v) = inst.settings.get("ConsoleBase").cloned() {
        if let Some(base) = parse_u16(&v) {
            inst.bus.acia.base = base;
        }
    }

    inst.cpu = Cpu::new();
    inst.cpu.reset(&mut inst.bus);
    // Preserve the ACIA's host-facing callbacks across a guest reset.
    let saved_cb = inst.bus.acia.tx_cb;
    let saved_user = inst.bus.acia.tx_user;
    let saved_base = inst.bus.acia.base;
    inst.bus.acia.hard_reset();
    inst.bus.acia.tx_cb = saved_cb;
    inst.bus.acia.tx_user = saved_user;
    inst.bus.acia.base = saved_base;
    inst.instr_count.store(0, Ordering::Relaxed);
    inst.instructions.store(0, Ordering::Relaxed);
    inst.cycles_counter.store(0, Ordering::Relaxed);
    inst.shadow_stack.clear();
    inst.state
        .store(EmfeState::Stopped as u8, Ordering::Release);
    inst.notify_state(
        EmfeState::Stopped,
        EmfeStopReason::None,
        inst.cpu.r.pc as u64,
        Some("Reset"),
    );
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_state(instance: EmfeInstance) -> EmfeState {
    ffi_catch!(EmfeState::Stopped, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return EmfeState::Stopped,
    };
    match inst.state.load(Ordering::Acquire) {
        1 => EmfeState::Running,
        2 => EmfeState::Halted,
        3 => EmfeState::Stepping,
        _ => EmfeState::Stopped,
    }
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_instruction_count(instance: EmfeInstance) -> i64 {
    ffi_catch!(0, {
    inst_ref(instance)
        .map(|i| i.instructions.load(Ordering::Relaxed))
        .unwrap_or(0)
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_get_cycle_count(instance: EmfeInstance) -> i64 {
    ffi_catch!(0, {
    inst_ref(instance)
        .map(|i| i.cycles_counter.load(Ordering::Relaxed))
        .unwrap_or(0)
    })
}

// ===========================================================================
// Breakpoints
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_add_breakpoint(instance: EmfeInstance, addr: u64) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.breakpoints.insert(
        addr as u16,
        Breakpoint {
            enabled: true,
            condition: None,
        },
    );
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_remove_breakpoint(instance: EmfeInstance, addr: u64) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.breakpoints.remove(&(addr as u16));
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_enable_breakpoint(
    instance: EmfeInstance,
    addr: u64,
    enabled: bool,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    match inst.breakpoints.get_mut(&(addr as u16)) {
        Some(bp) => {
            bp.enabled = enabled;
            EmfeResult::Ok
        }
        None => EmfeResult::ErrNotFound,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_set_breakpoint_condition(
    instance: EmfeInstance,
    addr: u64,
    cond: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    match inst.breakpoints.get_mut(&(addr as u16)) {
        Some(bp) => {
            bp.condition = if cond.is_null() {
                None
            } else {
                Some(CStr::from_ptr(cond).to_owned())
            };
            EmfeResult::Ok
        }
        None => EmfeResult::ErrNotFound,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_clear_breakpoints(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.breakpoints.clear();
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_get_breakpoints(
    instance: EmfeInstance,
    out: *mut EmfeBreakpointInfo,
    max: i32,
) -> i32 {
    ffi_catch!(0, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return 0,
    };
    if out.is_null() || max <= 0 {
        return 0;
    }
    let slice = std::slice::from_raw_parts_mut(out, max as usize);
    let mut count = 0usize;
    for (&addr, bp) in inst.breakpoints.iter() {
        if count >= max as usize {
            break;
        }
        slice[count] = EmfeBreakpointInfo {
            address: addr as u64,
            enabled: bp.enabled,
            condition: bp
                .condition
                .as_ref()
                .map_or(ptr::null(), |c| c.as_ptr()),
        };
        count += 1;
    }
    count as i32
    })
}

// ===========================================================================
// Watchpoints, call stack, framebuffer, input: Phase 1 stubs
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_add_watchpoint(
    instance: EmfeInstance,
    addr: u64,
    size: EmfeWatchpointSize,
    ty: EmfeWatchpointType,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    let a = addr as u16;
    inst.watchpoints.insert(
        a,
        Watchpoint {
            size,
            type_: ty,
            enabled: true,
            condition: String::new(),
        },
    );
    match ty {
        EmfeWatchpointType::Read | EmfeWatchpointType::ReadWrite => {
            if inst.bus.read_watch.binary_search(&a).is_err() {
                inst.bus.read_watch.push(a);
                inst.bus.read_watch.sort_unstable();
            }
        }
        _ => {}
    }
    match ty {
        EmfeWatchpointType::Write | EmfeWatchpointType::ReadWrite => {
            if inst.bus.write_watch.binary_search(&a).is_err() {
                inst.bus.write_watch.push(a);
                inst.bus.write_watch.sort_unstable();
            }
        }
        _ => {}
    }
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_remove_watchpoint(instance: EmfeInstance, addr: u64) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    let a = addr as u16;
    inst.watchpoints.remove(&a);
    inst.bus.read_watch.retain(|&x| x != a);
    inst.bus.write_watch.retain(|&x| x != a);
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_enable_watchpoint(
    instance: EmfeInstance,
    addr: u64,
    enabled: bool,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    match inst.watchpoints.get_mut(&(addr as u16)) {
        Some(wp) => {
            wp.enabled = enabled;
            EmfeResult::Ok
        }
        None => EmfeResult::ErrNotFound,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_set_watchpoint_condition(
    instance: EmfeInstance,
    addr: u64,
    cond: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    match inst.watchpoints.get_mut(&(addr as u16)) {
        Some(wp) => {
            wp.condition = if cond.is_null() {
                String::new()
            } else {
                CStr::from_ptr(cond).to_string_lossy().into_owned()
            };
            EmfeResult::Ok
        }
        None => EmfeResult::ErrNotFound,
    }
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_clear_watchpoints(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.watchpoints.clear();
    inst.bus.read_watch.clear();
    inst.bus.write_watch.clear();
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_get_watchpoints(
    instance: EmfeInstance,
    out: *mut EmfeWatchpointInfo,
    max: i32,
) -> i32 {
    ffi_catch!(0, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return 0,
    };
    if out.is_null() || max <= 0 {
        return 0;
    }
    let slice = std::slice::from_raw_parts_mut(out, max as usize);
    let mut count = 0usize;
    for (&addr, wp) in inst.watchpoints.iter() {
        if count >= max as usize {
            break;
        }
        slice[count] = EmfeWatchpointInfo {
            address: addr as u64,
            size: wp.size,
            type_: wp.type_,
            enabled: wp.enabled,
            condition: ptr::null(),
        };
        count += 1;
    }
    count as i32
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_get_call_stack(
    instance: EmfeInstance,
    out: *mut EmfeCallStackEntry,
    max: i32,
) -> i32 {
    ffi_catch!(0, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return 0,
    };
    if out.is_null() || max <= 0 {
        return 0;
    }
    // Emit innermost (most recently entered) frame first so the host can
    // display the top of stack at index 0.
    let n = inst.shadow_stack.len().min(max as usize);
    let slice = std::slice::from_raw_parts_mut(out, n);
    for (i, frame) in inst.shadow_stack.iter().rev().take(n).enumerate() {
        slice[i] = EmfeCallStackEntry {
            call_pc: frame.call_pc as u64,
            target_pc: frame.target_pc as u64,
            return_pc: frame.return_pc as u64,
            frame_pointer: 0,
            kind: EmfeCallStackKind::Call,
            label: ptr::null(),
        };
    }
    n as i32
    })
}

#[no_mangle]
pub extern "C" fn emfe_get_framebuffer_info(
    _instance: EmfeInstance,
    _out: *mut EmfeFramebufferInfo,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}
#[no_mangle]
pub extern "C" fn emfe_get_palette_entry(_instance: EmfeInstance, _index: u32) -> u32 {
    ffi_catch!(0, {
    0
    })
}
#[no_mangle]
pub extern "C" fn emfe_get_palette(
    _instance: EmfeInstance,
    _out: *mut u32,
    _max: i32,
) -> i32 {
    ffi_catch!(0, {
    0
    })
}
#[no_mangle]
pub extern "C" fn emfe_push_key(
    _instance: EmfeInstance,
    _scancode: u32,
    _pressed: bool,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}
#[no_mangle]
pub extern "C" fn emfe_push_mouse_move(
    _instance: EmfeInstance,
    _dx: i32,
    _dy: i32,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}
#[no_mangle]
pub extern "C" fn emfe_push_mouse_absolute(
    _instance: EmfeInstance,
    _x: i32,
    _y: i32,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}
#[no_mangle]
pub extern "C" fn emfe_push_mouse_button(
    _instance: EmfeInstance,
    _button: i32,
    _pressed: bool,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}

// ===========================================================================
// File loading
// ===========================================================================

#[no_mangle]
pub extern "C" fn emfe_load_elf(
    _instance: EmfeInstance,
    _path: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_load_binary(
    instance: EmfeInstance,
    path: *const c_char,
    load_address: u64,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if path.is_null() {
        return EmfeResult::ErrInvalid;
    }
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    let cstr = CStr::from_ptr(path);
    let path_str = cstr.to_string_lossy().into_owned();
    let data = match std::fs::read(&path_str) {
        Ok(d) => d,
        Err(e) => {
            inst.last_error = CString::new(format!("Cannot open file: {}", e)).unwrap();
            return EmfeResult::ErrIo;
        }
    };
    let addr0 = load_address as u16;
    for (i, &b) in data.iter().enumerate() {
        if (addr0 as usize + i) >= 0x10000 {
            break;
        }
        inst.bus.poke(addr0.wrapping_add(i as u16), b);
    }
    inst.program_start = addr0;
    inst.program_end = addr0.wrapping_add(data.len() as u16);

    // If a reset vector is present, load PC from it; otherwise PC = load addr.
    let vh = inst.bus.peek(0xFFFE);
    let vl = inst.bus.peek(0xFFFF);
    let vec = ((vh as u16) << 8) | (vl as u16);
    inst.cpu.r.pc = if vec != 0 { vec } else { addr0 };
    inst.cpu.r.s = 0xFF00;
    inst.last_error = CString::new("").unwrap();
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_load_srec(
    instance: EmfeInstance,
    path: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if path.is_null() {
        return EmfeResult::ErrInvalid;
    }
    if inst.state.load(Ordering::Acquire) == EmfeState::Running as u8 {
        return EmfeResult::ErrState;
    }
    let path_str = CStr::from_ptr(path).to_string_lossy().into_owned();
    let content = match std::fs::read_to_string(&path_str) {
        Ok(s) => s,
        Err(e) => {
            inst.last_error = CString::new(format!("Cannot open file: {}", e)).unwrap();
            return EmfeResult::ErrIo;
        }
    };

    let hex2 = |s: &str| -> Option<u8> {
        u8::from_str_radix(s, 16).ok()
    };
    let mut min_addr: u32 = 0x10000;
    let mut max_addr: u32 = 0;
    let mut entry: Option<u16> = None;
    for line in content.lines() {
        if line.len() < 4 || !line.starts_with('S') {
            continue;
        }
        let rec = &line[1..2];
        let bytes = &line[2..];
        if bytes.len() < 2 {
            continue;
        }
        let _bc = match hex2(&bytes[0..2]) {
            Some(b) => b as usize,
            None => continue,
        };
        match rec {
            "1" if bytes.len() >= 8 => {
                let addr = ((hex2(&bytes[2..4]).unwrap_or(0) as u16) << 8)
                    | (hex2(&bytes[4..6]).unwrap_or(0) as u16);
                let data_hex = &bytes[6..bytes.len() - 2];
                let mut a = addr;
                let mut p = data_hex;
                while p.len() >= 2 {
                    if let Some(b) = hex2(&p[0..2]) {
                        inst.bus.poke(a, b);
                        let au = a as u32;
                        if au < min_addr {
                            min_addr = au;
                        }
                        if au + 1 > max_addr {
                            max_addr = au + 1;
                        }
                        a = a.wrapping_add(1);
                    }
                    p = &p[2..];
                }
            }
            "9" if bytes.len() >= 6 => {
                entry = Some(
                    ((hex2(&bytes[2..4]).unwrap_or(0) as u16) << 8)
                        | (hex2(&bytes[4..6]).unwrap_or(0) as u16),
                );
            }
            _ => {}
        }
    }

    if let Some(e) = entry {
        inst.cpu.r.pc = e;
    } else if min_addr < 0x10000 {
        inst.cpu.r.pc = min_addr as u16;
    }
    inst.cpu.r.s = 0xFF00;
    inst.program_start = if min_addr < 0x10000 { min_addr as u16 } else { 0 };
    inst.program_end = max_addr as u16;
    inst.last_error = CString::new("").unwrap();
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_last_error(instance: EmfeInstance) -> *const c_char {
    ffi_catch!(std::ptr::null::<c_char>() as *const c_char, {
    match inst_ref(instance) {
        Some(i) => i.last_error.as_ptr(),
        None => b"Invalid instance\0".as_ptr() as *const c_char,
    }
    })
}

// ===========================================================================
// Console I/O
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_send_char(instance: EmfeInstance, ch: c_char) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.bus.acia.receive(ch as u8);
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_send_string(
    instance: EmfeInstance,
    s: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    if instance.is_null() || s.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let bytes = CStr::from_ptr(s).to_bytes();
    let inst = &mut *(instance as *mut PluginInstance);
    for &b in bytes {
        inst.bus.acia.receive(b);
    }
    EmfeResult::Ok
    })
}

// ===========================================================================
// Settings
// ===========================================================================

#[no_mangle]
pub unsafe extern "C" fn emfe_get_setting_defs(
    instance: EmfeInstance,
    out: *mut *const EmfeSettingDef,
) -> i32 {
    ffi_catch!(0, {
    let inst = match inst_ref(instance) {
        Some(i) => i,
        None => return 0,
    };
    if out.is_null() {
        return 0;
    }
    *out = inst.settings_defs.as_ptr();
    inst.settings_defs.len() as i32
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_setting(
    instance: EmfeInstance,
    key: *const c_char,
) -> *const c_char {
    ffi_catch!(std::ptr::null::<c_char>() as *const c_char, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return b"\0".as_ptr() as *const c_char,
    };
    if key.is_null() {
        return b"\0".as_ptr() as *const c_char;
    }
    let k = CStr::from_ptr(key).to_string_lossy().into_owned();
    let val = inst.staged.get(&k).cloned().unwrap_or_default();
    inst.setting_value_buf = CString::new(val).unwrap();
    inst.setting_value_buf.as_ptr()
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_set_setting(
    instance: EmfeInstance,
    key: *const c_char,
    value: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    if key.is_null() || value.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let k = CStr::from_ptr(key).to_string_lossy().into_owned();
    let v = CStr::from_ptr(value).to_string_lossy().into_owned();
    inst.staged.insert(k, v);
    EmfeResult::Ok
    })
}
#[no_mangle]
pub unsafe extern "C" fn emfe_apply_settings(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    inst.settings = inst.staged.clone();

    // Only hot-swap-safe settings update `applied` immediately; REQUIRES_RESET
    // settings (BoardType, ConsoleBase, ResetVector, InitialStack) wait for emfe_reset.
    for (k, v) in inst.staged.iter() {
        let flags = inst.setting_flags.get(k).copied().unwrap_or(0);
        if flags & EMFE_SETTING_FLAG_REQUIRES_RESET == 0 {
            inst.applied.insert(k.clone(), v.clone());
        }
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_get_applied_setting(
    instance: EmfeInstance,
    key: *const c_char,
) -> *const c_char {
    ffi_catch!(std::ptr::null::<c_char>() as *const c_char, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return b"\0".as_ptr() as *const c_char,
    };
    if key.is_null() {
        return b"\0".as_ptr() as *const c_char;
    }
    let k = CStr::from_ptr(key).to_string_lossy().into_owned();
    let val = inst.applied.get(&k).cloned().unwrap_or_default();
    inst.applied_setting_value_buf = CString::new(val).unwrap();
    inst.applied_setting_value_buf.as_ptr()
    })
}

#[no_mangle]
pub extern "C" fn emfe_get_list_item_defs(
    _instance: EmfeInstance,
    _key: *const c_char,
    _out: *mut *const EmfeListItemDef,
) -> i32 {
    ffi_catch!(0, {
    0
    })
}
#[no_mangle]
pub extern "C" fn emfe_get_list_item_count(
    _instance: EmfeInstance,
    _key: *const c_char,
) -> i32 {
    ffi_catch!(0, {
    0
    })
}
#[no_mangle]
pub extern "C" fn emfe_get_list_item_field(
    _instance: EmfeInstance,
    _key: *const c_char,
    _index: i32,
    _field: *const c_char,
) -> *const c_char {
    ffi_catch!(std::ptr::null::<c_char>() as *const c_char, {
    b"\0".as_ptr() as *const c_char
    })
}
#[no_mangle]
pub extern "C" fn emfe_set_list_item_field(
    _instance: EmfeInstance,
    _key: *const c_char,
    _index: i32,
    _field: *const c_char,
    _value: *const c_char,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}
#[no_mangle]
pub extern "C" fn emfe_add_list_item(
    _instance: EmfeInstance,
    _key: *const c_char,
) -> i32 {
    ffi_catch!(0, {
    -1
    })
}
#[no_mangle]
pub extern "C" fn emfe_remove_list_item(
    _instance: EmfeInstance,
    _key: *const c_char,
    _index: i32,
) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    EmfeResult::ErrUnsupported
    })
}

// ---------------------------------------------------------------------------
// Settings persistence — matches em8 / z8000 / mc68030 convention:
// writes `<data_dir>/appsettings.json` where `data_dir` is the directory
// supplied by the frontend via `emfe_set_data_dir`, falling back to
// `%LOCALAPPDATA%\emfe_plugin_mc6809\` if the frontend did not set one.
// ---------------------------------------------------------------------------

fn data_dir_cell() -> &'static Mutex<Option<String>> {
    static CELL: OnceLock<Mutex<Option<String>>> = OnceLock::new();
    CELL.get_or_init(|| Mutex::new(None))
}

fn get_settings_path() -> Option<PathBuf> {
    let mut guard = data_dir_cell().lock().ok()?;
    if guard.is_none() {
        // Fallback: %LOCALAPPDATA%\emfe_plugin_mc6809
        if let Ok(lad) = std::env::var("LOCALAPPDATA") {
            let p = PathBuf::from(lad).join("emfe_plugin_mc6809");
            *guard = Some(p.to_string_lossy().into_owned());
        }
    }
    let dir = guard.as_ref()?.clone();
    if dir.is_empty() {
        return None;
    }
    let _ = fs::create_dir_all(&dir);
    Some(PathBuf::from(dir).join("appsettings.json"))
}

fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out
}

// Minimal JSON string-value extractor: looks for `"key"` followed by `:` then
// a quoted string, with the same backslash-escape handling as json_escape.
// Matches the approach used by em8 / z8000. Not a general JSON parser.
fn json_extract_string(content: &str, key: &str) -> Option<String> {
    let needle = format!("\"{}\"", key);
    let pos = content.find(&needle)?;
    let after_key = &content[pos + needle.len()..];
    let colon_rel = after_key.find(':')?;
    let after_colon = &after_key[colon_rel + 1..];
    let q1_rel = after_colon.find('"')?;
    let rest = &after_colon[q1_rel + 1..];
    let bytes = rest.as_bytes();
    let mut i = 0;
    let mut out = String::new();
    while i < bytes.len() {
        let c = bytes[i];
        if c == b'"' {
            return Some(out);
        } else if c == b'\\' && i + 1 < bytes.len() {
            let esc = bytes[i + 1];
            match esc {
                b'"' => out.push('"'),
                b'\\' => out.push('\\'),
                b'n' => out.push('\n'),
                b'r' => out.push('\r'),
                b't' => out.push('\t'),
                _ => {
                    out.push(c as char);
                    out.push(esc as char);
                }
            }
            i += 2;
        } else {
            out.push(c as char);
            i += 1;
        }
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn emfe_save_settings(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    let path = match get_settings_path() {
        Some(p) => p,
        None => {
            inst.last_error = CString::new("No data dir available").unwrap();
            return EmfeResult::ErrIo;
        }
    };

    let mut out = String::from("{\n");
    let mut first = true;
    for (k, v) in inst.settings.iter() {
        if !first {
            out.push_str(",\n");
        }
        out.push_str(&format!("  \"{}\": \"{}\"", json_escape(k), json_escape(v)));
        first = false;
    }
    out.push_str("\n}\n");

    if let Err(e) = fs::write(&path, out) {
        inst.last_error = CString::new(e.to_string()).unwrap_or_default();
        return EmfeResult::ErrIo;
    }
    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_load_settings(instance: EmfeInstance) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    let inst = match inst_mut(instance) {
        Some(i) => i,
        None => return EmfeResult::ErrInvalid,
    };
    let path = match get_settings_path() {
        Some(p) => p,
        None => return EmfeResult::Ok,  // no dir yet, nothing to load
    };
    if !path.exists() {
        return EmfeResult::Ok;
    }
    let content = match fs::read_to_string(&path) {
        Ok(s) => s,
        Err(_) => return EmfeResult::Ok,
    };

    // Only update keys that already exist in `settings` (i.e. known to the
    // plugin) — unknown keys in the file are silently ignored, matching the
    // em8 / z8000 behaviour.
    let known_keys: Vec<String> = inst.settings.keys().cloned().collect();
    for key in known_keys {
        if let Some(val) = json_extract_string(&content, &key) {
            inst.settings.insert(key, val);
        }
    }

    // Sync staged + applied to the loaded committed values so nothing is
    // marked pending at startup.
    inst.staged = inst.settings.clone();
    inst.applied = inst.settings.clone();

    // Push MC6809-specific settings (ConsoleBase etc.) that only take effect
    // at reset, so the device reflects the loaded state.
    let parse_u16 = |s: &str| -> Option<u16> {
        let s = s.trim();
        if let Some(hex) = s.strip_prefix("0x").or(s.strip_prefix("0X")) {
            u16::from_str_radix(hex, 16).ok()
        } else {
            s.parse::<u16>().ok()
        }
    };
    if let Some(v) = inst.settings.get("ConsoleBase").cloned() {
        if let Some(base) = parse_u16(&v) {
            inst.bus.acia.base = base;
        }
    }

    EmfeResult::Ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn emfe_set_data_dir(path: *const c_char) -> EmfeResult {
    ffi_catch!(EmfeResult::ErrMemory, {
    if path.is_null() {
        return EmfeResult::ErrInvalid;
    }
    let s = CStr::from_ptr(path).to_string_lossy().into_owned();
    if let Ok(mut guard) = data_dir_cell().lock() {
        *guard = Some(s);
        EmfeResult::Ok
    } else {
        EmfeResult::ErrMemory
    }
    })
}

// ===========================================================================
// String util
// ===========================================================================

#[no_mangle]
pub extern "C" fn emfe_release_string(_s: *const c_char) {
    ffi_catch!((), {
    // All strings are plugin-owned; nothing to release.
    })
}
