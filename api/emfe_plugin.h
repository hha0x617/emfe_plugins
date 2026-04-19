/*
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 *
 * emfe_plugin.h - Emulator Frontend Plugin Interface
 *
 * C ABI plugin interface for emulator backends.
 * Compatible with C++ (LoadLibrary/GetProcAddress) and C# (P/Invoke).
 *
 * Copyright (c) 2026 Em68030 Project
 * SPDX-License-Identifier: MIT
 */

#ifndef EMFE_PLUGIN_H
#define EMFE_PLUGIN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Version & ABI ---------- */

#define EMFE_API_VERSION_MAJOR 1
#define EMFE_API_VERSION_MINOR 0

#ifdef _WIN32
#  define EMFE_EXPORT __declspec(dllexport)
#  define EMFE_CALL   __cdecl
#else
#  define EMFE_EXPORT __attribute__((visibility("default")))
#  define EMFE_CALL
#endif

/* ---------- Opaque Handle ---------- */

typedef struct EmfeInstanceTag* EmfeInstance;

/* ---------- Enumerations ---------- */

typedef enum {
    EMFE_REG_INT    = 0,   /* Integer register (u64) */
    EMFE_REG_FLOAT  = 1,   /* 64-bit floating point (f64) */
    EMFE_REG_FLOAT80 = 2   /* 80-bit extended precision (f80 as bytes) */
} EmfeRegType;

typedef enum {
    EMFE_REG_FLAG_NONE      = 0,
    EMFE_REG_FLAG_READONLY  = 1 << 0,  /* Register cannot be modified */
    EMFE_REG_FLAG_PC        = 1 << 1,  /* This is the program counter */
    EMFE_REG_FLAG_SP        = 1 << 2,  /* This is the stack pointer */
    EMFE_REG_FLAG_FLAGS     = 1 << 3,  /* This is a flags/status register */
    EMFE_REG_FLAG_FPU       = 1 << 4,  /* FPU register */
    EMFE_REG_FLAG_MMU       = 1 << 5,  /* MMU register */
    EMFE_REG_FLAG_HIDDEN    = 1 << 6   /* Not shown by default */
} EmfeRegFlags;

typedef enum {
    EMFE_STATE_STOPPED  = 0,   /* CPU is stopped (idle) */
    EMFE_STATE_RUNNING  = 1,   /* CPU is running */
    EMFE_STATE_HALTED   = 2,   /* CPU halted (double fault, etc.) */
    EMFE_STATE_STEPPING = 3    /* Single-stepping */
} EmfeState;

typedef enum {
    EMFE_STOP_REASON_NONE       = 0,
    EMFE_STOP_REASON_USER       = 1,   /* User requested stop */
    EMFE_STOP_REASON_BREAKPOINT = 2,   /* Hit breakpoint */
    EMFE_STOP_REASON_WATCHPOINT = 3,   /* Hit watchpoint */
    EMFE_STOP_REASON_STEP       = 4,   /* Step completed */
    EMFE_STOP_REASON_HALT       = 5,   /* CPU halted */
    EMFE_STOP_REASON_EXCEPTION  = 6    /* Unhandled exception */
} EmfeStopReason;

typedef enum {
    EMFE_BP_EXEC  = 0,   /* Execution breakpoint */
    EMFE_BP_READ  = 1,   /* Read watchpoint */
    EMFE_BP_WRITE = 2,   /* Write watchpoint */
    EMFE_BP_RW    = 3    /* Read/Write watchpoint */
} EmfeBreakpointType;

typedef enum {
    EMFE_OK             =  0,
    EMFE_ERR_INVALID    = -1,   /* Invalid parameter */
    EMFE_ERR_STATE      = -2,   /* Invalid state for operation */
    EMFE_ERR_NOTFOUND   = -3,   /* Item not found */
    EMFE_ERR_IO         = -4,   /* I/O error */
    EMFE_ERR_MEMORY     = -5,   /* Out of memory */
    EMFE_ERR_UNSUPPORTED = -6   /* Not supported */
} EmfeResult;

/* ---------- Structures ---------- */

/* Register definition (data-driven UI) */
typedef struct {
    uint32_t    reg_id;         /* Unique register ID */
    const char* name;           /* Display name: "D0", "PC", "FP0" */
    const char* group;          /* Group name: "Data", "Address", "System", "FPU", "MMU" */
    EmfeRegType type;           /* Value type */
    uint32_t    bit_width;      /* 8, 16, 32, 64, 80 */
    uint32_t    flags;          /* EmfeRegFlags bitmask */
} EmfeRegisterDef;

/* Register value (for batch get/set) */
typedef struct {
    uint32_t reg_id;
    union {
        uint64_t u64;
        double   f64;
        uint8_t  f80[10];       /* 80-bit extended, little-endian */
    } value;
} EmfeRegValue;

/* Disassembly line */
typedef struct {
    uint64_t    address;        /* Instruction address */
    const char* raw_bytes;      /* Hex bytes: "4E71" (plugin-owned) */
    const char* mnemonic;       /* "NOP" (plugin-owned) */
    const char* operands;       /* "#$1234,D0" (plugin-owned) */
    uint32_t    length;         /* Instruction length in bytes */
} EmfeDisasmLine;

/* Breakpoint info */
typedef struct {
    uint64_t    address;
    bool        enabled;
    const char* condition;      /* Condition expression or NULL */
} EmfeBreakpointInfo;

/* Plugin capability flags — returned in EmfeBoardInfo::capabilities. The
 * frontend uses these to enable/disable menu items, toolbar buttons, and
 * panels that would otherwise call into unsupported entry points. A plugin
 * that sets a flag MUST implement the corresponding API with meaningful
 * behaviour; a plugin that clears a flag MAY leave the entry point as a
 * stub returning EMFE_ERR_UNSUPPORTED. */
#define EMFE_CAP_LOAD_ELF        (1ULL <<  0)  /* emfe_load_elf */
#define EMFE_CAP_LOAD_SREC       (1ULL <<  1)  /* emfe_load_srec */
#define EMFE_CAP_LOAD_BINARY     (1ULL <<  2)  /* emfe_load_binary */
#define EMFE_CAP_STEP_OVER       (1ULL <<  3)  /* emfe_step_over */
#define EMFE_CAP_STEP_OUT        (1ULL <<  4)  /* emfe_step_out */
#define EMFE_CAP_CALL_STACK      (1ULL <<  5)  /* emfe_get_call_stack */
#define EMFE_CAP_WATCHPOINTS     (1ULL <<  6)  /* emfe_add_watchpoint et al. */
#define EMFE_CAP_FRAMEBUFFER     (1ULL <<  7)  /* emfe_get_framebuffer_info */
#define EMFE_CAP_INPUT_KEYBOARD  (1ULL <<  8)  /* emfe_push_key */
#define EMFE_CAP_INPUT_MOUSE     (1ULL <<  9)  /* emfe_push_mouse_* */

/* Board/plugin information */
typedef struct {
    const char* board_name;     /* "MVME147" */
    const char* cpu_name;       /* "MC68030" */
    const char* description;    /* Human-readable description */
    const char* version;        /* Plugin version string */
    uint64_t    capabilities;   /* Bitwise OR of EMFE_CAP_* flags */
} EmfeBoardInfo;

/* Negotiate request/response */
typedef struct {
    uint32_t api_version_major;
    uint32_t api_version_minor;
    uint32_t flags;             /* Reserved, set to 0 */
} EmfeNegotiateInfo;

/* Stop info (passed to state change callback) */
typedef struct {
    EmfeState       state;
    EmfeStopReason  stop_reason;
    uint64_t        stop_address;       /* PC at stop, if applicable */
    const char*     stop_message;       /* Human-readable reason or NULL */
} EmfeStateInfo;

/* ---------- Callback Types ---------- */

/* Console character output: called when emulator outputs a character */
typedef void (EMFE_CALL *EmfeConsoleCharCallback)(void* user_data, char ch);

/* State change: called when emulator state changes (stop, halt, etc.) */
typedef void (EMFE_CALL *EmfeStateChangeCallback)(void* user_data, const EmfeStateInfo* info);

/* Diagnostic output: called for debug/trace messages */
typedef void (EMFE_CALL *EmfeDiagnosticCallback)(void* user_data, const char* message);

/* ---------- Phase 1 API Functions ---------- */

/*
 * Discovery & Lifecycle
 */

/* Version negotiation. Returns EMFE_OK if compatible. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_negotiate(const EmfeNegotiateInfo* info);

/* Get board/plugin information. Pointers valid until DLL unload. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_board_info(EmfeBoardInfo* out_info);

/* Create a new emulator instance. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_create(EmfeInstance* out_instance);

/* Destroy an emulator instance. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance);

/*
 * Callbacks
 */

/* Set console character output callback. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_console_char_callback(
    EmfeInstance instance,
    EmfeConsoleCharCallback callback,
    void* user_data);

/* Set state change callback. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_state_change_callback(
    EmfeInstance instance,
    EmfeStateChangeCallback callback,
    void* user_data);

/* Set diagnostic output callback. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_diagnostic_callback(
    EmfeInstance instance,
    EmfeDiagnosticCallback callback,
    void* user_data);

/*
 * Registers
 */

/* Get register definitions. Returns count. out_defs points to plugin-owned array. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_register_defs(
    EmfeInstance instance,
    const EmfeRegisterDef** out_defs);

/* Get register values (batch). values array must have count entries with reg_id set. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_registers(
    EmfeInstance instance,
    EmfeRegValue* values,
    int32_t count);

/* Set register values (batch). */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_registers(
    EmfeInstance instance,
    const EmfeRegValue* values,
    int32_t count);

/*
 * Memory
 */

EMFE_EXPORT uint8_t  EMFE_CALL emfe_peek_byte(EmfeInstance instance, uint64_t address);
EMFE_EXPORT uint16_t EMFE_CALL emfe_peek_word(EmfeInstance instance, uint64_t address);
EMFE_EXPORT uint32_t EMFE_CALL emfe_peek_long(EmfeInstance instance, uint64_t address);

EMFE_EXPORT EmfeResult EMFE_CALL emfe_poke_byte(EmfeInstance instance, uint64_t address, uint8_t value);
EMFE_EXPORT EmfeResult EMFE_CALL emfe_poke_word(EmfeInstance instance, uint64_t address, uint16_t value);
EMFE_EXPORT EmfeResult EMFE_CALL emfe_poke_long(EmfeInstance instance, uint64_t address, uint32_t value);

/* Read a range of bytes. out_data must be at least length bytes. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_peek_range(
    EmfeInstance instance,
    uint64_t address,
    uint8_t* out_data,
    uint32_t length);

/* Get total addressable memory size. */
EMFE_EXPORT uint64_t EMFE_CALL emfe_get_memory_size(EmfeInstance instance);

/*
 * Disassembly
 */

/* Disassemble one instruction. out_line is plugin-owned, valid until next call. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_disassemble_one(
    EmfeInstance instance,
    uint64_t address,
    EmfeDisasmLine* out_line);

/* Disassemble a range. Returns count of lines written. out_lines must have max_lines capacity. */
EMFE_EXPORT int32_t EMFE_CALL emfe_disassemble_range(
    EmfeInstance instance,
    uint64_t start_address,
    uint64_t end_address,
    EmfeDisasmLine* out_lines,
    int32_t max_lines);

/* Get loaded program address range. Returns start/end of most recently loaded file. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_program_range(
    EmfeInstance instance,
    uint64_t* out_start,
    uint64_t* out_end);

/*
 * Execution Control
 */

/* Execute one instruction (synchronous). */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_step(EmfeInstance instance);

/* Step over (subroutine call = run to return). Async. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_step_over(EmfeInstance instance);

/* Step out (run until current subroutine returns). Async. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_step_out(EmfeInstance instance);

/* Start continuous execution (async). */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_run(EmfeInstance instance);

/* Stop execution (can be called from any thread). */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_stop(EmfeInstance instance);

/* Reset CPU. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_reset(EmfeInstance instance);

/* Get current execution state. */
EMFE_EXPORT EmfeState EMFE_CALL emfe_get_state(EmfeInstance instance);

/* Get instruction count. */
EMFE_EXPORT int64_t EMFE_CALL emfe_get_instruction_count(EmfeInstance instance);

/* Get cycle count. */
EMFE_EXPORT int64_t EMFE_CALL emfe_get_cycle_count(EmfeInstance instance);

/*
 * Breakpoints
 */

/* Add a breakpoint. Returns EMFE_OK or error. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_add_breakpoint(
    EmfeInstance instance,
    uint64_t address);

/* Remove a breakpoint. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_remove_breakpoint(
    EmfeInstance instance,
    uint64_t address);

/* Enable/disable a breakpoint. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_enable_breakpoint(
    EmfeInstance instance,
    uint64_t address,
    bool enabled);

/* Set breakpoint condition. condition=NULL to clear. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_breakpoint_condition(
    EmfeInstance instance,
    uint64_t address,
    const char* condition);

/* Clear all breakpoints. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_clear_breakpoints(EmfeInstance instance);

/* Get all breakpoints. Returns count. out_breakpoints must have max_count capacity. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_breakpoints(
    EmfeInstance instance,
    EmfeBreakpointInfo* out_breakpoints,
    int32_t max_count);

/*
 * Watchpoints (Phase 3)
 */

typedef enum {
    EMFE_WP_SIZE_BYTE = 1,
    EMFE_WP_SIZE_WORD = 2,
    EMFE_WP_SIZE_LONG = 4
} EmfeWatchpointSize;

typedef enum {
    EMFE_WP_READ      = 0,
    EMFE_WP_WRITE     = 1,
    EMFE_WP_READWRITE = 2
} EmfeWatchpointType;

typedef struct {
    uint64_t            address;
    EmfeWatchpointSize  size;
    EmfeWatchpointType  type;
    bool                enabled;
    const char*         condition;   /* optional */
} EmfeWatchpointInfo;

EMFE_EXPORT EmfeResult EMFE_CALL emfe_add_watchpoint(
    EmfeInstance instance,
    uint64_t address,
    EmfeWatchpointSize size,
    EmfeWatchpointType type);

EMFE_EXPORT EmfeResult EMFE_CALL emfe_remove_watchpoint(
    EmfeInstance instance,
    uint64_t address);

EMFE_EXPORT EmfeResult EMFE_CALL emfe_enable_watchpoint(
    EmfeInstance instance,
    uint64_t address,
    bool enabled);

/* Set watchpoint condition. condition=NULL to clear. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_watchpoint_condition(
    EmfeInstance instance,
    uint64_t address,
    const char* condition);

EMFE_EXPORT EmfeResult EMFE_CALL emfe_clear_watchpoints(EmfeInstance instance);

EMFE_EXPORT int32_t EMFE_CALL emfe_get_watchpoints(
    EmfeInstance instance,
    EmfeWatchpointInfo* out_watchpoints,
    int32_t max_count);

/*
 * Call Stack (Phase 3)
 */

typedef enum {
    EMFE_CALL_KIND_CALL      = 0,  /* BSR/JSR */
    EMFE_CALL_KIND_EXCEPTION = 1,
    EMFE_CALL_KIND_INTERRUPT = 2
} EmfeCallStackKind;

typedef struct {
    uint64_t            call_pc;     /* PC of BSR/JSR instruction */
    uint64_t            target_pc;   /* PC of subroutine entry */
    uint64_t            return_pc;   /* Expected return PC */
    uint64_t            frame_pointer; /* A7 or A6 */
    EmfeCallStackKind   kind;
    const char*         label;       /* symbolic name or NULL */
} EmfeCallStackEntry;

/* Get current call stack. Returns count, 0 for empty or if not supported. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_call_stack(
    EmfeInstance instance,
    EmfeCallStackEntry* out_entries,
    int32_t max_count);

/*
 * Framebuffer (Phase 3)
 */

typedef enum {
    EMFE_FB_FORMAT_INDEXED8  = 8,   /* 8-bit indexed (palette) */
    EMFE_FB_FORMAT_RGB565    = 16,  /* 16-bit RGB 5-6-5 */
    EMFE_FB_FORMAT_RGB888    = 24,  /* 24-bit RGB */
    EMFE_FB_FORMAT_RGBA8888  = 32   /* 32-bit RGBA */
} EmfeFramebufferFormat;

typedef struct {
    uint32_t    width;
    uint32_t    height;
    uint32_t    bpp;          /* bits per pixel */
    uint32_t    stride;       /* bytes per row */
    uint64_t    base_address; /* guest physical address */
    const uint8_t* pixels;    /* direct pointer to framebuffer memory (plugin-owned) */
    uint32_t    flags;        /* reserved */
} EmfeFramebufferInfo;

/* Get framebuffer info. Returns EMFE_ERR_UNSUPPORTED if framebuffer is disabled. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_framebuffer_info(
    EmfeInstance instance,
    EmfeFramebufferInfo* out_info);

/* Get a single palette entry (for INDEXED8 framebuffers). Returns 0xAARRGGBB. */
EMFE_EXPORT uint32_t EMFE_CALL emfe_get_palette_entry(
    EmfeInstance instance,
    uint32_t index);

/* Get all palette entries (for INDEXED8 framebuffers). out_colors receives
 * count colors in 0xAARRGGBB format. Returns the count of entries written. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_palette(
    EmfeInstance instance,
    uint32_t* out_colors,
    int32_t max_count);

/*
 * Input events (Phase 3)
 */

/* Push a key event. scancode is a platform-defined scan code.
 * pressed=true for key down, false for key up. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_key(
    EmfeInstance instance,
    uint32_t scancode,
    bool pressed);

/* Push a mouse move event. Delta relative to last position. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_mouse_move(
    EmfeInstance instance,
    int32_t dx,
    int32_t dy);

/* Push absolute mouse position (for pointer over framebuffer). */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_mouse_absolute(
    EmfeInstance instance,
    int32_t x,
    int32_t y);

/* Push a mouse button event. button: 0=left, 1=right, 2=middle. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_mouse_button(
    EmfeInstance instance,
    int32_t button,
    bool pressed);

/*
 * File Loading
 */

/* Load an ELF file into memory. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_elf(
    EmfeInstance instance,
    const char* file_path);

/* Load a raw binary file at the specified address. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_binary(
    EmfeInstance instance,
    const char* file_path,
    uint64_t load_address);

/* Load a Motorola S-Record file. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_srec(
    EmfeInstance instance,
    const char* file_path);

/* Get last error message. Returns plugin-owned string, valid until next call. */
EMFE_EXPORT const char* EMFE_CALL emfe_get_last_error(EmfeInstance instance);

/*
 * Settings (Phase 2)
 */

typedef enum {
    EMFE_SETTING_INT    = 0,
    EMFE_SETTING_STRING = 1,
    EMFE_SETTING_BOOL   = 2,
    EMFE_SETTING_COMBO  = 3,   /* String with enumerated choices */
    EMFE_SETTING_FILE   = 4,   /* File path with browse button */
    EMFE_SETTING_LIST   = 5    /* Dynamic list (e.g. SCSI disks) */
} EmfeSettingType;

/* Setting flags (bitmask for EmfeSettingDef.flags) */

/* The setting cannot be applied to a running emulator safely (it represents
 * a non-hot-swappable device). `emfe_apply_settings` commits the new value
 * but leaves the running hardware unchanged; the deferred change takes
 * effect at the next `emfe_reset` (or on a fresh `emfe_create`). UI should
 * render such settings with a pending indicator when the committed value
 * differs from the value currently applied to hardware (see
 * `emfe_get_applied_setting`). */
#define EMFE_SETTING_FLAG_REQUIRES_RESET  (1u << 0)

/* Setting definition (data-driven UI) */
typedef struct {
    const char*     key;            /* Unique key: "MemorySize", "TargetOS" */
    const char*     label;          /* Display label */
    const char*     group;          /* Tab/group: "General", "MVME147", "Advanced" */
    EmfeSettingType type;
    const char*     default_value;  /* Default as string */
    const char*     constraints;    /* Type-specific: "1..256" for INT, "NetBSD|Linux" for COMBO */
    const char*     depends_on;     /* Key of dependency, or NULL */
    const char*     depends_value;  /* Show only when depends_on == this value, or NULL */
    uint32_t        flags;          /* Bitmask of EMFE_SETTING_FLAG_* */
} EmfeSettingDef;

/* List item sub-field definition (for SETTING_LIST type) */
typedef struct {
    const char*     key;            /* Sub-field key: "Path", "ScsiId" */
    const char*     label;          /* Display label */
    EmfeSettingType type;           /* INT, STRING, FILE, COMBO */
    const char*     constraints;    /* Type-specific constraints */
} EmfeListItemDef;

/* Get setting definitions. Returns count. out_defs points to plugin-owned array. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_setting_defs(
    EmfeInstance instance,
    const EmfeSettingDef** out_defs);

/* Get a setting value as string. Returns plugin-owned string. */
EMFE_EXPORT const char* EMFE_CALL emfe_get_setting(
    EmfeInstance instance,
    const char* key);

/* Set a setting value (as string). Changes are staged until apply. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_setting(
    EmfeInstance instance,
    const char* key,
    const char* value);

/* Apply all staged settings. May require CPU stop for hardware changes.
 *
 * Settings flagged EMFE_SETTING_FLAG_REQUIRES_RESET are committed (future
 * reads of emfe_get_setting return the new value) but NOT applied to the
 * running hardware. They take effect at the next `emfe_reset`.
 * `emfe_get_applied_setting` returns the value currently in use, which may
 * lag behind the committed value until reset. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_apply_settings(EmfeInstance instance);

/* Returns the setting value currently applied to the running hardware.
 * For settings without EMFE_SETTING_FLAG_REQUIRES_RESET this equals the
 * value returned by emfe_get_setting. For reset-deferred settings the
 * returned string may lag until the next `emfe_reset`.
 *
 * Plugins that do not track applied state separately may simply forward
 * to emfe_get_setting. Returns a plugin-owned string, valid until the next
 * call on the same instance that could refresh the buffer. */
EMFE_EXPORT const char* EMFE_CALL emfe_get_applied_setting(
    EmfeInstance instance,
    const char* key);

/* LIST type: get sub-field definitions for a list setting. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_list_item_defs(
    EmfeInstance instance,
    const char* list_key,
    const EmfeListItemDef** out_defs);

/* LIST type: get item count. */
EMFE_EXPORT int32_t EMFE_CALL emfe_get_list_item_count(
    EmfeInstance instance,
    const char* list_key);

/* LIST type: get a sub-field value. Returns plugin-owned string. */
EMFE_EXPORT const char* EMFE_CALL emfe_get_list_item_field(
    EmfeInstance instance,
    const char* list_key,
    int32_t item_index,
    const char* field_key);

/* LIST type: set a sub-field value. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_list_item_field(
    EmfeInstance instance,
    const char* list_key,
    int32_t item_index,
    const char* field_key,
    const char* value);

/* LIST type: add a new item. Returns the new item index. */
EMFE_EXPORT int32_t EMFE_CALL emfe_add_list_item(
    EmfeInstance instance,
    const char* list_key);

/* LIST type: remove an item by index. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_remove_list_item(
    EmfeInstance instance,
    const char* list_key,
    int32_t item_index);

/* Save current settings to persistent storage. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_save_settings(EmfeInstance instance);

/* Load settings from persistent storage. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_settings(EmfeInstance instance);

/* Set the data directory for settings persistence. Must be called before
 * emfe_create() to take effect for initial load. The path is the directory
 * (e.g. %LOCALAPPDATA%\emfe_CsWPF), not the file. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_data_dir(const char* path);

/*
 * Console I/O
 */

/* Send a character to the emulator's console input. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_send_char(EmfeInstance instance, char ch);

/* Send a string to the emulator's console input. */
EMFE_EXPORT EmfeResult EMFE_CALL emfe_send_string(EmfeInstance instance, const char* str);

/*
 * String Utilities
 */

/* Release a string allocated by the plugin. */
EMFE_EXPORT void EMFE_CALL emfe_release_string(const char* str);

#ifdef __cplusplus
}
#endif

#endif /* EMFE_PLUGIN_H */
