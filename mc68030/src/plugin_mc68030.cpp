// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// plugin_mc68030.cpp - MC68030 plugin for emfe
// Wraps the Em68030 engine (Core + IO) as a C ABI plugin DLL.

#include "pch.h"
#include "emfe_plugin.h"

#include "Core/MC68030.h"
#include "Core/Memory.h"
#include "Core/Disassembler.h"
#include "IO/FileLoader.h"
#include "IO/ConsoleDevice.h"
#include "IO/PccDevice.h"
#include "IO/Wd33c93Device.h"
#include "IO/Z8530Device.h"
#include "IO/Mk48t02Device.h"
#include "IO/Mvme147IoSpaceDevice.h"
#include "IO/LanceDevice.h"
#include "IO/ScsiDisk.h"
#include "IO/ScsiCdrom.h"
#include "IO/HddDevice.h"
#include "IO/FramebufferDevice.h"
#include "IO/InputDevice.h"
#include "IO/Uart16550Device.h"
#include "IO/SlirpNetworkHandler.h"
#include "IO/TapNetworkHandler.h"
#include "IO/VirtualNetworkHandler.h"
#include "Config/EmulatorConfig.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// ============================================================================
// Register IDs
// ============================================================================

enum RegId : uint32_t {
    REG_D0 = 0, REG_D1, REG_D2, REG_D3, REG_D4, REG_D5, REG_D6, REG_D7,
    REG_A0 = 8, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7,
    REG_PC = 16,
    REG_SR = 17,
    REG_SSP = 18,
    REG_USP = 19,
    REG_VBR = 20,
    REG_CACR = 21,
    REG_CAAR = 22,
    REG_SFC = 23,
    REG_DFC = 24,
    // FPU
    REG_FP0 = 32, REG_FP1, REG_FP2, REG_FP3, REG_FP4, REG_FP5, REG_FP6, REG_FP7,
    REG_FPCR = 40,
    REG_FPSR = 41,
    REG_FPIAR = 42,
    // MMU
    REG_TC = 48,
    REG_TT0 = 49,
    REG_TT1 = 50,
    REG_MMUSR = 51,
    REG_CRP = 52,
    REG_SRP = 53,
    // Counters (read-only)
    REG_CYCLES = 60,
    REG_INSTRUCTIONS = 61,
    REG_COUNT
};

// ============================================================================
// Instance
// ============================================================================

struct EmfeInstanceData {
    // Core emulator
    std::unique_ptr<Em68030::Core::Memory> memory;
    std::unique_ptr<Em68030::Core::MC68030> cpu;
    std::unique_ptr<Em68030::Core::Disassembler> disasm;

    // IO devices
    std::unique_ptr<Em68030::IO::ConsoleDevice> consoleDevice;
    std::unique_ptr<Em68030::IO::PccDevice> pccDevice;
    std::unique_ptr<Em68030::IO::Z8530Device> sccDevice;
    std::unique_ptr<Em68030::IO::Mk48t02Device> rtcDevice;
    std::unique_ptr<Em68030::IO::Wd33c93Device> scsiDevice;
    std::unique_ptr<Em68030::IO::LanceDevice> lanceDevice;
    std::unique_ptr<Em68030::IO::Uart16550Device> uartDevice;
    std::unique_ptr<Em68030::IO::Mvme147IoSpaceDevice> ioSpaceDevice;
    std::unique_ptr<Em68030::IO::HddDevice> hddDevice;
    std::vector<std::unique_ptr<Em68030::IO::ScsiDisk>> scsiDisks;
    std::unique_ptr<Em68030::IO::ScsiCdrom> scsiCdrom;
    std::unique_ptr<Em68030::IO::FramebufferDevice> framebufferDevice;
    std::unique_ptr<Em68030::IO::InputDevice> inputDevice;
    int scsiCdromId = -1;

    // Boot stub state
    uint32_t brdIdAddress = 0;
    bool systemBooted = false;
    uint32_t programStartAddress = 0;
    uint32_t programEndAddress = 0;
    std::string lastLoadedFile;

    // State
    std::atomic<EmfeState> state{ EMFE_STATE_STOPPED };
    std::atomic<bool> stopRequested{ false };
    // Step-out target depth: stop when shadow stack drops to <= this value.
    // -1 = inactive (normal Run mode).
    std::atomic<int32_t> stepOutTargetDepth{ -1 };
    std::string lastError;

    // Emulation thread
    std::thread emulationThread;

    // Callbacks
    EmfeConsoleCharCallback consoleCharCb = nullptr;
    void* consoleCharUserData = nullptr;
    EmfeStateChangeCallback stateChangeCb = nullptr;
    void* stateChangeUserData = nullptr;
    EmfeDiagnosticCallback diagnosticCb = nullptr;
    void* diagnosticUserData = nullptr;

    // Breakpoints
    std::unordered_map<uint32_t, EmfeBreakpointInfo> breakpoints;
    std::unordered_set<uint32_t> enabledBreakpoints;

    // Watchpoints (Phase 3)
    struct WatchpointEntry {
        uint32_t address;
        uint32_t size;
        EmfeWatchpointType type;
        bool enabled;
        std::string condition;
    };
    std::unordered_map<uint32_t, WatchpointEntry> watchpoints;
    std::mutex watchpointsMutex;
    std::atomic<bool> watchpointHit{ false };
    uint32_t watchpointHitAddress = 0;

    // Register definitions (built once)
    std::vector<EmfeRegisterDef> regDefs;

    // Disassembly line string storage (valid until next call)
    struct DisasmStringStorage {
        std::string rawBytes;
        std::string mnemonic;
        std::string operands;
    };
    std::vector<DisasmStringStorage> disasmStorage;

    // Breakpoint condition string storage
    std::vector<std::string> bpConditionStorage;

    // Settings
    Em68030::Config::EmulatorConfig config;
    Em68030::Config::EmulatorConfig stagedConfig;
    Em68030::Config::EmulatorConfig appliedConfig; // last applied config for change detection
    std::vector<EmfeSettingDef> settingDefs;
    std::vector<EmfeListItemDef> scsiListItemDefs;
    std::string settingValueBuf;
    std::string appliedSettingValueBuf;  // buffer for emfe_get_applied_setting

    void BuildRegisterDefs();
    void BuildSettingDefs();
    void SetupGenericDevices();
    void SetupMvme147Devices();
    void TeardownDevices();
    void SetupTrapHandler();
    void Handle147BugCall();
    void WriteBoardIdPacket(uint32_t addr);
    void SetupMvme147BootStub(uint32_t topOfRam);
    void SetupMvme147LinuxBootStub(uint32_t topOfRam, uint32_t endOfKernel);
    void OutputChar(uint8_t ch);
    void NotifyStateChange(EmfeState newState, EmfeStopReason reason,
                           uint64_t addr = 0, const char* msg = nullptr);
    void EmulationLoop();
    static uint8_t ToBcd(int val) { return static_cast<uint8_t>(((val / 10) << 4) | (val % 10)); }
};

// ============================================================================
// Register definition builder
// ============================================================================

void EmfeInstanceData::BuildRegisterDefs() {
    regDefs.clear();

    auto addReg = [&](uint32_t id, const char* name, const char* group,
                      EmfeRegType type, uint32_t bits, uint32_t flags) {
        regDefs.push_back({ id, name, group, type, bits, flags });
    };

    // Data registers
    for (int i = 0; i < 8; i++) {
        static const char* names[] = { "D0","D1","D2","D3","D4","D5","D6","D7" };
        addReg(REG_D0 + i, names[i], "Data", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    }

    // Address registers
    for (int i = 0; i < 8; i++) {
        static const char* names[] = { "A0","A1","A2","A3","A4","A5","A6","A7" };
        uint32_t flags = (i == 7) ? EMFE_REG_FLAG_SP : EMFE_REG_FLAG_NONE;
        addReg(REG_A0 + i, names[i], "Address", EMFE_REG_INT, 32, flags);
    }

    // System registers
    addReg(REG_PC, "PC", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_PC);
    addReg(REG_SR, "SR", "System", EMFE_REG_INT, 16, EMFE_REG_FLAG_FLAGS);
    addReg(REG_SSP, "SSP", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    addReg(REG_USP, "USP", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    addReg(REG_VBR, "VBR", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    addReg(REG_CACR, "CACR", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    addReg(REG_CAAR, "CAAR", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    addReg(REG_SFC, "SFC", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);
    addReg(REG_DFC, "DFC", "System", EMFE_REG_INT, 32, EMFE_REG_FLAG_NONE);

    // FPU registers
    for (int i = 0; i < 8; i++) {
        static const char* names[] = { "FP0","FP1","FP2","FP3","FP4","FP5","FP6","FP7" };
        addReg(REG_FP0 + i, names[i], "FPU", EMFE_REG_FLOAT, 64, EMFE_REG_FLAG_FPU);
    }
    addReg(REG_FPCR, "FPCR", "FPU", EMFE_REG_INT, 32, EMFE_REG_FLAG_FPU);
    addReg(REG_FPSR, "FPSR", "FPU", EMFE_REG_INT, 32, EMFE_REG_FLAG_FPU);
    addReg(REG_FPIAR, "FPIAR", "FPU", EMFE_REG_INT, 32, EMFE_REG_FLAG_FPU);

    // MMU registers
    addReg(REG_TC, "TC", "MMU", EMFE_REG_INT, 32, EMFE_REG_FLAG_MMU);
    addReg(REG_TT0, "TT0", "MMU", EMFE_REG_INT, 32, EMFE_REG_FLAG_MMU);
    addReg(REG_TT1, "TT1", "MMU", EMFE_REG_INT, 32, EMFE_REG_FLAG_MMU);
    addReg(REG_MMUSR, "MMUSR", "MMU", EMFE_REG_INT, 16, EMFE_REG_FLAG_MMU);
    addReg(REG_CRP, "CRP", "MMU", EMFE_REG_INT, 64, EMFE_REG_FLAG_MMU);
    addReg(REG_SRP, "SRP", "MMU", EMFE_REG_INT, 64, EMFE_REG_FLAG_MMU);

    // Counters
    addReg(REG_CYCLES, "Cycles", "Counters", EMFE_REG_INT, 64,
           EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN);
    addReg(REG_INSTRUCTIONS, "Instructions", "Counters", EMFE_REG_INT, 64,
           EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN);
}

// ============================================================================
// State notification
// ============================================================================

void EmfeInstanceData::NotifyStateChange(EmfeState newState, EmfeStopReason reason,
                                          uint64_t addr, const char* msg) {
    state.store(newState, std::memory_order_release);
    if (stateChangeCb) {
        EmfeStateInfo info{};
        info.state = newState;
        info.stop_reason = reason;
        info.stop_address = addr;
        info.stop_message = msg;
        stateChangeCb(stateChangeUserData, &info);
    }
}

// ============================================================================
// Console output helper
// ============================================================================

void EmfeInstanceData::OutputChar(uint8_t ch) {
    if (consoleCharCb)
        consoleCharCb(consoleCharUserData, static_cast<char>(ch));
}

// ============================================================================
// TeardownDevices — destroy all devices for reconfiguration
// ============================================================================

void EmfeInstanceData::TeardownDevices() {
    // Stop emulation thread if running
    stopRequested.store(true);
    if (emulationThread.joinable())
        emulationThread.join();

    scsiDisks.clear();
    scsiCdrom.reset();
    inputDevice.reset();
    framebufferDevice.reset();
    uartDevice.reset();
    lanceDevice.reset();
    scsiDevice.reset();
    sccDevice.reset();
    rtcDevice.reset();
    pccDevice.reset();
    ioSpaceDevice.reset();
    hddDevice.reset();
    consoleDevice.reset();
    disasm.reset();
    cpu.reset();
    memory.reset();
    scsiCdromId = -1;
    systemBooted = false;
}

// ============================================================================
// SetupGenericDevices — minimal board (ConsoleDevice + TRAP #15)
// ============================================================================

void EmfeInstanceData::SetupGenericDevices() {
    memory = std::make_unique<Em68030::Core::Memory>();
    memory->AddRegion(0, config.MemorySize, Em68030::Core::RegionType::Ram);

    cpu = std::make_unique<Em68030::Core::MC68030>(*memory);
    cpu->JitEnabled = config.JitEnabled;
    cpu->JitMinBlockLength = config.JitMinBlockLength;
    cpu->JitCompileThreshold = static_cast<uint8_t>(config.JitCompileThreshold);

    consoleDevice = std::make_unique<Em68030::IO::ConsoleDevice>();
    consoleDevice->CharOutput = [this](char ch) { OutputChar(static_cast<uint8_t>(ch)); };
    consoleDevice->StringOutput = [this](const std::string& s) {
        for (char ch : s) OutputChar(static_cast<uint8_t>(ch));
    };

    cpu->DiagnosticOutput = [this](const std::string& msg) {
        if (diagnosticCb) diagnosticCb(diagnosticUserData, msg.c_str());
    };

    // Watchpoint hook
    cpu->OnMemoryAccess = [this](uint32_t addr, uint32_t accessSize, bool isWrite,
                                  uint32_t /*oldVal*/, uint32_t /*newVal*/) {
        if (!cpu->WatchpointsEnabled) return;
        std::lock_guard<std::mutex> lock(watchpointsMutex);
        for (auto& [wpAddr, wp] : watchpoints) {
            if (!wp.enabled) continue;
            // Check overlap: [addr, addr+accessSize) ∩ [wpAddr, wpAddr+wp.size)
            if (addr < wpAddr + wp.size && wpAddr < addr + accessSize) {
                bool typeMatch = (wp.type == EMFE_WP_READWRITE) ||
                                 (wp.type == EMFE_WP_WRITE && isWrite) ||
                                 (wp.type == EMFE_WP_READ && !isWrite);
                if (typeMatch) {
                    watchpointHit.store(true);
                    watchpointHitAddress = wpAddr;
                    return;
                }
            }
        }
    };

    // Framebuffer (optional, also available in Generic mode for testing)
    if (config.FramebufferEnabled) {
        framebufferDevice = std::make_unique<Em68030::IO::FramebufferDevice>(
            config.FramebufferWidth, config.FramebufferHeight,
            config.FramebufferBpp, config.ComputeVramBase());
        memory->RegisterDevice(Em68030::IO::FramebufferDevice::BASE_ADDRESS,
            Em68030::IO::FramebufferDevice::DEVICE_SIZE, framebufferDevice.get());
        inputDevice = std::make_unique<Em68030::IO::InputDevice>(
            static_cast<uint16_t>(config.FramebufferWidth),
            static_cast<uint16_t>(config.FramebufferHeight));
        memory->RegisterDevice(Em68030::IO::InputDevice::BASE_ADDRESS,
            Em68030::IO::InputDevice::DEVICE_SIZE, inputDevice.get());
    }

    disasm = std::make_unique<Em68030::Core::Disassembler>(*memory);
}

// ============================================================================
// SetupMvme147Devices — full MVME147 board
// ============================================================================

void EmfeInstanceData::SetupMvme147Devices() {
    // Memory
    memory = std::make_unique<Em68030::Core::Memory>();
    memory->AddRegion(0x00000000, config.MemorySize, Em68030::Core::RegionType::Ram);
    if (!config.Mvme147RomPath.empty())
        memory->AddRegion(0xFF800000, 4 * 1024 * 1024, Em68030::Core::RegionType::Rom);

    // CPU
    cpu = std::make_unique<Em68030::Core::MC68030>(*memory);
    cpu->JitEnabled = config.JitEnabled;
    cpu->JitMinBlockLength = config.JitMinBlockLength;
    cpu->JitCompileThreshold = static_cast<uint8_t>(config.JitCompileThreshold);

    // Devices
    pccDevice = std::make_unique<Em68030::IO::PccDevice>(*cpu);
    rtcDevice = std::make_unique<Em68030::IO::Mk48t02Device>();
    sccDevice = std::make_unique<Em68030::IO::Z8530Device>();
    scsiDevice = std::make_unique<Em68030::IO::Wd33c93Device>();

    // RTC configuration
    if (config.TargetOS != "Linux")
        rtcDevice->SetYearOffset(68);
    uint8_t ethAddr[] = { 0x21, 0x00, 0x00 };
    uint32_t kernelRamEnd = config.FramebufferEnabled
        ? config.ComputeVramBase()
        : static_cast<uint32_t>(config.MemorySize);
    rtcDevice->SetMvme147Config(kernelRamEnd, ethAddr, sizeof(ethAddr));

    // LANCE Ethernet — NetworkMode="None" means "the LANCE isn't on this
    // board at all", so we skip construction, memory mapping, interrupt
    // wiring, and the tick. Reads at $FFFE1800 then fall through to the
    // Mvme147IoSpaceDevice catch-all (returns 0), the guest autoconf
    // probe fails the LANCE magic check, and no ethernet interface shows
    // up in the guest. LanceDevice's ctor installs a VirtualNetworkHandler
    // by default so "Virtual" is implicit (no override).
    const bool networkPresent = (config.NetworkMode != "None");
    if (networkPresent) {
        lanceDevice = std::make_unique<Em68030::IO::LanceDevice>();
        lanceDevice->AttachMemory(memory.get());
        if (config.NetworkMode.find("TAP") != std::string::npos) {
            auto tapHandler = std::make_unique<Em68030::IO::TapNetworkHandler>(config.TapAdapterGuid);
            lanceDevice->SetNetworkHandler(std::move(tapHandler));
        } else if (config.NetworkMode.find("NAT") != std::string::npos) {
            auto gwIp = Em68030::IO::SlirpNetworkHandler::ParseIpAddress(config.NatGatewayIp);
            auto gwMac = Em68030::IO::SlirpNetworkHandler::ParseMacAddress(config.NatGatewayMac);
            auto natHandler = std::make_unique<Em68030::IO::SlirpNetworkHandler>(gwIp, gwMac);
            lanceDevice->SetNetworkHandler(std::move(natHandler));
        }
    }

    // Register I/O space (catch-all first, then specific devices override)
    ioSpaceDevice = std::make_unique<Em68030::IO::Mvme147IoSpaceDevice>();
    memory->RegisterDevice(0xFFFE0000, 0x10000, ioSpaceDevice.get());
    memory->RegisterDevice(0xFFFE1000, 48, pccDevice.get());
    memory->RegisterDevice(0xFFFE3000, 8, sccDevice.get());
    memory->RegisterDevice(0xFFFE4000, 4, scsiDevice.get());
    memory->RegisterDevice(0xFFFE0000, 2048, rtcDevice.get());
    if (lanceDevice)
        memory->RegisterDevice(0xFFFE1800, 4, lanceDevice.get());

    // Framebuffer (optional)
    if (config.FramebufferEnabled) {
        framebufferDevice = std::make_unique<Em68030::IO::FramebufferDevice>(
            config.FramebufferWidth, config.FramebufferHeight,
            config.FramebufferBpp, config.ComputeVramBase());
        memory->RegisterDevice(Em68030::IO::FramebufferDevice::BASE_ADDRESS,
            Em68030::IO::FramebufferDevice::DEVICE_SIZE, framebufferDevice.get());
        inputDevice = std::make_unique<Em68030::IO::InputDevice>(
            static_cast<uint16_t>(config.FramebufferWidth),
            static_cast<uint16_t>(config.FramebufferHeight));
        memory->RegisterDevice(Em68030::IO::InputDevice::BASE_ADDRESS,
            Em68030::IO::InputDevice::DEVICE_SIZE, inputDevice.get());
    }

    // Interrupt wiring
    if (lanceDevice) {
        lanceDevice->InterruptOutput = [this](bool active) {
            pccDevice->SetDeviceInterrupt("lance", active);
        };
    }
    sccDevice->InterruptOutput = [this](bool active) {
        pccDevice->SetDeviceInterrupt("scc", active);
    };
    scsiDevice->InterruptOutput = [this](bool active) {
        pccDevice->SetDeviceInterrupt("scsi", active);
    };

    // SCSI ↔ PCC bidirectional
    scsiDevice->AttachMemory(memory.get());
    scsiDevice->AttachPcc(pccDevice.get());
    pccDevice->SetScsiDevice(scsiDevice.get());

    // Console output from SCC
    sccDevice->GetChannelA().CharTransmitted = [this](uint8_t ch) { OutputChar(ch); };
    sccDevice->GetChannelB().CharTransmitted = [this](uint8_t ch) { OutputChar(ch); };

    // Linux UART
    if (config.TargetOS == "Linux") {
        uartDevice = std::make_unique<Em68030::IO::Uart16550Device>(0xFFFE2000);
        uartDevice->OnTransmit = [this](uint8_t ch) { OutputChar(ch); };
        memory->RegisterDevice(0xFFFE2000, 8, uartDevice.get());
    }

    // Tick handlers
    cpu->AddTickHandler([this]() {
        pccDevice->Tick();
        sccDevice->Tick(cpu->Stopped);
        if (lanceDevice) lanceDevice->Tick();
    });

    // RESET instruction
    cpu->OnResetInstruction = [this]() {
        pccDevice->HardwareReset();
    };

    // PCC watchdog reset (warm reboot)
    pccDevice->OnWatchdogReset = [this]() {
        if (cpu->DiagnosticOutput)
            cpu->DiagnosticOutput("\n[EMU] Watchdog reset triggered — performing warm reboot\n");
        pccDevice->HardwareReset();
        if (scsiDevice) scsiDevice->ResetBusState();
        systemBooted = false;
        cpu->GetMmu().Reset();
        cpu->GetMmu().FlushAll();

        if (!lastLoadedFile.empty() && std::filesystem::exists(lastLoadedFile)) {
            auto result = Em68030::IO::FileLoader::LoadElf(*memory, lastLoadedFile);
            cpu->PC = result.EntryPoint;
            programStartAddress = result.StartAddress;
            programEndAddress = result.EndAddress;
            uint32_t topOfRam = config.FramebufferEnabled
                ? config.ComputeVramBase()
                : static_cast<uint32_t>(config.MemorySize);
            if (config.TargetOS == "Linux")
                SetupMvme147LinuxBootStub(topOfRam, programEndAddress);
            else
                SetupMvme147BootStub(topOfRam);
            cpu->SR = 0x2700;
        } else {
            cpu->Reset();
        }
    };

    // Diagnostic output
    cpu->DiagnosticOutput = [this](const std::string& msg) {
        if (diagnosticCb) diagnosticCb(diagnosticUserData, msg.c_str());
    };

    // Watchpoint hook
    cpu->OnMemoryAccess = [this](uint32_t addr, uint32_t accessSize, bool isWrite,
                                  uint32_t /*oldVal*/, uint32_t /*newVal*/) {
        if (!cpu->WatchpointsEnabled) return;
        std::lock_guard<std::mutex> lock(watchpointsMutex);
        for (auto& [wpAddr, wp] : watchpoints) {
            if (!wp.enabled) continue;
            if (addr < wpAddr + wp.size && wpAddr < addr + accessSize) {
                bool typeMatch = (wp.type == EMFE_WP_READWRITE) ||
                                 (wp.type == EMFE_WP_WRITE && isWrite) ||
                                 (wp.type == EMFE_WP_READ && !isWrite);
                if (typeMatch) {
                    watchpointHit.store(true);
                    watchpointHitAddress = wpAddr;
                    return;
                }
            }
        }
    };

    // Load ROM
    if (!config.Mvme147RomPath.empty() && std::filesystem::exists(config.Mvme147RomPath)) {
        std::ifstream romFile(config.Mvme147RomPath, std::ios::binary);
        if (romFile) {
            std::vector<uint8_t> rom((std::istreambuf_iterator<char>(romFile)),
                                      std::istreambuf_iterator<char>());
            memory->LoadData(0xFF800000, rom);
        }
    }

    // Mount SCSI disks
    scsiDisks.clear();
    for (const auto& diskConfig : config.Mvme147ScsiDisks) {
        if (!diskConfig.Path.empty() && std::filesystem::exists(diskConfig.Path)) {
            auto disk = std::make_unique<Em68030::IO::ScsiDisk>();
            disk->MountImage(diskConfig.Path);
            scsiDevice->AttachTarget(diskConfig.ScsiId, disk.get());
            scsiDisks.push_back(std::move(disk));
        }
    }

    // Mount SCSI CD-ROM
    scsiCdrom = std::make_unique<Em68030::IO::ScsiCdrom>();
    if (!config.Mvme147ScsiCdromPath.empty() &&
        std::filesystem::exists(config.Mvme147ScsiCdromPath))
        scsiCdrom->MountImage(config.Mvme147ScsiCdromPath);
    scsiCdromId = config.Mvme147ScsiCdromId;
    scsiDevice->AttachTarget(scsiCdromId, scsiCdrom.get());

    // Placeholder generic devices
    consoleDevice = std::make_unique<Em68030::IO::ConsoleDevice>();
    consoleDevice->CharOutput = [this](char ch) { OutputChar(static_cast<uint8_t>(ch)); };
    consoleDevice->StringOutput = [this](const std::string& s) {
        for (char ch : s) OutputChar(static_cast<uint8_t>(ch));
    };
    disasm = std::make_unique<Em68030::Core::Disassembler>(*memory);
}

// ============================================================================
// SetupTrapHandler
// ============================================================================

void EmfeInstanceData::SetupTrapHandler() {
    if (!cpu) return;
    cpu->TrapExecuted = [this](int trapNum) {
        if (trapNum == 15) {
            cpu->TrapHandled = true;
            if (config.BoardType == "MVME147")
                Handle147BugCall();
            else if (consoleDevice)
                consoleDevice->HandleTrap(*cpu);
        }
    };
}

// ============================================================================
// Handle147BugCall — 147Bug TRAP #15 function handler
// ============================================================================

void EmfeInstanceData::Handle147BugCall() {
    uint16_t funcCode = cpu->ReadWord(cpu->PC);
    cpu->PC += 2;

    switch (funcCode) {
    case 0x0000: // .INCHR — no synchronous input in plugin
        cpu->SetFlagZ(true);
        break;

    case 0x0001: // .INSTAT
        cpu->SetFlagZ(true);
        break;

    case 0x0020: // .OUTCHR
        OutputChar(static_cast<uint8_t>(cpu->D[0] & 0xFF));
        break;

    case 0x0021: // .OUTSTR
    {
        uint32_t addr = cpu->A[0];
        for (int i = 0; i < 4096; i++) {
            uint8_t b = cpu->ReadByte(addr++);
            if (b == 0) break;
            OutputChar(b);
        }
        break;
    }

    case 0x0022: // .OUTLN
    {
        uint32_t addr = cpu->A[0];
        for (int i = 0; i < 4096; i++) {
            uint8_t b = cpu->ReadByte(addr++);
            if (b == 0) break;
            OutputChar(b);
        }
        OutputChar('\r');
        OutputChar('\n');
        break;
    }

    case 0x0026: // .PCRLF
        OutputChar('\r');
        OutputChar('\n');
        break;

    case 0x0053: // .RTC_RD
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        gmtime_s(&tm_buf, &tt);
        cpu->D[0] = (cpu->D[0] & 0xFFFFFF00) | ToBcd((tm_buf.tm_year - 68) % 100);
        cpu->D[1] = (cpu->D[1] & 0xFFFFFF00) | ToBcd(tm_buf.tm_mon + 1);
        cpu->D[2] = (cpu->D[2] & 0xFFFFFF00) | ToBcd(tm_buf.tm_mday);
        cpu->D[3] = (cpu->D[3] & 0xFFFFFF00) | ToBcd(tm_buf.tm_hour);
        cpu->D[4] = (cpu->D[4] & 0xFFFFFF00) | ToBcd(tm_buf.tm_min);
        cpu->D[5] = (cpu->D[5] & 0xFFFFFF00) | ToBcd(tm_buf.tm_sec);
        break;
    }

    case 0x0060: // .RETURN (alias)
    case 0x0063: // .RETURN
        cpu->Halted = true;
        cpu->StopReason = "147Bug .RETURN";
        break;

    case 0x0070: // .BRD_ID
    {
        if (systemBooted) {
            // Warm reboot
            if (cpu->DiagnosticOutput)
                cpu->DiagnosticOutput("\n[EMU] Warm reboot detected — reloading kernel\n");
            if (pccDevice) pccDevice->HardwareReset();
            if (scsiDevice) scsiDevice->ResetBusState();
            systemBooted = false;
            cpu->GetMmu().Reset();
            cpu->GetMmu().FlushAll();

            if (!lastLoadedFile.empty() && std::filesystem::exists(lastLoadedFile)) {
                auto result = Em68030::IO::FileLoader::LoadElf(*memory, lastLoadedFile);
                cpu->PC = result.EntryPoint;
                programStartAddress = result.StartAddress;
                programEndAddress = result.EndAddress;
                uint32_t topOfRam = config.FramebufferEnabled
                    ? config.ComputeVramBase()
                    : static_cast<uint32_t>(config.MemorySize);
                if (config.TargetOS == "Linux")
                    SetupMvme147LinuxBootStub(topOfRam, programEndAddress);
                else
                    SetupMvme147BootStub(topOfRam);
                cpu->SR = 0x2700;
            } else {
                cpu->Reset();
            }
            return;
        }
        systemBooted = true;
        uint32_t sp = cpu->A[7];
        cpu->WriteLong(sp, brdIdAddress);
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// WriteBoardIdPacket
// ============================================================================

void EmfeInstanceData::WriteBoardIdPacket(uint32_t addr) {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &tt);

    memory->PokeLong(addr + 0, 0x01234567);   // eye_catcher
    memory->PokeByte(addr + 4, 0x01);          // rev
    memory->PokeByte(addr + 5, static_cast<uint8_t>(tm_buf.tm_mon + 1));
    memory->PokeByte(addr + 6, static_cast<uint8_t>(tm_buf.tm_mday));
    memory->PokeByte(addr + 7, static_cast<uint8_t>(tm_buf.tm_year % 100));
    memory->PokeWord(addr + 8, 0x00E0);        // size
    memory->PokeWord(addr + 10, 0x0000);        // rsv1
    memory->PokeWord(addr + 12, 0x0147);        // model
    memory->PokeWord(addr + 14, 0x0053);        // suffix 'S'
    memory->PokeWord(addr + 16, 0x0002);        // options
    memory->PokeByte(addr + 18, 0x01);          // family (68K)
    memory->PokeByte(addr + 19, 0x03);          // cpu (68030)
    memory->PokeWord(addr + 20, 0x0000);
    memory->PokeWord(addr + 22, 0x0000);
    memory->PokeWord(addr + 24, 0x0000);
    memory->PokeWord(addr + 26, 0x0000);
    memory->PokeLong(addr + 28, 0x01470000);    // bug version

    const char* longname = "MVME147-010 ";
    for (int i = 0; i < 12; i++)
        memory->PokeByte(addr + 52 + static_cast<uint32_t>(i), static_cast<uint8_t>(longname[i]));

    const char* speed = "2500";
    for (int i = 0; i < 4; i++)
        memory->PokeByte(addr + 80 + static_cast<uint32_t>(i), static_cast<uint8_t>(speed[i]));
}

// ============================================================================
// SetupMvme147BootStub — NetBSD boot parameters
// ============================================================================

void EmfeInstanceData::SetupMvme147BootStub(uint32_t topOfRam) {
    uint32_t ssp = topOfRam - 0x3000;
    brdIdAddress = topOfRam - 0x2000;
    WriteBoardIdPacket(brdIdAddress);

    constexpr uint32_t PCC_WDSC_ADDR = 0xFFFE3000;
    uint32_t bootdevlun = config.Mvme147ScsiDisks.empty()
        ? 0u : static_cast<uint32_t>(config.Mvme147ScsiDisks[0].ScsiId);
    uint32_t bootArgs = ssp - 28;
    memory->PokeLong(bootArgs + 0,  0);              // return address
    memory->PokeLong(bootArgs + 4,  0);              // boothowto
    memory->PokeLong(bootArgs + 8,  PCC_WDSC_ADDR);  // bootaddr
    memory->PokeLong(bootArgs + 12, 0);              // bootctrllun
    memory->PokeLong(bootArgs + 16, bootdevlun);     // bootdevlun
    memory->PokeLong(bootArgs + 20, static_cast<uint32_t>(config.Mvme147BootPartition));
    memory->PokeLong(bootArgs + 24, 0);              // esyms

    cpu->VBR = 0;
    cpu->SSP = bootArgs;
    cpu->A[7] = bootArgs;
}

// ============================================================================
// SetupMvme147LinuxBootStub — Linux bootinfo chain
// ============================================================================

void EmfeInstanceData::SetupMvme147LinuxBootStub(uint32_t topOfRam, uint32_t endOfKernel) {
    uint32_t ssp = topOfRam - 0x3000;
    uint32_t biAddr = (endOfKernel + 3) & ~3u;

    // BI_MACHTYPE
    memory->PokeWord(biAddr + 0, 0x0001);
    memory->PokeWord(biAddr + 2, 8);
    memory->PokeLong(biAddr + 4, 6); // MACH_MVME147
    biAddr += 8;

    // BI_CPUTYPE
    memory->PokeWord(biAddr + 0, 0x0002);
    memory->PokeWord(biAddr + 2, 8);
    memory->PokeLong(biAddr + 4, (1 << 1)); // CPU_68030
    biAddr += 8;

    // BI_FPUTYPE
    memory->PokeWord(biAddr + 0, 0x0003);
    memory->PokeWord(biAddr + 2, 8);
    memory->PokeLong(biAddr + 4, (1 << 1)); // FPU_68882
    biAddr += 8;

    // BI_MMUTYPE
    memory->PokeWord(biAddr + 0, 0x0004);
    memory->PokeWord(biAddr + 2, 8);
    memory->PokeLong(biAddr + 4, (1 << 1)); // MMU_68030
    biAddr += 8;

    // BI_MEMCHUNK
    memory->PokeWord(biAddr + 0, 0x0005);
    memory->PokeWord(biAddr + 2, 12);
    memory->PokeLong(biAddr + 4, 0);
    memory->PokeLong(biAddr + 8, topOfRam);
    biAddr += 12;

    // BI_COMMAND_LINE
    const std::string& cmdline = config.LinuxCommandLine;
    uint32_t cmdLen = static_cast<uint32_t>(cmdline.size()) + 1;
    uint32_t cmdRecSize = 4 + ((cmdLen + 3) & ~3u);
    memory->PokeWord(biAddr + 0, 0x0007);
    memory->PokeWord(biAddr + 2, static_cast<uint16_t>(cmdRecSize));
    for (uint32_t i = 0; i < cmdLen; i++)
        memory->PokeByte(biAddr + 4 + i, i < cmdline.size() ? static_cast<uint8_t>(cmdline[i]) : 0);
    for (uint32_t i = cmdLen; i < ((cmdLen + 3) & ~3u); i++)
        memory->PokeByte(biAddr + 4 + i, 0);
    biAddr += cmdRecSize;

    // BI_VME_TYPE
    memory->PokeWord(biAddr + 0, 0x8000);
    memory->PokeWord(biAddr + 2, 8);
    memory->PokeLong(biAddr + 4, 0x0147); // VME_TYPE_MVME147
    biAddr += 8;

    // BI_VME_BRDINFO
    memory->PokeWord(biAddr + 0, 0x8001);
    memory->PokeWord(biAddr + 2, 4 + 24);
    for (uint32_t i = 0; i < 24; i++)
        memory->PokeByte(biAddr + 4 + i, 0);
    biAddr += 4 + 24;

    // BI_LAST
    memory->PokeWord(biAddr + 0, 0x0000);
    memory->PokeWord(biAddr + 2, 4);

    cpu->VBR = 0;
    cpu->SSP = ssp;
    cpu->A[7] = ssp;
}

// ============================================================================
// Setting definitions builder
// ============================================================================

// Helper: static string storage for setting def string fields
static std::vector<std::string> s_settingStrings;
static const char* ss(const std::string& s) {
    s_settingStrings.push_back(s);
    return s_settingStrings.back().c_str();
}

void EmfeInstanceData::BuildSettingDefs() {
    settingDefs.clear();
    s_settingStrings.clear();

    auto add = [&](const char* key, const char* label, const char* group,
                   EmfeSettingType type, const char* defVal = "",
                   const char* constraints = nullptr,
                   const char* dependsOn = nullptr, const char* dependsVal = nullptr,
                   uint32_t flags = 0) {
        settingDefs.push_back({ key, label, group, type, defVal, constraints,
                                dependsOn, dependsVal, flags });
    };
    // Alias: everything device-affecting requires a reset to apply safely.
    const uint32_t R = EMFE_SETTING_FLAG_REQUIRES_RESET;

    // General tab
    add("MemorySize",    "Memory Size (MB)",  "General", EMFE_SETTING_INT,    "48", "1|256", nullptr, nullptr, R);
    add("BoardType",     "Board Type",        "General", EMFE_SETTING_COMBO,  "Generic", "Generic|MVME147", nullptr, nullptr, R);
    add("TargetOS",      "Target OS",         "MVME147", EMFE_SETTING_COMBO,  "NetBSD", "NetBSD|Linux", "BoardType", "MVME147", R);
    add("Theme",         "Theme",             "General", EMFE_SETTING_COMBO,  "Dark", "Dark|Light|System");

    // MVME147 tab
    add("Mvme147RomPath",      "ROM Path",            "MVME147", EMFE_SETTING_FILE,  "", nullptr, "BoardType", "MVME147", R);
    add("Mvme147ScsiDisks",    "SCSI Disks",          "MVME147", EMFE_SETTING_LIST,  "", nullptr, "BoardType", "MVME147", R);
    add("Mvme147ScsiCdromPath","SCSI CD-ROM Path",    "MVME147", EMFE_SETTING_FILE,  "", nullptr, "BoardType", "MVME147", R);
    add("Mvme147ScsiCdromId",  "SCSI CD-ROM ID",      "MVME147", EMFE_SETTING_COMBO, "3", "0|1|2|3|4|5|6|7", "BoardType", "MVME147", R);
    add("NetBsdKernelImagePath","NetBSD Kernel Path",  "MVME147", EMFE_SETTING_FILE,  "", nullptr, "TargetOS", "NetBSD", R);
    add("Mvme147BootPartition","Boot Partition",       "MVME147", EMFE_SETTING_COMBO, "a", "a|b|c|d|e|f|g|h", "TargetOS", "NetBSD", R);
    add("LinuxKernelImagePath", "Linux Kernel Path",   "MVME147", EMFE_SETTING_FILE,  "", nullptr, "TargetOS", "Linux", R);
    add("LinuxCommandLine",     "Linux Command Line",  "MVME147", EMFE_SETTING_STRING,"root=/dev/sda1 console=ttyS0", nullptr, "TargetOS", "Linux", R);

    // Network tab
    add("NetworkMode",   "Network Mode",      "Network", EMFE_SETTING_COMBO,  "Virtual", "Virtual|NAT|TAP|None", nullptr, nullptr, R);
    add("NatGatewayIp",  "NAT Gateway IP",    "Network", EMFE_SETTING_STRING, "10.0.2.2", nullptr, "NetworkMode", "NAT", R);
    add("TapAdapterGuid","TAP Adapter GUID",  "Network", EMFE_SETTING_STRING, "", nullptr, "NetworkMode", "TAP", R);

    // Console tab — purely UI, hot-swappable
    add("ConsoleScrollbackLines","Scrollback Lines",  "Console", EMFE_SETTING_INT,  "2000", "0|100000");
    add("ConsoleColumns",        "Columns",           "Console", EMFE_SETTING_INT,  "80",   "40|320");
    add("ConsoleRows",           "Rows",              "Console", EMFE_SETTING_INT,  "24",   "10|100");

    // Performance tab — hot tuning, hot-swappable
    add("JitEnabled",          "JIT Compiler",        "Performance", EMFE_SETTING_BOOL,  "false");
    add("JitMinBlockLength",   "JIT Min Block Length", "Performance", EMFE_SETTING_INT,   "3", "1|32");
    add("JitCompileThreshold", "JIT Compile Threshold","Performance", EMFE_SETTING_INT,   "32", "1|255");

    // Advanced tab — debugger options, hot-swappable
    add("CallStackMode",      "Call Stack Mode",     "Advanced",    EMFE_SETTING_COMBO, "ShadowStack", "ShadowStack|A6Chain");

    // Framebuffer tab — device, requires reset
    add("FramebufferEnabled",  "Enable Framebuffer",  "Framebuffer", EMFE_SETTING_BOOL,  "false", nullptr, nullptr, nullptr, R);
    add("FramebufferWidth",    "Width",               "Framebuffer", EMFE_SETTING_INT,   "640", "320|1920", nullptr, nullptr, R);
    add("FramebufferHeight",   "Height",              "Framebuffer", EMFE_SETTING_INT,   "480", "240|1200", nullptr, nullptr, R);
    add("FramebufferBpp",      "Bits Per Pixel",      "Framebuffer", EMFE_SETTING_COMBO, "16", "8|16|32", nullptr, nullptr, R);

    // SCSI disk list item sub-fields
    scsiListItemDefs.clear();
    scsiListItemDefs.push_back({ "Path",   "Disk Image Path", EMFE_SETTING_FILE, nullptr });
    scsiListItemDefs.push_back({ "ScsiId", "SCSI ID",         EMFE_SETTING_INT,  "0|7" });
}

// ============================================================================
// Condition expression evaluator
// Supports: D0-D7, A0-A7, PC, SR, SSP registers
//           $hex, 0xhex, decimal literals
//           ==, !=, <, >, <=, >=, &, &&, ||, ()
// ============================================================================

namespace {

struct CondParser {
    const char* p;
    const Em68030::Core::MC68030& cpu;

    void skipWS() { while (*p == ' ' || *p == '\t') ++p; }

    bool tryChar(char c) { skipWS(); if (*p == c) { ++p; return true; } return false; }

    uint32_t parseNumber() {
        skipWS();
        if (*p == '$') { ++p; return static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 16)); }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; return static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 16)); }
        return static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 10));
    }

    bool tryReg(uint32_t& val) {
        skipWS();
        if ((p[0] == 'D' || p[0] == 'd') && p[1] >= '0' && p[1] <= '7') {
            val = cpu.D[p[1] - '0']; p += 2; return true;
        }
        if ((p[0] == 'A' || p[0] == 'a') && p[1] >= '0' && p[1] <= '7') {
            val = cpu.A[p[1] - '0']; p += 2; return true;
        }
        if ((p[0] == 'P' || p[0] == 'p') && (p[1] == 'C' || p[1] == 'c') &&
            !(p[2] >= '0' && p[2] <= '9') && !(p[2] >= 'A' && p[2] <= 'Z') && !(p[2] >= 'a' && p[2] <= 'z')) {
            val = cpu.PC; p += 2; return true;
        }
        if ((p[0] == 'S' || p[0] == 's') && (p[1] == 'R' || p[1] == 'r') &&
            !(p[2] >= '0' && p[2] <= '9') && !(p[2] >= 'A' && p[2] <= 'Z') && !(p[2] >= 'a' && p[2] <= 'z')) {
            val = cpu.SR; p += 2; return true;
        }
        if (((p[0] == 'S' || p[0] == 's') && (p[1] == 'S' || p[1] == 's') && (p[2] == 'P' || p[2] == 'p')) &&
            !(p[3] >= '0' && p[3] <= '9') && !(p[3] >= 'A' && p[3] <= 'Z') && !(p[3] >= 'a' && p[3] <= 'z')) {
            val = cpu.SSP; p += 3; return true;
        }
        return false;
    }

    uint32_t parsePrimary() {
        skipWS();
        uint32_t val;
        if (tryReg(val)) return val;
        if (*p == '(') { ++p; val = parseOr(); tryChar(')'); return val; }
        return parseNumber();
    }

    uint32_t parseBitAnd() {
        uint32_t left = parsePrimary();
        while (true) {
            skipWS();
            if (*p == '&' && p[1] != '&') { ++p; left &= parsePrimary(); }
            else break;
        }
        return left;
    }

    uint32_t parseCompare() {
        uint32_t left = parseBitAnd();
        skipWS();
        if (p[0] == '=' && p[1] == '=') { p += 2; return left == parseBitAnd() ? 1u : 0u; }
        if (p[0] == '!' && p[1] == '=') { p += 2; return left != parseBitAnd() ? 1u : 0u; }
        if (p[0] == '<' && p[1] == '=') { p += 2; return left <= parseBitAnd() ? 1u : 0u; }
        if (p[0] == '>' && p[1] == '=') { p += 2; return left >= parseBitAnd() ? 1u : 0u; }
        if (p[0] == '<') { ++p; return left < parseBitAnd() ? 1u : 0u; }
        if (p[0] == '>') { ++p; return left > parseBitAnd() ? 1u : 0u; }
        return left;
    }

    uint32_t parseAnd() {
        uint32_t left = parseCompare();
        while (true) {
            skipWS();
            if (p[0] == '&' && p[1] == '&') { p += 2; uint32_t right = parseCompare(); left = (left && right) ? 1u : 0u; }
            else break;
        }
        return left;
    }

    uint32_t parseOr() {
        uint32_t left = parseAnd();
        while (true) {
            skipWS();
            if (p[0] == '|' && p[1] == '|') { p += 2; uint32_t right = parseAnd(); left = (left || right) ? 1u : 0u; }
            else break;
        }
        return left;
    }

    bool evaluate() { return parseOr() != 0; }
};

bool EvaluateCondition(const Em68030::Core::MC68030& cpu, const char* condition) {
    if (!condition || !*condition) return true;
    try {
        CondParser parser{ condition, cpu };
        return parser.evaluate();
    } catch (...) {
        return true;
    }
}

} // anonymous namespace

// ============================================================================
// Emulation loop (runs on worker thread)
// ============================================================================

void EmfeInstanceData::EmulationLoop() {
    try {
        auto& c = *cpu;
        while (!stopRequested.load(std::memory_order_relaxed)) {
            // Check breakpoints (with condition evaluation)
            if (!enabledBreakpoints.empty() && enabledBreakpoints.contains(c.PC)) {
                auto bpIt = breakpoints.find(c.PC);
                if (bpIt == breakpoints.end() || !bpIt->second.condition ||
                    EvaluateCondition(c, bpIt->second.condition)) {
                    NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_BREAKPOINT, c.PC, "Breakpoint hit");
                    return;
                }
            }

            // Execute one instruction. Bus errors are signalled via the CPU's
            // BusErrorPending flag (set by Mmu/Memory), and HandleBusError is
            // invoked inside ExecuteNextFast/ExecuteNextFastJit before they
            // return — so this loop no longer needs a try/catch around
            // BusErrorException. The exception-based path was removed because
            // its CRT unwinder intermittently AV'd under the .NET host.
            bool ok;
            if (c.JitEnabled)
                ok = c.ExecuteNextFastJit();
            else
                ok = c.ExecuteNextFast();

            if (!ok) {
                if (c.Halted || (!c.HasExternalDevices() && c.Stopped)) {
                    NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_HALT, c.PC,
                                      c.StopReason.empty() ? "CPU halted" : c.StopReason.c_str());
                    return;
                }
                continue; // STOP waiting for interrupt — keep looping
            }
            if (c.Halted) {
                NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_HALT, c.PC,
                                  c.StopReason.empty() ? "CPU halted" : c.StopReason.c_str());
                return;
            }

            // Check watchpoint hit (with condition evaluation)
            if (watchpointHit.exchange(false)) {
                bool shouldStop = true;
                {
                    std::lock_guard<std::mutex> lock(watchpointsMutex);
                    auto wpIt = watchpoints.find(watchpointHitAddress);
                    if (wpIt != watchpoints.end() && !wpIt->second.condition.empty())
                        shouldStop = EvaluateCondition(c, wpIt->second.condition.c_str());
                }
                if (shouldStop) {
                    NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_WATCHPOINT,
                                      watchpointHitAddress, "Watchpoint hit");
                    return;
                }
            }

            // Step-out target: stop when shadow stack has unwound to or below target depth
            int32_t target = stepOutTargetDepth.load(std::memory_order_relaxed);
            if (target >= 0 && c._shadowStackTop <= target) {
                stepOutTargetDepth.store(-1, std::memory_order_relaxed);
                NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_STEP, c.PC, "Step out");
                return;
            }
        }

        NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_USER, c.PC, "Stopped by user");
    } catch (const std::exception& ex) {
        lastError = std::string("Emulation exception: ") + ex.what();
        NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_EXCEPTION,
                          cpu ? cpu->PC : 0, lastError.c_str());
    } catch (...) {
        lastError = "Emulation: unknown exception";
        NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_EXCEPTION,
                          cpu ? cpu->PC : 0, lastError.c_str());
    }
}

// ============================================================================
// Board info (static)
// ============================================================================

static EmfeBoardInfo s_boardInfo = {
    "MVME147",
    "MC68030",
    "Motorola MVME147 board with MC68030 CPU, MC68882 FPU, WD33C93 SCSI, LANCE Ethernet",
    "1.0.0",
    EMFE_CAP_LOAD_ELF | EMFE_CAP_LOAD_SREC | EMFE_CAP_LOAD_BINARY |
    EMFE_CAP_STEP_OVER | EMFE_CAP_STEP_OUT | EMFE_CAP_CALL_STACK |
    EMFE_CAP_WATCHPOINTS | EMFE_CAP_FRAMEBUFFER |
    EMFE_CAP_INPUT_KEYBOARD | EMFE_CAP_INPUT_MOUSE |
    EMFE_CAP_CONSOLE_TX_SPACE
};

// ============================================================================
// API Implementation
// ============================================================================

EmfeResult EMFE_CALL emfe_negotiate(const EmfeNegotiateInfo* info) {
    if (!info) return EMFE_ERR_INVALID;
    if (info->api_version_major != EMFE_API_VERSION_MAJOR) return EMFE_ERR_UNSUPPORTED;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_get_board_info(EmfeBoardInfo* out_info) {
    if (!out_info) return EMFE_ERR_INVALID;
    *out_info = s_boardInfo;
    return EMFE_OK;
}

// Forward declaration (defined later in Settings section)
static std::filesystem::path GetSettingsPath();

EmfeResult EMFE_CALL emfe_create(EmfeInstance* out_instance) {
    if (!out_instance) return EMFE_ERR_INVALID;

    auto inst = new (std::nothrow) EmfeInstanceData();
    if (!inst) return EMFE_ERR_MEMORY;

    try {
        // Load settings from emfe-managed location (set via emfe_set_data_dir or default)
        try {
            auto path = GetSettingsPath();
            if (std::filesystem::exists(path)) {
                std::ifstream ifs(path);
                if (ifs.is_open()) {
                    nlohmann::json j = nlohmann::json::parse(ifs);
                    inst->config = j.get<Em68030::Config::EmulatorConfig>();
                }
            }
        } catch (...) {
            // Reset to default config on any load error
            inst->config = Em68030::Config::EmulatorConfig{};
        }
        inst->stagedConfig = inst->config;
        inst->appliedConfig = inst->config;

        // Setup devices based on board type
        // If MVME147 setup fails, fall back to Generic
        try {
            if (inst->config.BoardType == "MVME147")
                inst->SetupMvme147Devices();
            else
                inst->SetupGenericDevices();
        } catch (const std::exception& ex) {
            // MVME147 setup failed — teardown and fall back to Generic
            inst->TeardownDevices();
            inst->config.BoardType = "Generic";
            inst->stagedConfig.BoardType = "Generic";
            inst->appliedConfig.BoardType = "Generic";
            inst->SetupGenericDevices();
            inst->lastError = std::string("MVME147 setup failed, fell back to Generic: ") + ex.what();
        }
        inst->SetupTrapHandler();

        inst->BuildRegisterDefs();
        inst->BuildSettingDefs();

        *out_instance = reinterpret_cast<EmfeInstance>(inst);
        return EMFE_OK;
    } catch (const std::exception& ex) {
        delete inst;
        return EMFE_ERR_IO;
    } catch (...) {
        delete inst;
        return EMFE_ERR_MEMORY;
    }
}

EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    // Stop emulation if running
    inst->stopRequested.store(true, std::memory_order_release);
    if (inst->emulationThread.joinable()) {
        inst->emulationThread.join();
    }

    delete inst;
    return EMFE_OK;
}

// ---------- Callbacks ----------

EmfeResult EMFE_CALL emfe_set_console_char_callback(
    EmfeInstance instance, EmfeConsoleCharCallback callback, void* user_data) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->consoleCharCb = callback;
    inst->consoleCharUserData = user_data;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_state_change_callback(
    EmfeInstance instance, EmfeStateChangeCallback callback, void* user_data) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->stateChangeCb = callback;
    inst->stateChangeUserData = user_data;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_diagnostic_callback(
    EmfeInstance instance, EmfeDiagnosticCallback callback, void* user_data) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->diagnosticCb = callback;
    inst->diagnosticUserData = user_data;
    return EMFE_OK;
}

// ---------- Registers ----------

int32_t EMFE_CALL emfe_get_register_defs(EmfeInstance instance, const EmfeRegisterDef** out_defs) {
    if (!instance || !out_defs) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    *out_defs = inst->regDefs.data();
    return static_cast<int32_t>(inst->regDefs.size());
}

EmfeResult EMFE_CALL emfe_get_registers(EmfeInstance instance, EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    auto& c = *inst->cpu;

    for (int32_t i = 0; i < count; i++) {
        auto& v = values[i];
        v.value.u64 = 0;

        switch (v.reg_id) {
        case REG_D0: case REG_D1: case REG_D2: case REG_D3:
        case REG_D4: case REG_D5: case REG_D6: case REG_D7:
            v.value.u64 = c.D[v.reg_id - REG_D0];
            break;
        case REG_A0: case REG_A1: case REG_A2: case REG_A3:
        case REG_A4: case REG_A5: case REG_A6: case REG_A7:
            v.value.u64 = c.A[v.reg_id - REG_A0];
            break;
        case REG_PC:   v.value.u64 = c.PC; break;
        case REG_SR:   v.value.u64 = c.SR; break;
        case REG_SSP:  v.value.u64 = c.SSP; break;
        case REG_USP:  v.value.u64 = c.USP; break;
        case REG_VBR:  v.value.u64 = c.VBR; break;
        case REG_CACR: v.value.u64 = c.CACR; break;
        case REG_CAAR: v.value.u64 = c.CAAR; break;
        case REG_SFC:  v.value.u64 = c.SFC; break;
        case REG_DFC:  v.value.u64 = c.DFC; break;
        // FPU
        case REG_FP0: case REG_FP1: case REG_FP2: case REG_FP3:
        case REG_FP4: case REG_FP5: case REG_FP6: case REG_FP7:
            v.value.f64 = c.GetFpu().FP[v.reg_id - REG_FP0];
            break;
        case REG_FPCR:  v.value.u64 = c.GetFpu().FPCR; break;
        case REG_FPSR:  v.value.u64 = c.GetFpu().FPSR; break;
        case REG_FPIAR: v.value.u64 = c.GetFpu().FPIAR; break;
        // MMU
        case REG_TC:    v.value.u64 = c.GetMmu().GetTC(); break;
        case REG_TT0:   v.value.u64 = c.GetMmu().GetTT0(); break;
        case REG_TT1:   v.value.u64 = c.GetMmu().GetTT1(); break;
        case REG_MMUSR: v.value.u64 = c.GetMmu().MMUSR; break;
        case REG_CRP:   v.value.u64 = c.GetMmu().CRP; break;
        case REG_SRP:   v.value.u64 = c.GetMmu().SRP; break;
        // Counters
        case REG_CYCLES:       v.value.u64 = static_cast<uint64_t>(c.CycleCount); break;
        case REG_INSTRUCTIONS: v.value.u64 = static_cast<uint64_t>(c.InstructionCount); break;
        default:
            return EMFE_ERR_INVALID;
        }
    }
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_registers(EmfeInstance instance, const EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;
    auto& c = *inst->cpu;

    for (int32_t i = 0; i < count; i++) {
        const auto& v = values[i];
        switch (v.reg_id) {
        case REG_D0: case REG_D1: case REG_D2: case REG_D3:
        case REG_D4: case REG_D5: case REG_D6: case REG_D7:
            c.D[v.reg_id - REG_D0] = static_cast<uint32_t>(v.value.u64);
            break;
        case REG_A0: case REG_A1: case REG_A2: case REG_A3:
        case REG_A4: case REG_A5: case REG_A6: case REG_A7:
            c.A[v.reg_id - REG_A0] = static_cast<uint32_t>(v.value.u64);
            break;
        case REG_PC:   c.PC   = static_cast<uint32_t>(v.value.u64); break;
        case REG_SR:   c.SetSR(static_cast<uint16_t>(v.value.u64)); break;
        case REG_SSP:  c.SSP  = static_cast<uint32_t>(v.value.u64); break;
        case REG_USP:  c.USP  = static_cast<uint32_t>(v.value.u64); break;
        case REG_VBR:  c.VBR  = static_cast<uint32_t>(v.value.u64); break;
        case REG_CACR: c.CACR = static_cast<uint32_t>(v.value.u64); break;
        case REG_CAAR: c.CAAR = static_cast<uint32_t>(v.value.u64); break;
        case REG_SFC:  c.SFC  = static_cast<uint32_t>(v.value.u64); break;
        case REG_DFC:  c.DFC  = static_cast<uint32_t>(v.value.u64); break;
        // FPU
        case REG_FP0: case REG_FP1: case REG_FP2: case REG_FP3:
        case REG_FP4: case REG_FP5: case REG_FP6: case REG_FP7:
            c.GetFpu().FP[v.reg_id - REG_FP0] = v.value.f64;
            break;
        case REG_FPCR:  c.GetFpu().FPCR  = static_cast<uint32_t>(v.value.u64); break;
        case REG_FPSR:  c.GetFpu().FPSR  = static_cast<uint32_t>(v.value.u64); break;
        case REG_FPIAR: c.GetFpu().FPIAR = static_cast<uint32_t>(v.value.u64); break;
        // Read-only registers
        case REG_CYCLES: case REG_INSTRUCTIONS:
        case REG_TC: case REG_TT0: case REG_TT1:
        case REG_MMUSR: case REG_CRP: case REG_SRP:
            break; // Ignore writes to read-only or MMU registers
        default:
            return EMFE_ERR_INVALID;
        }
    }
    return EMFE_OK;
}

// ---------- Memory ----------

uint8_t EMFE_CALL emfe_peek_byte(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->memory->PeekByte(static_cast<uint32_t>(address));
}

uint16_t EMFE_CALL emfe_peek_word(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->memory->PeekWord(static_cast<uint32_t>(address));
}

uint32_t EMFE_CALL emfe_peek_long(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->memory->PeekLong(static_cast<uint32_t>(address));
}

EmfeResult EMFE_CALL emfe_poke_byte(EmfeInstance instance, uint64_t address, uint8_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->memory->PokeByte(static_cast<uint32_t>(address), value);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_poke_word(EmfeInstance instance, uint64_t address, uint16_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->memory->PokeWord(static_cast<uint32_t>(address), value);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_poke_long(EmfeInstance instance, uint64_t address, uint32_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->memory->PokeLong(static_cast<uint32_t>(address), value);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_peek_range(EmfeInstance instance, uint64_t address,
                                      uint8_t* out_data, uint32_t length) {
    if (!instance || !out_data) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    auto data = inst->memory->GetRange(static_cast<uint32_t>(address), static_cast<int>(length));
    std::memcpy(out_data, data.data(), data.size());
    return EMFE_OK;
}

uint64_t EMFE_CALL emfe_get_memory_size(EmfeInstance instance) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->memory->GetSize();
}

// ---------- Disassembly ----------

EmfeResult EMFE_CALL emfe_disassemble_one(EmfeInstance instance, uint64_t address,
                                           EmfeDisasmLine* out_line) {
    if (!instance || !out_line) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    auto line = inst->disasm->DisassembleOne(static_cast<uint32_t>(address));

    // Store strings (valid until next call)
    inst->disasmStorage.resize(1);
    inst->disasmStorage[0].rawBytes = line.RawBytes;
    inst->disasmStorage[0].mnemonic = line.Mnemonic;
    inst->disasmStorage[0].operands = line.Operands;

    out_line->address = line.Address;
    out_line->raw_bytes = inst->disasmStorage[0].rawBytes.c_str();
    out_line->mnemonic = inst->disasmStorage[0].mnemonic.c_str();
    out_line->operands = inst->disasmStorage[0].operands.c_str();
    out_line->length = static_cast<uint32_t>(line.Length);
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_disassemble_range(EmfeInstance instance, uint64_t start_address,
                                          uint64_t end_address, EmfeDisasmLine* out_lines,
                                          int32_t max_lines) {
    if (!instance || !out_lines || max_lines <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    auto lines = inst->disasm->DisassembleRange(
        static_cast<uint32_t>(start_address),
        static_cast<uint32_t>(end_address),
        max_lines);

    int32_t count = static_cast<int32_t>(std::min(lines.size(), static_cast<size_t>(max_lines)));
    inst->disasmStorage.resize(count);

    for (int32_t i = 0; i < count; i++) {
        inst->disasmStorage[i].rawBytes = lines[i].RawBytes;
        inst->disasmStorage[i].mnemonic = lines[i].Mnemonic;
        inst->disasmStorage[i].operands = lines[i].Operands;

        out_lines[i].address = lines[i].Address;
        out_lines[i].raw_bytes = inst->disasmStorage[i].rawBytes.c_str();
        out_lines[i].mnemonic = inst->disasmStorage[i].mnemonic.c_str();
        out_lines[i].operands = inst->disasmStorage[i].operands.c_str();
        out_lines[i].length = static_cast<uint32_t>(lines[i].Length);
    }
    return count;
}

// ---------- Execution ----------

EmfeResult EMFE_CALL emfe_step(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    inst->state.store(EMFE_STATE_STEPPING);
    inst->cpu->ExecuteStep();
    inst->state.store(inst->cpu->Halted ? EMFE_STATE_HALTED : EMFE_STATE_STOPPED);

    inst->NotifyStateChange(inst->state.load(), EMFE_STOP_REASON_STEP, inst->cpu->PC, nullptr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_step_over(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Disassemble current instruction to check if it's BSR/JSR
    auto line = inst->disasm->DisassembleOne(inst->cpu->PC);
    bool isCall = (line.Mnemonic == "BSR" || line.Mnemonic == "JSR" ||
                   line.Mnemonic.starts_with("BSR.") || line.Mnemonic.starts_with("JSR"));

    if (!isCall) {
        // Not a call - just step
        return emfe_step(instance);
    }

    // Wait for any pending previous worker thread to finish cleaning up
    // enabledBreakpoints before we read/modify it (data race fix).
    if (inst->emulationThread.joinable())
        inst->emulationThread.join();

    // Set temporary breakpoint at next instruction
    uint32_t nextPC = inst->cpu->PC + static_cast<uint32_t>(line.Length);
    bool hadBP = inst->enabledBreakpoints.contains(nextPC);
    if (!hadBP) {
        inst->enabledBreakpoints.insert(nextPC);
    }

    // Run
    inst->stopRequested.store(false);
    inst->state.store(EMFE_STATE_RUNNING);

    inst->emulationThread = std::thread([inst, nextPC, hadBP]() {
        inst->EmulationLoop();
        if (!hadBP)
            inst->enabledBreakpoints.erase(nextPC);
    });

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_step_out(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    auto& c = *inst->cpu;
    if (c._shadowStackTop > 0) {
        if (inst->emulationThread.joinable())
            inst->emulationThread.join();

        // Stop when shadow stack depth drops to (current - 1) — i.e. when the
        // current innermost frame has been popped by RTS. Frame-based tracking
        // works correctly with recursive calls (the BP-on-returnPC approach
        // mis-fires when nested calls hit the same code address).
        int32_t targetDepth = c._shadowStackTop - 1;
        inst->stepOutTargetDepth.store(targetDepth, std::memory_order_relaxed);
        inst->stopRequested.store(false);
        inst->state.store(EMFE_STATE_RUNNING);

        inst->emulationThread = std::thread([inst]() {
            inst->EmulationLoop();
            inst->stepOutTargetDepth.store(-1, std::memory_order_relaxed);
        });
    }

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_run(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Wait for previous thread to finish
    if (inst->emulationThread.joinable()) {
        inst->emulationThread.join();
    }

    // If PC is sitting on a breakpoint, execute one instruction first
    // to avoid immediately re-triggering the same breakpoint.
    auto& c = *inst->cpu;
    if (!inst->enabledBreakpoints.empty() && inst->enabledBreakpoints.contains(c.PC)) {
        c.ExecuteNextFast();
        if (c.Halted) {
            inst->NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_HALT, c.PC,
                c.StopReason.empty() ? "CPU halted" : c.StopReason.c_str());
            return EMFE_OK;
        }
    }

    inst->stopRequested.store(false);
    inst->state.store(EMFE_STATE_RUNNING);

    inst->emulationThread = std::thread([inst]() {
        inst->EmulationLoop();
    });

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_stop(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->stopRequested.store(true, std::memory_order_release);

    if (inst->emulationThread.joinable()) {
        inst->emulationThread.join();
    }

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_reset(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Flush any deferred REQUIRES_RESET settings: if the committed config
    // differs from the applied config on any device-affecting field, tear
    // down and rebuild the device tree so the new values take effect.
    auto& a = inst->appliedConfig;
    auto& c = inst->config;
    bool deviceChange =
        a.BoardType != c.BoardType ||
        a.MemorySize != c.MemorySize ||
        a.TargetOS != c.TargetOS ||
        a.NetworkMode != c.NetworkMode ||
        a.NatGatewayIp != c.NatGatewayIp ||
        a.TapAdapterGuid != c.TapAdapterGuid ||
        a.FramebufferEnabled != c.FramebufferEnabled ||
        a.FramebufferWidth != c.FramebufferWidth ||
        a.FramebufferHeight != c.FramebufferHeight ||
        a.FramebufferBpp != c.FramebufferBpp ||
        a.Mvme147RomPath != c.Mvme147RomPath ||
        a.Mvme147ScsiCdromPath != c.Mvme147ScsiCdromPath ||
        a.Mvme147ScsiCdromId != c.Mvme147ScsiCdromId ||
        a.Mvme147BootPartition != c.Mvme147BootPartition ||
        a.NetBsdKernelImagePath != c.NetBsdKernelImagePath ||
        a.LinuxKernelImagePath != c.LinuxKernelImagePath ||
        a.LinuxCommandLine != c.LinuxCommandLine ||
        a.Mvme147ScsiDisks.size() != c.Mvme147ScsiDisks.size();

    if (deviceChange) {
        inst->TeardownDevices();
        if (inst->config.BoardType == "MVME147")
            inst->SetupMvme147Devices();
        else
            inst->SetupGenericDevices();
        inst->SetupTrapHandler();
        inst->BuildRegisterDefs();
        inst->appliedConfig = inst->config;  // everything now current
    }

    if (inst->pccDevice) inst->pccDevice->HardwareReset();
    if (inst->scsiDevice) inst->scsiDevice->ResetBusState();
    inst->systemBooted = false;
    inst->cpu->Reset();
    inst->state.store(EMFE_STATE_STOPPED);
    inst->NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_NONE, inst->cpu->PC, "Reset");
    return EMFE_OK;
}

EmfeState EMFE_CALL emfe_get_state(EmfeInstance instance) {
    if (!instance) return EMFE_STATE_STOPPED;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->state.load();
}

int64_t EMFE_CALL emfe_get_instruction_count(EmfeInstance instance) {
    if (!instance) return 0;
    return reinterpret_cast<EmfeInstanceData*>(instance)->cpu->InstructionCount;
}

int64_t EMFE_CALL emfe_get_cycle_count(EmfeInstance instance) {
    if (!instance) return 0;
    return reinterpret_cast<EmfeInstanceData*>(instance)->cpu->CycleCount;
}

// ---------- Breakpoints ----------

EmfeResult EMFE_CALL emfe_add_breakpoint(EmfeInstance instance, uint64_t address) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);

    EmfeBreakpointInfo bp{};
    bp.address = address;
    bp.enabled = true;
    bp.condition = nullptr;
    inst->breakpoints[addr] = bp;
    inst->enabledBreakpoints.insert(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_remove_breakpoint(EmfeInstance instance, uint64_t address) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);

    inst->breakpoints.erase(addr);
    inst->enabledBreakpoints.erase(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_enable_breakpoint(EmfeInstance instance, uint64_t address, bool enabled) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);

    auto it = inst->breakpoints.find(addr);
    if (it == inst->breakpoints.end()) return EMFE_ERR_NOTFOUND;

    it->second.enabled = enabled;
    if (enabled)
        inst->enabledBreakpoints.insert(addr);
    else
        inst->enabledBreakpoints.erase(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_breakpoint_condition(EmfeInstance instance, uint64_t address,
                                                    const char* condition) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);

    auto it = inst->breakpoints.find(addr);
    if (it == inst->breakpoints.end()) return EMFE_ERR_NOTFOUND;

    // Store condition string
    if (condition) {
        inst->bpConditionStorage.push_back(condition);
        it->second.condition = inst->bpConditionStorage.back().c_str();
    } else {
        it->second.condition = nullptr;
    }
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_clear_breakpoints(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->breakpoints.clear();
    inst->enabledBreakpoints.clear();
    inst->bpConditionStorage.clear();
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_get_breakpoints(EmfeInstance instance, EmfeBreakpointInfo* out_breakpoints,
                                        int32_t max_count) {
    if (!instance || !out_breakpoints || max_count <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    int32_t i = 0;
    for (auto& [addr, bp] : inst->breakpoints) {
        if (i >= max_count) break;
        out_breakpoints[i] = bp;
        i++;
    }
    return i;
}

// ---------- Watchpoints (Phase 3) ----------

static void UpdateWatchpointsEnabledFlag(EmfeInstanceData* inst) {
    bool anyEnabled = false;
    for (auto& [addr, wp] : inst->watchpoints) {
        if (wp.enabled) { anyEnabled = true; break; }
    }
    if (inst->cpu) inst->cpu->WatchpointsEnabled = anyEnabled;
}

EmfeResult EMFE_CALL emfe_add_watchpoint(EmfeInstance instance, uint64_t address,
                                          EmfeWatchpointSize size, EmfeWatchpointType type) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    inst->watchpoints[addr] = { addr, static_cast<uint32_t>(size), type, true };
    UpdateWatchpointsEnabledFlag(inst);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_remove_watchpoint(EmfeInstance instance, uint64_t address) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    inst->watchpoints.erase(addr);
    UpdateWatchpointsEnabledFlag(inst);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_enable_watchpoint(EmfeInstance instance, uint64_t address, bool enabled) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    auto it = inst->watchpoints.find(addr);
    if (it == inst->watchpoints.end()) return EMFE_ERR_NOTFOUND;
    it->second.enabled = enabled;
    UpdateWatchpointsEnabledFlag(inst);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_watchpoint_condition(EmfeInstance instance, uint64_t address, const char* condition) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint32_t addr = static_cast<uint32_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    auto it = inst->watchpoints.find(addr);
    if (it == inst->watchpoints.end()) return EMFE_ERR_NOTFOUND;
    it->second.condition = condition ? condition : "";
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_clear_watchpoints(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    inst->watchpoints.clear();
    if (inst->cpu) inst->cpu->WatchpointsEnabled = false;
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_get_watchpoints(EmfeInstance instance, EmfeWatchpointInfo* out_watchpoints,
                                        int32_t max_count) {
    if (!instance || !out_watchpoints || max_count <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    int32_t i = 0;
    for (auto& [addr, wp] : inst->watchpoints) {
        if (i >= max_count) break;
        out_watchpoints[i].address = wp.address;
        out_watchpoints[i].size = static_cast<EmfeWatchpointSize>(wp.size);
        out_watchpoints[i].type = wp.type;
        out_watchpoints[i].enabled = wp.enabled;
        out_watchpoints[i].condition = wp.condition.empty() ? nullptr : wp.condition.c_str();
        i++;
    }
    return i;
}

// ---------- Call Stack (Phase 3) ----------

int32_t EMFE_CALL emfe_get_call_stack(EmfeInstance instance, EmfeCallStackEntry* out_entries,
                                       int32_t max_count) {
    if (!instance || !out_entries || max_count <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (!inst->cpu) return 0;

    auto& c = *inst->cpu;

    // A6Chain mode: walk A6 frame pointer chain + heuristic stack scan
    if (inst->config.CallStackMode == "A6Chain") {
        uint32_t memSize = static_cast<uint32_t>(inst->memory->GetFastRamSize());

        auto isCodeAddress = [&](uint32_t addr) -> bool {
            if (addr == 0 || addr >= memSize || (addr & 1) != 0) return false;
            if (inst->programStartAddress < inst->programEndAddress)
                return addr >= inst->programStartAddress && addr < inst->programEndAddress;
            return addr >= 0x1000;
        };

        int32_t count = 0;

        // Walk A6 frame pointer chain
        // LINK A6 creates: mem[A6] = saved A6, mem[A6+4] = return address
        uint32_t fp = c.A[6];
        for (int i = 0; i < max_count && fp != 0; i++) {
            if (fp + 4 >= memSize || fp < 0x1000 || (fp & 1) != 0)
                break;
            try {
                uint32_t retAddr = inst->memory->ReadLong(fp + 4);
                uint32_t savedFp = inst->memory->ReadLong(fp);
                if (!isCodeAddress(retAddr))
                    break;
                out_entries[count].call_pc = retAddr;
                out_entries[count].target_pc = 0;
                out_entries[count].return_pc = retAddr;
                out_entries[count].frame_pointer = fp;
                out_entries[count].kind = EMFE_CALL_KIND_CALL;
                out_entries[count].label = nullptr;
                count++;
                if (savedFp == 0 || savedFp == fp || savedFp <= fp)
                    break;
                fp = savedFp;
            } catch (...) { break; }
        }

        // Heuristic stack scan: find return-address-like values on the stack
        // that weren't found via A6 chain (catches -fomit-frame-pointer code, interrupts)
        std::unordered_set<uint32_t> knownAddrs;
        for (int i = 0; i < count; i++) knownAddrs.insert(static_cast<uint32_t>(out_entries[i].call_pc));

        uint32_t sp = c.A[7];
        constexpr uint32_t ScanBytes = 4096;
        for (uint32_t offset = 0; offset < ScanBytes && sp + offset + 3 < memSize && count < max_count; offset += 2) {
            try {
                uint32_t val = inst->memory->ReadLong(sp + offset);
                if (isCodeAddress(val) && knownAddrs.find(val) == knownAddrs.end()) {
                    out_entries[count].call_pc = val;
                    out_entries[count].target_pc = 0;
                    out_entries[count].return_pc = val;
                    out_entries[count].frame_pointer = sp + offset;
                    out_entries[count].kind = EMFE_CALL_KIND_CALL;
                    out_entries[count].label = "?";
                    knownAddrs.insert(val);
                    count++;
                }
            } catch (...) { break; }
        }
        return count;
    }

    // Default: ShadowStack mode
    int32_t count = std::min(c._shadowStackTop, max_count);
    for (int32_t i = 0; i < count; i++) {
        auto& e = c._shadowStack[count - 1 - i];
        out_entries[i].call_pc = e.callPC;
        out_entries[i].target_pc = e.targetPC;
        out_entries[i].return_pc = e.returnPC;
        out_entries[i].frame_pointer = e.sp;
        out_entries[i].kind = static_cast<EmfeCallStackKind>(e.kind);
        out_entries[i].label = nullptr;
    }
    return count;
}

// ---------- Framebuffer (Phase 3) ----------

EmfeResult EMFE_CALL emfe_get_framebuffer_info(EmfeInstance instance, EmfeFramebufferInfo* out_info) {
    if (!instance || !out_info) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (!inst->framebufferDevice) return EMFE_ERR_UNSUPPORTED;

    uint32_t w = static_cast<uint32_t>(inst->config.FramebufferWidth);
    uint32_t h = static_cast<uint32_t>(inst->config.FramebufferHeight);
    uint32_t bpp = static_cast<uint32_t>(inst->config.FramebufferBpp);
    uint32_t vramBase = inst->config.ComputeVramBase();

    out_info->width = w;
    out_info->height = h;
    out_info->bpp = bpp;
    out_info->stride = (w * bpp + 7) / 8;
    out_info->base_address = vramBase;
    out_info->flags = 0;

    // Direct pointer to VRAM (fast RAM pointer + offset)
    const uint8_t* fastRam = inst->memory->GetFastRamPointer();
    if (fastRam && vramBase < inst->memory->GetFastRamSize()) {
        out_info->pixels = fastRam + vramBase;
    } else {
        out_info->pixels = nullptr;
    }
    return EMFE_OK;
}

uint32_t EMFE_CALL emfe_get_palette_entry(EmfeInstance instance, uint32_t index) {
    (void)instance; (void)index;
    // MVME147 FramebufferDevice currently doesn't expose a palette.
    // Return grayscale for now (for INDEXED8 framebuffers this may be overridden later).
    uint8_t g = static_cast<uint8_t>(index & 0xFF);
    return 0xFF000000u | (g << 16) | (g << 8) | g;
}

int32_t EMFE_CALL emfe_get_palette(EmfeInstance instance, uint32_t* out_colors, int32_t max_count) {
    (void)instance;
    if (!out_colors || max_count <= 0) return 0;
    int32_t n = std::min(max_count, 256);
    for (int32_t i = 0; i < n; i++)
        out_colors[i] = 0xFF000000u | (i << 16) | (i << 8) | i;
    return n;
}

// ---------- Input events (Phase 3) ----------

EmfeResult EMFE_CALL emfe_push_key(EmfeInstance instance, uint32_t scancode, bool pressed) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->inputDevice)
        inst->inputDevice->PushKeyEvent(static_cast<uint16_t>(scancode), pressed ? 1 : 0);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_push_mouse_move(EmfeInstance instance, int32_t dx, int32_t dy) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->inputDevice)
        inst->inputDevice->PushMouseMoveEvent(static_cast<int16_t>(dx), static_cast<int16_t>(dy));
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_push_mouse_absolute(EmfeInstance instance, int32_t x, int32_t y) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->inputDevice)
        inst->inputDevice->PushMouseAbsEvent(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_push_mouse_button(EmfeInstance instance, int32_t button, bool pressed) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->inputDevice)
        inst->inputDevice->PushMouseButtonEvent(static_cast<uint16_t>(button), pressed ? 1 : 0);
    return EMFE_OK;
}

// ---------- File Loading ----------

EmfeResult EMFE_CALL emfe_load_elf(EmfeInstance instance, const char* file_path) {
    if (!instance || !file_path) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    try {
        auto result = Em68030::IO::FileLoader::LoadElf(*inst->memory, file_path);
        inst->cpu->PC = result.EntryPoint;
        inst->programStartAddress = result.StartAddress;
        inst->programEndAddress = result.EndAddress;
        inst->lastLoadedFile = file_path;
        inst->systemBooted = false;

        if (inst->config.BoardType == "MVME147") {
            uint32_t topOfRam = inst->config.FramebufferEnabled
                ? inst->config.ComputeVramBase()
                : static_cast<uint32_t>(inst->config.MemorySize);
            if (inst->config.TargetOS == "Linux")
                inst->SetupMvme147LinuxBootStub(topOfRam, result.EndAddress);
            else
                inst->SetupMvme147BootStub(topOfRam);
            inst->cpu->SR = 0x2700;
        } else {
            inst->cpu->SR = 0x2700;
            inst->cpu->A[7] = inst->memory->GetFastRamSize();
            inst->cpu->SSP = inst->cpu->A[7];
        }
        inst->lastError.clear();
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

const char* EMFE_CALL emfe_get_last_error(EmfeInstance instance) {
    if (!instance) return "Invalid instance";
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->lastError.c_str();
}

EmfeResult EMFE_CALL emfe_get_program_range(EmfeInstance instance, uint64_t* out_start, uint64_t* out_end) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (out_start) *out_start = inst->programStartAddress;
    if (out_end) *out_end = inst->programEndAddress;
    return EMFE_OK;
}

// ---------- Console I/O ----------

EmfeResult EMFE_CALL emfe_send_char(EmfeInstance instance, char ch) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->sccDevice)
        inst->sccDevice->GetChannelA().QueueInput(static_cast<uint8_t>(ch));
    if (inst->uartDevice)
        inst->uartDevice->ReceiveChar(static_cast<uint8_t>(ch));
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_send_string(EmfeInstance instance, const char* str) {
    if (!instance || !str) return EMFE_ERR_INVALID;
    while (*str) {
        emfe_send_char(instance, *str++);
    }
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_console_tx_space(EmfeInstance instance) {
    if (!instance) return -1;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    // Uart16550 has a strict 64-byte FIFO and silently drops past it.
    // Z8530's QueueInput is unbounded, so it doesn't drop.  Report the
    // tightest constraint so the host waits for whichever UART is
    // currently in play.
    int32_t space = INT32_MAX;
    if (inst->uartDevice) {
        size_t free_bytes = inst->uartDevice->GetRxFifoFreeSpace();
        int32_t uart_space = free_bytes > static_cast<size_t>(INT32_MAX)
                                 ? INT32_MAX
                                 : static_cast<int32_t>(free_bytes);
        if (uart_space < space) space = uart_space;
    }
    // sccDevice has no cap; don't narrow `space` for it.

    // If neither console device is active, we can't meaningfully report
    // space — pretend "unbounded" so the host doesn't throttle pointlessly.
    if (!inst->uartDevice && !inst->sccDevice) return -1;
    return space;
}

// ---------- File Loading (Phase 2 additions) ----------

EmfeResult EMFE_CALL emfe_load_binary(EmfeInstance instance, const char* file_path, uint64_t load_address) {
    if (!instance || !file_path) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    try {
        Em68030::IO::FileLoader::LoadBinary(*inst->memory, file_path, static_cast<uint32_t>(load_address));
        inst->cpu->PC = static_cast<uint32_t>(load_address);
        inst->cpu->SR = 0x2700;
        inst->cpu->A[7] = inst->memory->GetFastRamSize();
        inst->cpu->SSP = inst->cpu->A[7];
        inst->lastError.clear();
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

EmfeResult EMFE_CALL emfe_load_srec(EmfeInstance instance, const char* file_path) {
    if (!instance || !file_path) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    try {
        auto result = Em68030::IO::FileLoader::LoadSRecord(*inst->memory, file_path);
        if (result.HasEntryPoint)
            inst->cpu->PC = result.EntryPoint;
        inst->cpu->SR = 0x2700;
        inst->cpu->A[7] = inst->memory->GetFastRamSize();
        inst->cpu->SSP = inst->cpu->A[7];
        inst->lastError.clear();
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

// ---------- Settings ----------

// Helper to get/set config fields by key
static std::string GetConfigValue(const Em68030::Config::EmulatorConfig& cfg, const std::string& key) {
    if (key == "MemorySize")             return std::to_string(cfg.MemorySize / (1024*1024));
    if (key == "BoardType")              return cfg.BoardType;
    if (key == "TargetOS")               return cfg.TargetOS;
    if (key == "Theme")                  return cfg.Theme;
    if (key == "Mvme147RomPath")         return cfg.Mvme147RomPath;
    if (key == "Mvme147ScsiCdromPath")   return cfg.Mvme147ScsiCdromPath;
    if (key == "Mvme147ScsiCdromId")     return std::to_string(cfg.Mvme147ScsiCdromId);
    if (key == "Mvme147BootPartition") {
        char c = static_cast<char>('a' + std::clamp(cfg.Mvme147BootPartition, 0, 7));
        return std::string(1, c);
    }
    if (key == "NetBsdKernelImagePath")  return cfg.NetBsdKernelImagePath;
    if (key == "LinuxKernelImagePath")   return cfg.LinuxKernelImagePath;
    if (key == "LinuxCommandLine")       return cfg.LinuxCommandLine;
    if (key == "NetworkMode")            return cfg.NetworkMode;
    if (key == "NatGatewayIp")           return cfg.NatGatewayIp;
    if (key == "TapAdapterGuid")         return cfg.TapAdapterGuid;
    if (key == "ConsoleScrollbackLines") return std::to_string(cfg.ConsoleScrollbackLines);
    if (key == "ConsoleColumns")         return std::to_string(cfg.ConsoleColumns);
    if (key == "ConsoleRows")            return std::to_string(cfg.ConsoleRows);
    if (key == "JitEnabled")             return cfg.JitEnabled ? "true" : "false";
    if (key == "JitMinBlockLength")      return std::to_string(cfg.JitMinBlockLength);
    if (key == "JitCompileThreshold")    return std::to_string(cfg.JitCompileThreshold);
    if (key == "FramebufferEnabled")     return cfg.FramebufferEnabled ? "true" : "false";
    if (key == "FramebufferWidth")       return std::to_string(cfg.FramebufferWidth);
    if (key == "FramebufferHeight")      return std::to_string(cfg.FramebufferHeight);
    if (key == "FramebufferBpp")         return std::to_string(cfg.FramebufferBpp);
    if (key == "CallStackMode")          return cfg.CallStackMode;
    return "";
}

static void SetConfigValue(Em68030::Config::EmulatorConfig& cfg, const std::string& key, const std::string& val) {
    try {
        if (key == "MemorySize")             cfg.MemorySize = std::stoi(val) * 1024 * 1024;
        else if (key == "BoardType")         cfg.BoardType = val;
        else if (key == "TargetOS")          cfg.TargetOS = val;
        else if (key == "Theme")             cfg.Theme = val;
        else if (key == "Mvme147RomPath")    cfg.Mvme147RomPath = val;
        else if (key == "Mvme147ScsiCdromPath") cfg.Mvme147ScsiCdromPath = val;
        else if (key == "Mvme147ScsiCdromId")   cfg.Mvme147ScsiCdromId = std::stoi(val);
        else if (key == "Mvme147BootPartition") {
            if (!val.empty() && val[0] >= 'a' && val[0] <= 'h')
                cfg.Mvme147BootPartition = val[0] - 'a';
            else
                cfg.Mvme147BootPartition = std::stoi(val);
        }
        else if (key == "NetBsdKernelImagePath") cfg.NetBsdKernelImagePath = val;
        else if (key == "LinuxKernelImagePath")  cfg.LinuxKernelImagePath = val;
        else if (key == "LinuxCommandLine")      cfg.LinuxCommandLine = val;
        else if (key == "NetworkMode")       cfg.NetworkMode = val;
        else if (key == "NatGatewayIp")      cfg.NatGatewayIp = val;
        else if (key == "TapAdapterGuid")    cfg.TapAdapterGuid = val;
        else if (key == "ConsoleScrollbackLines") cfg.ConsoleScrollbackLines = std::stoi(val);
        else if (key == "ConsoleColumns")    cfg.ConsoleColumns = std::stoi(val);
        else if (key == "ConsoleRows")       cfg.ConsoleRows = std::stoi(val);
        else if (key == "JitEnabled")        cfg.JitEnabled = (val == "true" || val == "1");
        else if (key == "JitMinBlockLength") cfg.JitMinBlockLength = std::stoi(val);
        else if (key == "JitCompileThreshold") cfg.JitCompileThreshold = std::stoi(val);
        else if (key == "FramebufferEnabled") cfg.FramebufferEnabled = (val == "true" || val == "1");
        else if (key == "FramebufferWidth")  cfg.FramebufferWidth = std::stoi(val);
        else if (key == "FramebufferHeight") cfg.FramebufferHeight = std::stoi(val);
        else if (key == "FramebufferBpp")    cfg.FramebufferBpp = std::stoi(val);
        else if (key == "CallStackMode")     cfg.CallStackMode = val;
    } catch (...) {}
}

int32_t EMFE_CALL emfe_get_setting_defs(EmfeInstance instance, const EmfeSettingDef** out_defs) {
    if (!instance || !out_defs) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    *out_defs = inst->settingDefs.data();
    return static_cast<int32_t>(inst->settingDefs.size());
}

const char* EMFE_CALL emfe_get_setting(EmfeInstance instance, const char* key) {
    if (!instance || !key) return "";
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->settingValueBuf = GetConfigValue(inst->stagedConfig, key);
    return inst->settingValueBuf.c_str();
}

const char* EMFE_CALL emfe_get_applied_setting(EmfeInstance instance, const char* key) {
    if (!instance || !key) return "";
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->appliedSettingValueBuf = GetConfigValue(inst->appliedConfig, key);
    return inst->appliedSettingValueBuf.c_str();
}

EmfeResult EMFE_CALL emfe_set_setting(EmfeInstance instance, const char* key, const char* value) {
    if (!instance || !key || !value) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    SetConfigValue(inst->stagedConfig, key, value);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_apply_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Commit staged → config. This is what emfe_get_setting reports.
    inst->config = inst->stagedConfig;

    // Hot-swappable settings: update running hardware now and sync
    // appliedConfig's corresponding fields.
    inst->cpu->JitEnabled = inst->config.JitEnabled;
    inst->cpu->JitMinBlockLength = inst->config.JitMinBlockLength;
    inst->cpu->JitCompileThreshold = static_cast<uint8_t>(inst->config.JitCompileThreshold);
    if (!inst->config.JitEnabled)
        inst->cpu->InvalidateJitCache();

    inst->appliedConfig.Theme = inst->config.Theme;
    inst->appliedConfig.ConsoleScrollbackLines = inst->config.ConsoleScrollbackLines;
    inst->appliedConfig.ConsoleColumns = inst->config.ConsoleColumns;
    inst->appliedConfig.ConsoleRows = inst->config.ConsoleRows;
    inst->appliedConfig.JitEnabled = inst->config.JitEnabled;
    inst->appliedConfig.JitMinBlockLength = inst->config.JitMinBlockLength;
    inst->appliedConfig.JitCompileThreshold = inst->config.JitCompileThreshold;
    inst->appliedConfig.CallStackMode = inst->config.CallStackMode;

    // Device-affecting fields are deferred: leave appliedConfig alone so
    // the UI can show them as pending. The running hardware keeps its
    // current state until emfe_reset flushes the deferred values.
    //
    // See the corresponding EMFE_SETTING_FLAG_REQUIRES_RESET entries in
    // BuildSettingDefs() for the precise list; this code mirrors that set
    // by simply omitting those fields above.
    return EMFE_OK;
}

// --- LIST type: SCSI disk list ---

int32_t EMFE_CALL emfe_get_list_item_defs(EmfeInstance instance, const char* list_key,
                                           const EmfeListItemDef** out_defs) {
    if (!instance || !list_key || !out_defs) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (std::string(list_key) == "Mvme147ScsiDisks") {
        *out_defs = inst->scsiListItemDefs.data();
        return static_cast<int32_t>(inst->scsiListItemDefs.size());
    }
    return 0;
}

int32_t EMFE_CALL emfe_get_list_item_count(EmfeInstance instance, const char* list_key) {
    if (!instance || !list_key) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (std::string(list_key) == "Mvme147ScsiDisks")
        return static_cast<int32_t>(inst->stagedConfig.Mvme147ScsiDisks.size());
    return 0;
}

const char* EMFE_CALL emfe_get_list_item_field(EmfeInstance instance, const char* list_key,
                                                int32_t item_index, const char* field_key) {
    if (!instance || !list_key || !field_key) return "";
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (std::string(list_key) == "Mvme147ScsiDisks") {
        auto& disks = inst->stagedConfig.Mvme147ScsiDisks;
        if (item_index < 0 || item_index >= static_cast<int32_t>(disks.size())) return "";
        std::string fk(field_key);
        if (fk == "Path") { inst->settingValueBuf = disks[item_index].Path; return inst->settingValueBuf.c_str(); }
        if (fk == "ScsiId") { inst->settingValueBuf = std::to_string(disks[item_index].ScsiId); return inst->settingValueBuf.c_str(); }
    }
    return "";
}

EmfeResult EMFE_CALL emfe_set_list_item_field(EmfeInstance instance, const char* list_key,
                                               int32_t item_index, const char* field_key, const char* value) {
    if (!instance || !list_key || !field_key || !value) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (std::string(list_key) == "Mvme147ScsiDisks") {
        auto& disks = inst->stagedConfig.Mvme147ScsiDisks;
        if (item_index < 0 || item_index >= static_cast<int32_t>(disks.size())) return EMFE_ERR_INVALID;
        std::string fk(field_key);
        if (fk == "Path") disks[item_index].Path = value;
        else if (fk == "ScsiId") try { disks[item_index].ScsiId = std::stoi(value); } catch (...) {}
        return EMFE_OK;
    }
    return EMFE_ERR_NOTFOUND;
}

int32_t EMFE_CALL emfe_add_list_item(EmfeInstance instance, const char* list_key) {
    if (!instance || !list_key) return -1;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (std::string(list_key) == "Mvme147ScsiDisks") {
        auto& disks = inst->stagedConfig.Mvme147ScsiDisks;
        disks.push_back({});
        return static_cast<int32_t>(disks.size() - 1);
    }
    return -1;
}

EmfeResult EMFE_CALL emfe_remove_list_item(EmfeInstance instance, const char* list_key, int32_t item_index) {
    if (!instance || !list_key) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (std::string(list_key) == "Mvme147ScsiDisks") {
        auto& disks = inst->stagedConfig.Mvme147ScsiDisks;
        if (item_index < 0 || item_index >= static_cast<int32_t>(disks.size())) return EMFE_ERR_INVALID;
        disks.erase(disks.begin() + item_index);
        return EMFE_OK;
    }
    return EMFE_ERR_NOTFOUND;
}

// Plugin-managed data directory (overrides default path).
// Stored as std::string to avoid any std::filesystem::path static initialization
// issues when the plugin DLL is loaded into a managed (.NET) process.
static std::string& GetDataDirString() {
    static std::string dataDir;
    return dataDir;
}

static std::filesystem::path GetSettingsPath() {
    auto& dir = GetDataDirString();
    if (dir.empty()) {
        // Default to %LOCALAPPDATA%\emfe_plugin_m68030
        wchar_t* appData = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
            std::filesystem::path defaultDir = std::filesystem::path(appData) / L"emfe_plugin_m68030";
            ::CoTaskMemFree(appData);
            dir = defaultDir.string();
        }
    }
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    return std::filesystem::path(dir) / "appsettings.json";
}

EmfeResult EMFE_CALL emfe_set_data_dir(const char* path) {
    if (!path) return EMFE_ERR_INVALID;
    try {
        GetDataDirString() = path;
        return EMFE_OK;
    } catch (...) {
        return EMFE_ERR_MEMORY;
    }
}

EmfeResult EMFE_CALL emfe_save_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    try {
        auto path = GetSettingsPath();
        nlohmann::json j = inst->config;
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            inst->lastError = "Failed to open settings file for writing";
            return EMFE_ERR_IO;
        }
        ofs << j.dump(2);
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

EmfeResult EMFE_CALL emfe_load_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    try {
        auto path = GetSettingsPath();
        if (std::filesystem::exists(path)) {
            std::ifstream ifs(path);
            if (ifs.is_open()) {
                nlohmann::json j = nlohmann::json::parse(ifs);
                inst->config = j.get<Em68030::Config::EmulatorConfig>();
            }
        }
        inst->stagedConfig = inst->config;
        inst->appliedConfig = inst->config;
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

// ---------- String Utilities ----------

void EMFE_CALL emfe_release_string(const char* str) {
    (void)str;
}

// ============================================================================
// DLL entry point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule;
    (void)lpReserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
