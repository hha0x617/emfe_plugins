# -----------------------------------------------------------------------------
# build.ps1 — assemble the custom CBIOS and combine with CCP / BDOS into
#             a bootable SSSD-256K CP/M 2.2 disk image.
#
# Phase 4.3 deliverable per docs/phase4_design.md §4.
#
# Inputs:
#   * cbios/cbios.asm        (this repo, MIT) — origin 0xFA00, raw .bin out
#   * <companion>/cpm22/binary/ccp.bin   (companion repo, CP/M licensed)
#   * <companion>/cpm22/binary/bdos.bin  (companion repo, CP/M licensed)
#
# Output (CP/M-licensed because it contains CCP + BDOS bytes):
#   * <companion>/disks/cpm22-sssd.img
#
# Single-direction file flow — this repo NEVER reads CCP / BDOS files into
# its own working tree; assembled output lands in the companion repo so
# the license posture is unambiguous.
#
# Usage:
#   # default sibling layout (D:\projects\emfe_plugins  +  D:\projects\emfe_plugin_z80_cpm22)
#   pwsh ./build.ps1
#   # explicit companion path
#   pwsh ./build.ps1 -CompanionPath D:\path\to\emfe_plugin_z80_cpm22
#   # explicit pasmo path (otherwise looked up in PATH)
#   pwsh ./build.ps1 -PasmoExe C:\tools\pasmo.exe
# -----------------------------------------------------------------------------

[CmdletBinding()]
param(
    [string]$CompanionPath,
    [string]$PasmoExe,
    [switch]$SkipImage   # assemble cbios.bin only, don't try to build the disk
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = $PSScriptRoot
$CbiosSrc   = Join-Path $ScriptRoot 'cbios/cbios.asm'
$CbiosBin   = Join-Path $ScriptRoot 'cbios/cbios.bin'

# -----------------------------------------------------------------------------
# Resolve pasmo. Phase 4.3 picks pasmo per the design-doc lean (§7.1); the
# .asm source uses only standard mnemonics so other assemblers should work
# if a user prefers them — they'd substitute the call below themselves.
# -----------------------------------------------------------------------------
if (-not $PasmoExe) {
    $cmd = Get-Command pasmo -ErrorAction SilentlyContinue
    if ($cmd) { $PasmoExe = $cmd.Source }
}
if (-not $PasmoExe -or -not (Test-Path $PasmoExe)) {
    Write-Error @"
pasmo not found on PATH and no -PasmoExe given.

Install pasmo from https://pasmo.speccy.org/ (single-binary Z80 assembler)
or pass -PasmoExe <path> on the command line. The source compiles cleanly
with sjasmplus / z80asm / tniASM too if you'd rather substitute another
assembler — the only requirement is "produces a flat .bin from cbios.asm".
"@
    exit 1
}
Write-Host "pasmo: $PasmoExe"

# -----------------------------------------------------------------------------
# Assemble cbios.asm. pasmo's default output is Intel HEX; we want a raw
# binary so the combine step can splat it straight into the disk image.
# -----------------------------------------------------------------------------
if (-not (Test-Path $CbiosSrc)) {
    Write-Error "Source not found: $CbiosSrc"
    exit 1
}
Write-Host "Assembling $CbiosSrc -> $CbiosBin"
& $PasmoExe --bin $CbiosSrc $CbiosBin
if ($LASTEXITCODE -ne 0) {
    Write-Error "pasmo failed with exit code $LASTEXITCODE"
    exit 1
}
$cbiosSize = (Get-Item $CbiosBin).Length
Write-Host ("CBIOS assembled: {0} bytes" -f $cbiosSize)

if ($SkipImage) {
    Write-Host "Skipping disk-image build (per -SkipImage)."
    exit 0
}

# -----------------------------------------------------------------------------
# Resolve companion repo. Default: sibling directory next to emfe_plugins.
# -----------------------------------------------------------------------------
if (-not $CompanionPath) {
    # PSScriptRoot is .../emfe_plugins/z80/examples/cpm22 — walk up to the
    # parent of emfe_plugins, then check for the canonical sibling name.
    $emfePluginsRoot = (Resolve-Path (Join-Path $ScriptRoot '../../..')).Path
    $parent = Split-Path $emfePluginsRoot -Parent
    $CompanionPath  = Join-Path $parent 'emfe_plugin_z80_cpm22'
}
$CompanionPath = (Resolve-Path -LiteralPath $CompanionPath -ErrorAction SilentlyContinue) ?
    (Resolve-Path -LiteralPath $CompanionPath).Path :
    $CompanionPath

$Cpm22Src   = Join-Path $CompanionPath 'cpm22/source/CPM22.Z80'
$DisksDir   = Join-Path $CompanionPath 'disks'
$ImageOut   = Join-Path $DisksDir 'cpm22-sssd.img'

# Skip-with-message path (mirrors the zexdoc / eForth pattern): if the
# companion repo isn't populated, document what's needed and exit 0 so a
# casual `pwsh ./build.ps1` doesn't look like a build failure.
if (-not (Test-Path $Cpm22Src)) {
    Write-Host ""
    Write-Host "(SKIP - companion CPM22.Z80 source not found)"
    Write-Host ""
    Write-Host "  Expected:  $Cpm22Src"
    Write-Host ""
    Write-Host "  Populate the companion repo with CPM22.Z80 from cpm2-asm.zip"
    Write-Host "  on 'The Unofficial CP/M Web Site' — see"
    Write-Host "    emfe_plugin_z80_cpm22/cpm22/source/README.md"
    Write-Host "  and"
    Write-Host "    emfe_plugin_z80_cpm22/docs/provenance.md"
    Write-Host "  for the canonical source and SHA-256. Then re-run this script."
    exit 0
}

# -----------------------------------------------------------------------------
# Assemble CPM22.Z80 with MEM EQU 64 substitution.
#
# The upstream source defaults to MEM EQU 62 (62-KiB system, ORG 0xDC00).
# Our plugin's memory map (phase4_design.md §3.1) puts CCP at 0xE400, which
# corresponds to MEM = 64. We substitute the value into a temporary copy so
# the on-disk source under the companion stays verbatim (matching the
# documented SHA-256 in provenance.md).
# -----------------------------------------------------------------------------

$Cpm22Tmp = Join-Path $ScriptRoot 'cbios/cpm22-mem64.z80'
$Cpm22Bin = Join-Path $ScriptRoot 'cbios/cpm22-system.bin'

$srcText = Get-Content $Cpm22Src -Raw
$patched = $srcText -replace '(?m)^(\s*MEM\s+EQU\s+)\d+', '${1}64'
if ($patched -eq $srcText) {
    Write-Error "Could not find 'MEM EQU N' line in $Cpm22Src to patch — upstream layout may have changed."
    exit 1
}
Set-Content -LiteralPath $Cpm22Tmp -Value $patched -NoNewline

Write-Host "Assembling $Cpm22Src (MEM=64) -> $Cpm22Bin"
& $PasmoExe --bin $Cpm22Tmp $Cpm22Bin
if ($LASTEXITCODE -ne 0) {
    Write-Error "pasmo failed on CPM22.Z80 with exit code $LASTEXITCODE"
    exit 1
}
$cpm22Size = (Get-Item $Cpm22Bin).Length
Write-Host ("CCP+BDOS assembled: {0} bytes" -f $cpm22Size)

# -----------------------------------------------------------------------------
# Build the disk image.
#
# SSSD-256K layout (77 tracks * 26 sectors * 128 B = 256 256 bytes):
#   LBA 0           boot stub at 0x0000 (128 bytes — JP into WBOOT entry)
#   LBA 1..N        CCP + BDOS + CBIOS bytes in order, 128 B per sector
#   LBA N+1..end    BDOS-formatted blank space (all 0xE5 = "empty dir entry")
#
# WBOOT (see cbios.asm) reads 44 sectors starting at LBA 1 directly into
# RAM at 0xE400, which spans CCP (0xE400) + BDOS (0xEC00) but NOT CBIOS
# (which sits above at 0xFA00 — already in RAM from the cold-boot loader
# that placed the full system on first run).
#
# The cold boot loader for the very first boot is the boot stub at LBA 0,
# whose job is "place the system image in RAM and JP CBIOS BOOT". For the
# Phase 4 skeleton we cheat: the plugin loads the entire disk image into
# RAM 0..N-1 directly (host-side), then sets PC=0x0000 and the boot stub
# is reached. The stub then jumps into CBIOS BOOT at 0xFA00.
# -----------------------------------------------------------------------------

$SectorSize  = 128
$ImgSize     = 77 * 26 * $SectorSize  # = 256 256 bytes
$image       = New-Object byte[] $ImgSize
# Fill with CP/M "empty directory entry" marker so a freshly-formatted
# image looks like a blank filesystem to BDOS.
for ($i = 0; $i -lt $ImgSize; $i++) { $image[$i] = 0xE5 }

# -- Boot stub at LBA 0 ---------------------------------------------------
#    JP 0FA00H              ; jump straight into CBIOS BOOT
$image[0] = 0xC3
$image[1] = 0x00
$image[2] = 0xFA
# (remaining 125 bytes of sector 0 stay 0xE5; the JP is all we need)

# -- CCP + BDOS at LBA 1 onwards ------------------------------------------
# The CPM22.Z80 source assembles to a single flat blob starting at ORG
# 0xE400 (because we patched MEM EQU 64). Total size with both CCP +
# BDOS is around 5632 bytes; cap at 44 sectors so it never overlaps
# the CBIOS region the cold-boot loader will lay down at LBA 45+.
$payload = [System.IO.File]::ReadAllBytes($Cpm22Bin)
if ($payload.Length -gt ($SectorSize * 44)) {
    Write-Error ("CCP+BDOS payload is {0} bytes (> 44 sectors = {1}). " +
                 "Source layout may have changed — check CPM22.Z80 ORG vs " +
                 "WBOOT sector count in cbios.asm." -f
                 $payload.Length, ($SectorSize * 44))
    exit 1
}
[Array]::Copy($payload, 0, $image, $SectorSize, $payload.Length)

# -- CBIOS at LBA after CCP+BDOS (NOT auto-loaded by WBOOT — see cbios.asm
#    header comment for why) ----------------------------------------------
$cbiosBytes = [System.IO.File]::ReadAllBytes($CbiosBin)
$cbiosLba   = 45    # immediately after the 44 CCP+BDOS sectors
$cbiosOff   = $cbiosLba * $SectorSize
if ($cbiosOff + $cbiosBytes.Length -gt $ImgSize) {
    Write-Error "CBIOS overruns disk image — image size needs to grow."
    exit 1
}
[Array]::Copy($cbiosBytes, 0, $image, $cbiosOff, $cbiosBytes.Length)

# -- Write the image ------------------------------------------------------
if (-not (Test-Path $DisksDir)) {
    New-Item -ItemType Directory -Path $DisksDir | Out-Null
}
[System.IO.File]::WriteAllBytes($ImageOut, $image)

Write-Host ""
Write-Host ("Disk image written: {0} ({1} bytes)" -f $ImageOut, $image.Length)
Write-Host ("  boot stub:    LBA 0, {0} bytes" -f 3)
Write-Host ("  CCP+BDOS:     LBA 1..{0}, {1} bytes total" -f (1 + [Math]::Ceiling($payload.Length / $SectorSize) - 1), $payload.Length)
Write-Host ("  CBIOS:        LBA {0}+, {1} bytes" -f $cbiosLba, $cbiosBytes.Length)
Write-Host ""
Write-Host "Mount this image in the plugin via Settings -> DiskImagePath"
Write-Host "(Phase 4.4 wiring) or via emfe_z80_mount_disk_image (Phase 4.1)."
