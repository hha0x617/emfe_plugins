# Phase 5 — CP/M 2.2 smoke tests

Status: **Design draft** — review-ready, no implementation yet.
Companion: [`emfe_plugins_cpm_supplements`](https://github.com/hha0x617/emfe_plugins_cpm_supplements)
(private as of 2026-05-24).
Precedent: [`phase4_design.md`](phase4_design.md) — same review-doc shape.

This document fills in the level of detail that
[`implementation_plan.md`](implementation_plan.md) §7 leaves open. It
covers the CP/M utility inventory and licensing, the disk-image
filesystem-write extension needed to bake utility binaries onto the
boot disk, the REPL-driven smoke harness shape, the per-tool pass
criteria, and the sub-phase decomposition. Each section ends with
the concrete sub-phase (5.1–5.5) it unblocks.

## 0. Where Phase 4 left us (recap)

Phase 4 closed end-to-end on 2026-05-24 (PR #77) with a real `a>`
prompt captured from cold boot:

```
* emfe Z80 CBIOS (Phase 4 skeleton) *

a>
```

Phase 4's disk image holds:

| LBA range | Content | Source |
|---|---|---|
| 0 | Boot stub (`JP 0xFA00`) | Phase 4.3 build.ps1 |
| 1..44 | CCP + BDOS (5632 B, code+data) | Calkins' `CPM22.Z80` |
| 45 | CPM22.Z80's internal fake BIOS jump table (intentionally NOT staged) | Calkins' `CPM22.Z80` |
| 46..57 | Custom CBIOS (~541 B + padding) | `cbios.asm` |
| 58..2001 | Empty CP/M filesystem (all `0xE5`) | build.ps1 |

The `a>` prompt is functional — BDOS / CCP both work — but **no
user files are on the disk**, so the user can't actually do
anything from the prompt. Typing `DIR` would print `NO FILES`;
typing `PIP` would print `?` (command not found).

Phase 5 makes the disk *usable* by baking the standard CP/M
utilities onto it, then drives the prompt to actually run them.

## 1. CP/M 2.2 utility inventory (sub-phase 5.1 input)

The eight transient utilities that ship with stock CP/M 2.2 from
Digital Research:

| Utility | Typical size | Function |
|---|---|---|
| `PIP.COM` | ~7.7 KiB | **P**eripheral **I**nterchange **P**rogram — file copy / concat / device redirect |
| `STAT.COM` | ~5.5 KiB | Disk + file usage stats; set file/disk attributes |
| `ED.COM` | ~6.7 KiB | Line-oriented text editor |
| `SUBMIT.COM` | ~2 KiB | Batch-command-file processor |
| `XSUB.COM` | ~1 KiB | Resident submit-input redirector |
| `LOAD.COM` | ~2 KiB | Hex-image-to-COM converter |
| `DUMP.COM` | ~256 B | Hex dump of a file |
| `DDT.COM` | ~6 KiB | Dynamic Debugging Tool (mini-monitor with breakpoints + disasm) |

`DIR` and `TYPE` are NOT separate utilities — they're built into
the CCP. `ERA`, `REN`, `SAVE`, `USER` are also built-in. Phase 4's
shipped CCP can run them today; they just don't have files to
operate on yet.

For the Phase 5 smoke test the **primary** targets are the four
the implementation plan calls out: `DIR` (built-in, trivial after
file-system population), `PIP` (the user-visible workhorse),
`STAT` (proves disk parameter block round-trip), `ED` (proves
console line-editing wiring). The remaining four are stretch
goals — if `PIP` works the others almost certainly do too, but
asserting on each is cheap.

### 1.1 Sourcing options

| Source | Pro | Con |
|---|---|---|
| **A. Extract from `D:/projects/cpm/CP_M 2.2/22src1.img`** (an existing CP/M-formatted disk in the local scratchpad) | Already on the dev host; bytes are verified DRI distribution; matches Sparks-letter scope unambiguously | Requires a CP/M filesystem reader to extract |
| **B. Use `cpmtools` (cpm8680.com) directly** to read 22src1.img and write a fresh image | Industry-standard tool; handles directory walk + extent allocation; one tool to learn | External dependency the user has to install; PowerShell-native pipeline drifts toward "needs WSL or cygwin" |
| **C. Re-assemble from upstream `*.ASM` sources** (cpm22pat.zip contains some patches but not full utilities) | Full source provenance | Source for most CP/M 2.2 utilities is *not* freely available even from DRI's archives; would need asl / mac for each utility |
| **D. Use pre-built binaries from a Z80 retrocomputing project** (e.g., RC2014 distribution, joyce, mame's cpm images) | Often well-curated, easy to drop in | Each distribution has its own license posture re-statement; need per-source review |

**Lean: A** (extract from 22src1.img). Reasons: bytes are upstream
DRI; license is unambiguously under the Sparks 2022 letter via the
same chain CPM22.Z80 already uses; no new external tool needed if
we write a tiny CP/M filesystem reader in PowerShell.

Open question: do we write the FS reader inline in `build.ps1`,
or factor to a separate `cpmfs.ps1` module? The reader is also
useful for **verifying** images later (Phase 5.5). Lean:
separate module so the read + write helpers share types.

## 2. License posture for utility binaries

The Sparks 2022 letter that covers CPM22.Z80 also covers the eight
DRI-supplied utility binaries — they're part of the same CP/M 2.2
distribution, named directly. Per-file inclusion review per
`emfe_plugins_cpm_supplements/scope.md` §1.4 still happens:

- Each `.COM` file mirrored from 22src1.img gets a row in
  `emfe_plugins_cpm_supplements/docs/provenance.md` with SHA-256
  of the bytes-as-extracted, source disk image SHA-256, extraction
  date, normalisation notes ("none — copied byte-for-byte from
  directory extents").
- File extension stays `.COM` (uppercase, matching CP/M FS
  convention).
- Files land in `cpm22/binary/utility/` (new subdirectory), not
  `cpm22/binary/` (which `scope.md` reserves for CCP + BDOS).

The mirrored utility binaries are CP/M-licensed, inherit the same
`LICENSE-CPM-2022.txt` umbrella, and stay in the companion repo.
**Never** copied into the public `emfe_plugins` tree.

## 3. Disk-image filesystem write (sub-phase 5.2)

Today `build.ps1` writes the system tracks (LBA 0..57) + fills the
data area (LBA 58+) with `0xE5` (CP/M "empty directory" marker).
Phase 5 extends it to also write a **valid CP/M filesystem** with
named files in the data area.

### 3.1 CP/M 2.2 disk geometry recap

For the SSSD-256K layout we use (matches CPM22.Z80 DPB defaults):

| Constant | Value | Meaning |
|---|---|---|
| `SPT` | 26 | sectors per track |
| `BSH` / `BLM` | 3 / 7 | block-shift / block-mask → 1 KiB blocks (8 sectors / block) |
| `EXM` | 0 | extent mask |
| `DSM` | 251 | last block number (= 252 data blocks) |
| `DRM` | 63 | last directory entry (= 64 entries = 2 KiB directory) |
| `AL0/AL1` | `0xC0 0x00` | directory occupies first 2 data blocks |
| `OFF` | 2 | reserved tracks at start (CCP+BDOS+CBIOS live here) |

Net usable space: 252 blocks × 1 KiB - 2 blocks (directory) =
**250 KiB** of file storage. Way more than the ~32 KiB the eight
standard utilities need.

### 3.2 Directory entry layout (32 bytes per entry)

```
+0   user code (0–15, or 0xE5 = empty)
+1   filename (8 bytes, uppercase, space-padded)
+9   extension (3 bytes, uppercase, space-padded)
+12  extent number (low byte)
+13  reserved (always 0)
+14  reserved (always 0)
+15  RC — record count of this extent (0–128, in 128-byte records)
+16  allocation map (16 bytes, one block number per byte for
     blocks-per-extent ≤ 16; for 1-KiB blocks on SSSD-256K
     each byte is one block index)
```

For Phase 5 every utility fits in a single 16-block extent
(~16 KiB max), so we never need multi-extent files.

### 3.3 `build.ps1` extension plan

Add a `Add-CpmFile` helper that:

1. Reads the .COM bytes from disk
2. Allocates N = `ceil(size / 1024)` blocks from the free pool
   (track a `[bool[252]]` free bitmap; mark blocks 0–1 used for
   directory)
3. Picks the next free directory slot (next non-`0xE5` user
   byte slot)
4. Fills the directory entry: user=0, filename padded, extension
   padded, extent=0, RC = `ceil(size / 128)`, allocation list
   with N block numbers (then 0-pad to 16)
5. Writes the file bytes into the data area at
   `(OFF × SPT + block × 8) × 128` per allocated block

After all files are added, write the directory back to disk
(first 2 blocks of data area = LBA 52..67).

The helper is ~80–100 LoC of PowerShell. Sub-phase 5.2
deliverable.

### 3.4 Reference image after Phase 5.2

A `cpm22-sssd.img` with:

| File | Size (target) | Blocks |
|---|---|---|
| `PIP.COM` | ~7.7 KiB | 8 |
| `STAT.COM` | ~5.5 KiB | 6 |
| `ED.COM` | ~6.7 KiB | 7 |
| `SUBMIT.COM` | ~2 KiB | 2 |
| `XSUB.COM` | ~1 KiB | 1 |
| `LOAD.COM` | ~2 KiB | 2 |
| `DUMP.COM` | ~256 B | 1 |
| `DDT.COM` | ~6 KiB | 6 |
| **Total** | ~31 KiB | 33 blocks |

Plus 2 directory blocks = 35 of 252 used. Plenty of room for
test data files the smoke tests will create.

## 4. REPL-driven smoke harness (sub-phase 5.3)

`TestCpmBootIfPresent` currently checks for `a>` then exits.
Phase 5 needs a harness that **types commands into the prompt**
and reads back results. The shape:

```cpp
struct CpmSession {
    EmfeInstance inst;
    CaptureCtx   ctx;
    std::string  CaptureSince(size_t mark);
    bool         WaitForPrompt(int64_t maxSteps);
    void         RunSteps(int64_t steps);
    void         TypeLine(const std::string& cmd);
};
```

### 4.1 Input wire — `TypeLine`

CCP reads keystrokes via BIOS `CONIN`. We push bytes into the
plugin's UART receive FIFO (existing `emfe_send_string` / Phase
1.2 hook), then run the CPU enough steps for CCP to drain them.
Two subtleties:

- **Line terminator**: CP/M expects `CR` (`0x0D`) to end a line,
  not `LF`. Sending `"DIR\r"` is correct; sending `"DIR\r\n"`
  injects an extra blank line at the prompt (harmless but noisy
  in transcripts).
- **Echo**: CCP echoes each typed character back to CONOUT as
  it reads them. The captured stream will contain `D I R \r \n`
  (CCP appends LF on CR) before the command's actual output.
  The harness compares against the post-command tail, not the
  whole capture.

### 4.2 Prompt detection — `WaitForPrompt`

Run `emfe_step` in a loop, stopping when the captured tail ends
with `a>` (or `b>`, `c>`, … if the test switched drives). Cap at
~5M instructions per command. Failure to reach the prompt within
budget = test fail with "command hung" diagnostic.

Important: CCP prints the prompt on a **fresh line** (CR LF then
`a>`). So the pattern is `\n[a-p]>`. The harness compares the
two-byte suffix `[letter]>` after stripping trailing CR/LF.

### 4.3 Per-command output capture — `CaptureSince`

```cpp
size_t mark = session.ctx.captured.size();
session.TypeLine("DIR");
session.WaitForPrompt(5'000'000);
std::string output = session.CaptureSince(mark);
```

`output` will contain the typed echo + CCP's CRLF + the command
output + CCP's next prompt. Per-tool spec describes exactly which
substring to assert on.

### 4.4 Shared boot fixture

A helper that boots the image once + waits for the first prompt
+ returns a `CpmSession`. Reused across every Phase 5 smoke test.

## 5. Per-tool smoke specs (sub-phase 5.4)

Each test is a `TestCpm<Tool>IfPresent` that runs only when the
disk image is at `examples/cpm22/cpm22-sssd.img` (same SKIP-when-
absent pattern as `TestCpmBootIfPresent`). Bundled into a single
`TestCpmSmokeIfPresent` block at the top level so the boot
fixture is shared.

| Test | Command | Pass criterion |
|---|---|---|
| `Smoke_DirEmpty` | `DIR FOO` | output contains `NO FILE` (CCP's "not found" line) |
| `Smoke_DirListing` | `DIR` | output contains all 8 utility names — `PIP`, `STAT`, `ED`, `SUBMIT`, `XSUB`, `LOAD`, `DUMP`, `DDT` |
| `Smoke_Type` | `TYPE PIP.COM` (or some pre-baked text file) | output contains the file's first byte sequence — for binary files, just check non-empty + no hang |
| `Smoke_Pip` | `PIP B:=A:PIP.COM` — wait, no B: drive. Use `PIP NEW.TXT=A:PIP.COM[O]` (binary copy to same drive) | next `DIR` shows `NEW.TXT`; STAT shows correct size |
| `Smoke_Stat` | `STAT` | output contains a recognisable disk-stats line like `A: R/W, Space:` |
| `Smoke_StatFile` | `STAT PIP.COM` | output contains `PIP.COM` and the size in records |
| `Smoke_Ed` | `ED FOO.TXT` then `i`/text/`^Z`/`E` | exits cleanly back to prompt; `DIR` shows `FOO.TXT` after; `TYPE FOO.TXT` shows the typed text |
| `Smoke_Submit` (stretch) | create `SUB.SUB` containing `STAT`, then `SUBMIT SUB` | STAT runs as if typed |
| `Smoke_Dump` (stretch) | `DUMP DUMP.COM` | output is hex |
| `Smoke_Ddt` (stretch) | `DDT PIP.COM` then `G0` (or just `^C` to exit) | prompt returns |

**`ED` is the riskiest** of the four primary targets — it does
console-mode line-editing with special-character handling that
exercises BIOS `CONST` polling. If `CONIN`'s spin-loop semantics
or `CONST`'s status-bit reporting is even slightly off, `ED`
hangs or misreads.

## 6. Optional: TP3 / Hi-Tech C (sub-phase 5.5 stretch)

The implementation plan §7 lists "optional: Turbo Pascal 3.0 or
Hi-Tech C compile a Hello World". This is a real stretch goal:

- **Turbo Pascal 3.0**: commercial, no clean freeware release.
  Borland's various community-license offerings cover TP3 but the
  posture is muddier than the Sparks letter. Even if obtained,
  bringing it up reveals subtle CCP/BDOS bugs (TP3 uses BDOS in
  unusual ways).
- **Hi-Tech C**: distributed by HI-TECH Software under
  "non-commercial use" terms historically; some CP/M variants
  available. Same posture-clarity issue.

Lean: **defer to Phase 5.5+** as a Stretch-Stretch target. Phase
5's MVP closes when sub-phases 5.1–5.4 land and `DIR / PIP / STAT
/ ED` all pass their smoke gates.

## 7. Cross-repo flow update (sub-phase 5.2 + 5.3 input)

```
emfe_plugins_cpm_supplements/                       (CP/M-licensed)
  cpm22/source/CPM22.Z80              ← Phase 4 mirror, verbatim
  cpm22/binary/utility/                ← Phase 5.1 — NEW
    PIP.COM, STAT.COM, ED.COM, ...    extracted from 22src1.img
  docs/provenance.md                  ← Phase 5.1 — extended

emfe_plugins/z80/                                     (MIT)
  examples/cpm22/build.ps1            ← Phase 5.2 — extend
    + Add-CpmFile helper
    + reads cpm22/binary/utility/*.COM
    + writes CP/M directory entries
  examples/cpm22/cpmfs.ps1            ← Phase 5.2 — NEW (module)
    Read-CpmDirectory, Add-CpmFile, ...
  tests/test_z80.cpp                  ← Phase 5.3 — extend
    CpmSession harness + Smoke_* tests
```

One-way file flow unchanged: emfe_plugins reads from companion,
writes back disk images that inherit CP/M licensing.

## 8. Phase 5 sub-phase decomposition

| # | Theme | Status |
|---|---|---|
| 5.1 | Mirror CP/M utility binaries (`PIP.COM`, `STAT.COM`, `ED.COM`, ...) from `22src1.img` into companion's `cpm22/binary/utility/` + extend `provenance.md`. Includes writing a tiny CP/M filesystem **reader** to do the extraction | planned |
| 5.2 | Extend `build.ps1` (via `cpmfs.ps1` module) to bake utility .COM files onto the disk image — directory entries + block allocation + file data writes | planned |
| 5.3 | `CpmSession` REPL-driven smoke harness in `tests/test_z80.cpp` — input echo, line-end conventions, prompt detection, per-command output capture | planned |
| 5.4 | Per-tool smoke tests — `DIR / PIP / STAT / ED` are MVP; `SUBMIT / DUMP / DDT` are stretch in the same PR | planned |
| 5.5 (stretch) | Turbo Pascal 3.0 / Hi-Tech C path — defer pending license posture review on each | deferred |

**Phase 5 done when**: `DIR / PIP / STAT / ED` smoke tests all pass
locally on a fresh `build.ps1` output; CI sees them SKIP cleanly
until the user runs the build.

## 9. Acceptance criterion

A real run that captures (last ~200 chars):

```
A> DIR
A: PIP      COM : STAT     COM : ED       COM : SUBMIT   COM
A: XSUB     COM : LOAD     COM : DUMP     COM : DDT      COM
A> STAT *.COM
   2000 32K of 250K Remaining on A:
A> PIP NEW.TXT=PIP.COM
A>
```

(Exact column-spacing depends on the CCP version; the smoke tests
match substrings, not column widths.)

## 10. Open questions for review

1. **CP/M filesystem reader/writer language** — PowerShell-native
   (matches the rest of the pipeline; ~200 LoC across the two
   directions) vs `cpmtools` external dependency (less code, more
   tooling). Lean: **PowerShell-native** in a `cpmfs.ps1` module.
2. **Per-utility name normalisation** — DRI's 22src1.img stores
   filenames upper-case + space-padded; do we keep that exact
   form when mirroring or canonicalise to short form (e.g.
   `pip.com`)? Lean: **keep upper-case + padding** so the bytes
   stay byte-identical to the source; the file system itself is
   case-preserving.
3. **Multi-extent files** — none of the 8 standard utilities need
   them at SSSD-256K. Do we still implement multi-extent in the
   FS writer for future use (TP3 / Hi-Tech C are larger)? Lean:
   **defer** — single-extent only in 5.2; extend if 5.5 needs it.
4. **`ED` interactive vs scripted** — `ED.COM` is interactive
   and CP/M's most input-heavy utility. The smoke test sends
   `i`/text/`^Z`/`E` as a script. Are there CCP/BDOS bugs likely
   to be uncovered? Lean: **yes, likely** — `ED` will probably
   surface 1-2 more bugs the way `CCP` cold boot did. Budget
   one extra debug iteration.
5. **`SUBMIT` / `XSUB`** — these are BDOS-resident; they may
   interact with BDOS function 47 (chain to program) in ways
   our minimal Phase 4 CBIOS doesn't yet support. Lean:
   **stretch goal in 5.4**, deferred to a follow-up if it
   doesn't fall out for free.
6. **Boot transcript verification** — capture the cold-boot
   transcript (Phase 4's `a>` + DIR + each smoke command) as
   a frozen text file in the companion's `disks/` next to the
   image, for human inspection. Lean: **yes**, names like
   `cpm22-sssd-boot-transcript.txt`. Single small file.
7. **TP3 / Hi-Tech C** — deferred but worth a posture check
   before promising. Lean: **revisit at Phase 5 close** rather
   than committing now.
8. **5.4 test layering** — one big `TestCpmSmokeIfPresent`
   block that boots once and runs every smoke command (faster,
   shares the boot cost) vs per-command separate `TestCpm<Tool>
   IfPresent` tests (clearer failure isolation). Lean: **one
   block with sub-sections**, since boot is ~5M instructions
   and we save time at the cost of slightly muddier "which test
   failed" reporting. Each sub-section prints a clear banner so
   transcripts stay diagnostic.
