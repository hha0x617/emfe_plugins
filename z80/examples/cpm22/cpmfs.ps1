# -----------------------------------------------------------------------------
# cpmfs.ps1 — minimal CP/M 2.2 filesystem writer (and reader, for verification)
#
# Operates on flat byte-array disk images matching the SSSD-256K layout
# the rest of this examples/cpm22/ pipeline uses (see phase4_design.md §1
# and phase5_design.md §3.1 for the geometry).
#
# Public API:
#   New-CpmFsContext  -Image <byte[]>
#       Build an FS context bound to an in-memory disk image. Allocator
#       starts with the directory blocks (0 and 1) marked in-use; all
#       other blocks free.
#
#   Add-CpmFile       -Ctx <ctx> -HostPath <string> [-CpmName <string>]
#       Add a host file into the CP/M filesystem: allocate blocks, write
#       a directory entry, copy bytes into the data area. CpmName
#       defaults to the host filename (e.g. "PIP.COM"). Phase 5.2 limit:
#       single-extent files only (≤ 16 KiB / 16 blocks).
#
#   Read-CpmDirectory -Image <byte[]>
#       Walk the directory and return [pscustomobject]s for non-empty
#       entries. Used for image verification (Phase 5.4 transcript).
#
# Layout constants embedded here match the standard CP/M 2.2 SSSD-256K
# DPB (SPT=26, BSH=3 → 1-KiB blocks, OFF=2 reserved tracks, DRM=63 →
# 64 directory entries, AL0/AL1 = 0xC0/0x00 → first 2 blocks reserved
# for the directory).
# -----------------------------------------------------------------------------

$Script:SectorSize     = 128
$Script:BlockSize      = 1024            # = 8 sectors
$Script:DataAreaLba    = 52              # OFF (2 tracks) × SPT (26) = 52
$Script:DataAreaOffset = 52 * 128        # = 6656
$Script:DirEntries     = 64              # DRM (63) + 1
$Script:DirEntrySize   = 32
$Script:DirBlocks      = 2               # AL0 = 0xC0 → blocks 0 + 1
$Script:TotalBlocks    = 252             # DSM (251) + 1
$Script:MaxBlocksPerExtent = 16          # EXM = 0

function New-CpmFsContext {
    param(
        [Parameter(Mandatory)] [byte[]] $Image
    )
    if ($Image.Length -lt ($Script:DataAreaOffset + $Script:DirEntries * $Script:DirEntrySize)) {
        throw "Image too small for the SSSD-256K directory area."
    }
    $free = New-Object bool[] $Script:TotalBlocks
    # The first two blocks (0 and 1) are reserved for the directory.
    $free[0] = $true
    $free[1] = $true
    return [pscustomobject]@{
        Image        = $Image
        FreeBlocks   = $free
        NextDirEntry = 0
    }
}

function Add-CpmFile {
    param(
        [Parameter(Mandatory)] $Ctx,
        [Parameter(Mandatory)] [string] $HostPath,
        [string] $CpmName
    )
    if (-not (Test-Path $HostPath)) { throw "File not found: $HostPath" }
    $bytes = [System.IO.File]::ReadAllBytes($HostPath)
    $size  = $bytes.Length

    if (-not $CpmName) {
        $CpmName = Split-Path $HostPath -Leaf
    }
    # CP/M directory expects 8.3 — pad and uppercase.
    $name = ''
    $ext  = ''
    if ($CpmName -match '^([^.]*)\.?(.*)$') {
        $name = $Matches[1]
        $ext  = $Matches[2]
    }
    if ($name.Length -gt 8 -or $ext.Length -gt 3) {
        throw "CP/M filename '$CpmName' exceeds 8.3 (name='$name' ext='$ext')."
    }
    $nameField = $name.PadRight(8).Substring(0, 8).ToUpper()
    $extField  = $ext.PadRight(3).Substring(0, 3).ToUpper()

    $blocksNeeded  = [int][Math]::Ceiling($size / [double]$Script:BlockSize)
    $recordsNeeded = [int][Math]::Ceiling($size / [double]$Script:SectorSize)
    if ($blocksNeeded -gt $Script:MaxBlocksPerExtent) {
        throw ("File '$CpmName' is $size bytes (= $blocksNeeded blocks) — " +
               "exceeds the $Script:MaxBlocksPerExtent-block single-extent " +
               "limit. Multi-extent writes are Phase 5.5+ work.")
    }
    if ($Ctx.NextDirEntry -ge $Script:DirEntries) {
        throw "Directory full (64 entries used)."
    }

    # Allocate blocks linearly from the free pool, starting at block 2
    # (blocks 0/1 are the directory). Linear allocation is fine for
    # build-time population — fragmentation is not a concern.
    $allocated = New-Object 'System.Collections.Generic.List[int]'
    for ($b = $Script:DirBlocks; $b -lt $Script:TotalBlocks -and $allocated.Count -lt $blocksNeeded; $b++) {
        if (-not $Ctx.FreeBlocks[$b]) {
            $Ctx.FreeBlocks[$b] = $true
            [void] $allocated.Add($b)
        }
    }
    if ($allocated.Count -lt $blocksNeeded) {
        throw "Out of free blocks for '$CpmName' (needed $blocksNeeded, allocated $($allocated.Count))."
    }

    # Build the 32-byte directory entry.
    $entry = New-Object byte[] $Script:DirEntrySize
    $entry[0]  = 0                              # user code 0
    for ($i = 0; $i -lt 8; $i++) { $entry[1 + $i] = [byte][char] $nameField[$i] }
    for ($i = 0; $i -lt 3; $i++) { $entry[9 + $i] = [byte][char] $extField[$i] }
    $entry[12] = 0                              # extent (low)
    $entry[13] = 0                              # reserved (S1)
    $entry[14] = 0                              # extent (high) — always 0 in Phase 5.2
    $entry[15] = [byte] $recordsNeeded          # RC
    for ($i = 0; $i -lt 16; $i++) {
        $entry[16 + $i] = if ($i -lt $allocated.Count) { [byte] $allocated[$i] } else { 0 }
    }

    # Write directory entry.
    $dirOff = $Script:DataAreaOffset + $Ctx.NextDirEntry * $Script:DirEntrySize
    [Array]::Copy($entry, 0, $Ctx.Image, $dirOff, $Script:DirEntrySize)
    $Ctx.NextDirEntry++

    # Write file data, block by block.
    $bytePos = 0
    foreach ($b in $allocated) {
        $blockOff  = $Script:DataAreaOffset + ($b * $Script:BlockSize)
        $chunkSize = [Math]::Min($Script:BlockSize, $size - $bytePos)
        [Array]::Copy($bytes, $bytePos, $Ctx.Image, $blockOff, $chunkSize)
        $bytePos += $chunkSize
    }

    # Pretty diagnostic.
    Write-Host ("  + {0,-12} {1,5} B  RC={2,3}  blocks={3}" -f
                $CpmName, $size, $recordsNeeded,
                ($allocated -join ','))
}

function Read-CpmDirectory {
    param(
        [Parameter(Mandatory)] [byte[]] $Image
    )
    $entries = @()
    for ($e = 0; $e -lt $Script:DirEntries; $e++) {
        $off = $Script:DataAreaOffset + $e * $Script:DirEntrySize
        $user = $Image[$off]
        if ($user -eq 0xE5) { continue }    # free slot
        $name = -join (0..7 | ForEach-Object { [char]($Image[$off + 1 + $_] -band 0x7F) })
        $ext  = -join (0..2 | ForEach-Object { [char]($Image[$off + 9 + $_] -band 0x7F) })
        $extentLo = $Image[$off + 12]
        $extentHi = $Image[$off + 14]
        $rc       = $Image[$off + 15]
        $blocks   = @()
        for ($i = 0; $i -lt 16; $i++) {
            $b = $Image[$off + 16 + $i]
            if ($b -ne 0) { $blocks += $b }
        }
        $entries += [pscustomobject]@{
            Index   = $e
            User    = $user
            Name    = $name.TrimEnd()
            Ext     = $ext.TrimEnd()
            Extent  = $extentLo + $extentHi * 32
            Records = $rc
            Blocks  = $blocks
        }
    }
    return $entries
}
