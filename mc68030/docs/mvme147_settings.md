# MVME147 Settings Behavior

This document describes how the **MVME147** tab in the emulator's
**Settings** dialog applies changes to the running emulator. The
behavior is identical between the plugin frontends (`emfe_WinUI3Cpp`,
`emfe_CsWPF`) and the standalone applications (`em68030_WinUI3Cpp`,
`em68030_CsWPF`), because the settings layer is shared (the MC68030
core's `EmulatorConfig` plus the `mc68030` plugin's apply logic).

Japanese: [mvme147_settings_ja.md](mvme147_settings_ja.md)

---

## 1. Per-target-OS SCSI bus configuration

The MVME147 board can boot either NetBSD or Linux. The two operating
systems generally expect different SCSI bus configurations — different
disk images, different SCSI IDs for `sd0`, different CD-ROM ISOs.

To support that without forcing the user to manually re-edit the SCSI
list every time they switch OS, the following fields are stored
**per-Target-OS**:

- **SCSI Disks** (the dynamic list with Path / SCSI ID per row)
- **SCSI CD-ROM Path**
- **SCSI CD-ROM ID**

Switching the **Target OS** combo box (in the Settings dialog) swaps
the visible SCSI Disks list, CD-ROM Path, and CD-ROM ID to the values
remembered for the newly selected OS. The values you previously edited
under the other OS are not lost — they're stored in their own slot in
the per-OS map and reappear when you switch back.

Other MVME147 fields (ROM Path, NetBSD/Linux Kernel Path, Linux
Command Line, etc.) are *not* per-OS — they're either OS-specific by
naming (`NetBsdKernelImagePath`) or shared across both OSes.

### Migration from older configs

Configurations saved by older builds had a single SCSI Disks list /
CD-ROM Path / CD-ROM ID shared across both OSes. When you open such a
config with a current build, those values are automatically copied
into both the NetBSD slot and the Linux slot — so the existing devices
remain visible regardless of which OS you select. The two slots only
diverge once you actually edit them.

The legacy single-value JSON keys (`Mvme147ScsiDisks`,
`Mvme147ScsiCdromPath`, `Mvme147ScsiCdromId`) are still written
alongside the new `Mvme147ScsiDisksByTargetOS` /
`Mvme147ScsiCdromPathByTargetOS` / `Mvme147ScsiCdromIdByTargetOS`
maps, so going backwards (opening the new config in an older build) is
also safe — the older build sees the active OS's values via the legacy
keys.

---

## 2. When does pressing **OK** actually apply changes?

The Settings dialog's **OK** button has three different behaviors
depending on what changed and whether emulation has been started yet.
This is intentional: while the CPU and devices are alive, ripping them
down to apply a structural change would silently destroy in-flight
state. The dialog therefore distinguishes three setting classes:

### a. Hot-swappable settings — applied immediately, always

CPU runs uninterrupted. Examples:
- JIT enabled / threshold
- Theme (Dark / Light / System)
- Console scrollback buffer / column / row size

These take effect the instant you press OK and never require a Reset.

### b. Removable-media device (SCSI CD-ROM) — applied immediately, always

The SCSI CD-ROM is a **removable-media** device per the SCSI standard:
its INQUIRY response reports `RMB=1` (Removable Media Bit, byte 1
bit 7 = 0x80), and the device returns `CHECK CONDITION` with
`UNIT ATTENTION` (sense key `0x06`, ASC `0x28` — *MEDIUM MAY HAVE
CHANGED*) on the first SCSI command after media is swapped, so a
properly written guest OS rediscovers the disc.

Currently the following two CD-ROM fields apply immediately on OK
(whether emulation has started or not):
- **SCSI CD-ROM Path** (the ISO image) — handled by
  `ScsiCdrom::UnmountImage` + `MountImage`, which sets the
  `m_mediaChanged` flag so the next CDB returns the UNIT ATTENTION.
- **SCSI CD-ROM ID** (target ID on the SCSI bus) — handled by
  `WD33C93Device::DetachTarget(oldId)` +
  `AttachTarget(newId, scsiCdrom)`, repositioning the CD-ROM on the
  live SCSI bus without disturbing other targets.

> **Note (HDDs are NOT hot-swappable):** SCSI Disks (`ScsiDisk`)
> represent fixed media (`RMB=0`), and live hot-swap of a HDD requires
> guest-OS support that NetBSD/mvme68k and Linux/m68k do not provide
> in their default configurations. SCSI Disks therefore go through
> the **deferred** path (§2c below), not this one — even though
> `ScsiDisk` itself has `MountImage`/`UnmountImage` methods, exposing
> them as live operations would silently corrupt the guest OS's
> filesystem state. If you want to change SCSI Disks, you must Reset
> after pressing OK.

### c. Deferred device-affecting settings

The remaining MVME147 fields are deferred when emulation is in
progress, and applied immediately when emulation has not yet started:

- **SCSI Disks** (whole list — path + SCSI ID per row)
- **Target OS**, **ROM Path**
- **NetBSD/Linux Kernel Path**, **Linux Command Line**, **Boot Partition**
- **Network Mode**, **NAT Gateway IP/MAC**, **TAP Adapter**
- **Memory Size**, **Framebuffer Enabled / Width / Height / BPP**

How they apply depends on the current state:

| State | Behavior on OK |
|---|---|
| Emulation has not yet started since last Reset | The device tree is torn down and rebuilt with the new values, the kernel ELF is automatically reloaded into RAM (if one was previously loaded), and the CPU is reset. Then **Run** boots with the new configuration. |
| Emulation is running (Stop is required first — OK is disabled while running) | — |
| Emulation has been started, then stopped (paused mid-session) | The change is **deferred**: stored on the staged config but not applied to the live device tree. The dialog shows a `*` pending marker (see §3). The running CPU and devices keep their state. Press **Reset** or **Full Reset** to flush the deferred changes. |

Why deferred during a session? An MVME147 board's device tree (memory
layout, mounted SCSI disks, framebuffer geometry, network interface)
is set up at power-on; tearing it down mid-execution would silently
destroy the OS's state — open files, kernel page tables, network
sockets, etc.

---

## 3. Pending markers

When a setting is deferred (case 2c above), the dialog shows an
orange asterisk `*` to indicate "this change is staged but the running
hardware hasn't seen it yet." Hovering the marker shows a tooltip
explaining that the change will take effect on the next Reset / Full
Reset (or on next emulator restart).

- For plain settings (string / combo / number), the `*` appears to
  the left of the setting's editor.
- For **SCSI Disks** (a list setting), the `*` appears beside the
  "SCSI Disks" section header.

The dialog rebuilds itself on every combo-box change (e.g. swapping
Target OS), so the pending markers stay in sync with whatever the
user has just edited.

---

## 4. Common workflows

### Boot a fresh kernel with a different disk

1. Open Settings → MVME147 tab.
2. Edit SCSI Disks (add / remove / change paths or IDs).
3. Press OK.
4. Press **Run (F5)**.

Result: device tree rebuilt + kernel ELF reloaded + boot from new
disks. (Works only if you haven't pressed Run since opening emfe — see
§2c.)

### Swap a CD-ROM during a NetBSD session

1. Press Stop (or even leave NetBSD running and accept that OK is
   disabled mid-Run, then Stop afterwards).
2. Open Settings → MVME147 tab → change SCSI CD-ROM Path.
3. Press OK.
4. Press Run.

Result: the new ISO is live on the bus immediately. Inside NetBSD,
the next access to the CD device sees a media-change UNIT ATTENTION
and you can `mount /dev/cd0a /mnt` against the new image.

### Edit SCSI Disks during a session

1. Run, then Stop (paused mid-session).
2. Open Settings → MVME147 tab → edit SCSI Disks.
3. Press OK.

Result: the change is **deferred** (you'll see the orange `*`
on the SCSI Disks section header). The currently-running NetBSD/Linux
keeps its existing devices.

To actually apply: press **Reset** or **Full Reset**. The device tree
is torn down with the new SCSI Disks configuration, the kernel ELF is
reloaded, and the CPU starts from the kernel entry point.

### Switch from NetBSD to Linux (or vice versa)

1. Open Settings → MVME147 tab → change Target OS combo.
2. The SCSI Disks list, SCSI CD-ROM Path, and SCSI CD-ROM ID
   immediately swap to the values you have remembered for the other
   OS (or empty/defaults the first time).
3. (Optional) edit any of the per-OS fields.
4. Press OK.

If emulation hasn't started yet → device tree is rebuilt with the new
OS's configuration immediately. If emulation has started → deferred
until Reset.
