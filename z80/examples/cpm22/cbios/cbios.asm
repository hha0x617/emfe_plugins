; -----------------------------------------------------------------------------
; cbios.asm — Custom CBIOS for CP/M 2.2 running on emfe_plugin_z80
;
; Original work, MIT OR Apache-2.0 dual-licensed, (c) 2026 hha0x617.
;
; Maps the 17 CP/M 2.2 BIOS jump-table entries to:
;   * the plugin's UART  (I/O ports 0xFE = data, 0xFF = status)
;   * the plugin's sector-device disk MMIO (ports 0xF0..0xF7)
;
; See docs/phase4_design.md §3 for the design rationale and §1 for
; the disk MMIO contract. Phase 4.2 deliverable: this skeleton only —
; the build pipeline that combines CBIOS + CCP + BDOS into a bootable
; disk image lands in Phase 4.3.
;
; Memory layout (single-drive single-bank CP/M 2.2 on 64 KiB Z80):
;   0x0000-0x00FF   zero page (warm-boot jump, BDOS entry, default DMA)
;   0x0100-0xE3FF   TPA (transient program area)
;   0xE400-0xEBFF   CCP   (Console Command Processor, ~2 KiB)
;   0xEC00-0xF9FF   BDOS  (Basic Disk Operating System, ~3.5 KiB)
;   0xFA00-0xFFFF   CBIOS (this file)
;
; Build assumption: pasmo (single-binary, Windows-friendly, flat .bin
; output). Compatible with most Z80 assemblers — uses only standard
; mnemonics + ORG / EQU / DB / DW / DS pseudo-ops.
; -----------------------------------------------------------------------------

; -----------------------------------------------------------------------------
; System constants
; -----------------------------------------------------------------------------

CCP_BASE        EQU     0E400H          ; CCP entry
BDOS_BASE       EQU     0EC00H          ; BDOS entry
CBIOS_BASE      EQU     0FA00H          ; this file.
                                        ;
                                        ; This MUST match the BIOS-table
                                        ; address that CPM22.Z80's BDOS
                                        ; resolves internally (its own
                                        ; fake "JP 0" table sits at the
                                        ; very end of the assembled blob
                                        ; at 0xFA00..0xFA32). On boot,
                                        ; our CBIOS at 0xFA00 OVERLAYS
                                        ; that fake table, so BDOS's
                                        ; CALL CONOUT etc. lands in our
                                        ; real handlers. Calkins'
                                        ; CPM22.Z80 is 5683 bytes total
                                        ; — the trailing 51 bytes are
                                        ; precisely the fake BIOS table
                                        ; (= 17 entries × 3 bytes), which
                                        ; we MUST NOT load via WBOOT or
                                        ; we'd overwrite our real CBIOS.
DEFAULT_DMA     EQU     0080H           ; CP/M default DMA buffer

; BDOS entry is at BDOS_BASE + 6 (the first 6 bytes are a JMP to the
; actual BDOS body).
BDOS_ENTRY      EQU     BDOS_BASE + 6

; UART (see docs/mmio.md)
UART_DATA       EQU     0FEH
UART_STATUS     EQU     0FFH
UART_TX_READY   EQU     01H             ; bit 0 of status
UART_RX_AVAIL   EQU     02H             ; bit 1 of status

; Disk MMIO (see docs/phase4_design.md §1)
DISK_LBA0       EQU     0F0H
DISK_LBA1       EQU     0F1H
DISK_LBA2       EQU     0F2H
DISK_BUF_LO     EQU     0F3H
DISK_BUF_HI     EQU     0F4H
DISK_CMD        EQU     0F5H
DISK_STATUS     EQU     0F6H

DISK_CMD_NOP    EQU     00H
DISK_CMD_READ   EQU     01H
DISK_CMD_WRITE  EQU     02H

DISK_STAT_READY    EQU  01H
DISK_STAT_ERROR    EQU  02H
DISK_STAT_WPROT    EQU  04H
DISK_STAT_LOADED   EQU  08H

; SSSD-256K floppy geometry (Phase 4 target — see phase4_design.md §2.1).
;   77 tracks * 26 sectors/track * 128 bytes/sector = 256 256 bytes.
DISK_SPT        EQU     26              ; sectors per track
DISK_TRACKS     EQU     77

; -----------------------------------------------------------------------------
; CBIOS origin and BIOS jump table
; -----------------------------------------------------------------------------

                ORG     CBIOS_BASE

                ; Standard CP/M 2.2 BIOS jump-table layout (17 entries,
                ; 3 bytes each). BDOS picks BIOS routines up by index
                ; from this fixed structure.
                JP      BOOT            ; 00  cold boot
WBOOTE:         JP      WBOOT           ; 01  warm boot
                JP      CONST           ; 02  console status
                JP      CONIN           ; 03  console input
                JP      CONOUT          ; 04  console output
                JP      LIST            ; 05  list device output
                JP      PUNCH           ; 06  punch device output
                JP      READER          ; 07  reader device input
                JP      HOME            ; 08  move disk to track 0
                JP      SELDSK          ; 09  select disk drive
                JP      SETTRK          ; 10  set track number
                JP      SETSEC          ; 11  set sector number
                JP      SETDMA          ; 12  set DMA address
                JP      READ            ; 13  read a logical sector
                JP      WRITE           ; 14  write a logical sector
                JP      LISTST          ; 15  list device status
                JP      SECTRAN         ; 16  logical → physical sector

; -----------------------------------------------------------------------------
; BOOT — cold boot entry. Called once at power-on.
;
; Responsibilities (CP/M 2.2 convention):
;   1. Print a sign-on message (optional but customary).
;   2. Install the warm-boot vector at 0x0000 and BDOS entry at 0x0005.
;   3. Initialise the default DMA address.
;   4. Select drive A and jump into the CCP cold-start entry.
; -----------------------------------------------------------------------------

BOOT:
                LD      SP, 0100H       ; transient stack until CCP takes over

                ; Sign-on message — useful as a "did the BIOS run?"
                ; smoke signal in the captured UART output. The "*" is
                ; deliberately ugly so it's easy to spot in transcripts.
                LD      HL, SIGNON_MSG
                CALL    PRINT_STR

                ; Fall through into the common cold + warm boot setup.

GOCPM:
                ; Install reset / BDOS entry vectors per CP/M 2.2.
                ; Address 0x0000 = jump to warm boot;
                ; Address 0x0005 = jump to BDOS.
                LD      A, 0C3H         ; JP opcode
                LD      (0000H), A
                LD      HL, WBOOTE
                LD      (0001H), HL
                LD      (0005H), A
                LD      HL, BDOS_ENTRY
                LD      (0006H), HL

                ; Reset default DMA buffer to 0x0080.
                LD      BC, DEFAULT_DMA
                CALL    SETDMA

                ; Select drive A (also zeros the CBIOS-internal track /
                ; sector state). After SELDSK returns the DPH pointer,
                ; BDOS expects C to hold the selected drive and jumps
                ; into CCP cold-start.
                LD      C, 0
                CALL    SELDSK

                ; Hand control to the CCP. C = 0 means "cold start —
                ; load the prompt, no submit file pending".
                LD      C, 0
                JP      CCP_BASE

; -----------------------------------------------------------------------------
; WBOOT — warm boot entry. Called when a transient program terminates
; via JP 0x0000 or when the user presses ^C at the CCP prompt.
;
; Phase 4 single-drive responsibilities:
;   1. Reload CCP + BDOS from the system tracks (tracks 0..1 reserved).
;   2. Re-install the BDOS / warm-boot vectors and re-enter CCP.
;
; Reading CCP+BDOS from disk uses the same READ entry point that
; transient programs use — convenient code reuse but means we MUST
; have already set the disk MMIO up to a known state.
; -----------------------------------------------------------------------------

WBOOT:
                LD      SP, 0100H

                ; Reload CCP + BDOS from system tracks. CCP_BASE..BDOS_END
                ; spans CCP (0xE400) through end of BDOS (0xF9FF) = 0x1600
                ; bytes = 44 sectors of 128 bytes. Sectors start at LBA 1
                ; in the SSSD layout — sector 0 is the boot stub at 0x0000
                ; which the cold-boot loader already placed.
                LD      DE, 1           ; starting LBA
                LD      HL, CCP_BASE    ; destination address
                LD      B, 44           ; sector count — covers CCP+BDOS
                                        ; code + storage variables, but
                                        ; deliberately STOPS before LBA
                                        ; 45 which contains CPM22.Z80's
                                        ; fake BIOS jump table. Loading
                                        ; that would overwrite our real
                                        ; CBIOS at 0xFA00 and bootloop.
WBLOOP:
                PUSH    BC
                PUSH    DE
                PUSH    HL
                ; LBA → disk regs
                LD      A, E
                OUT     (DISK_LBA0), A
                LD      A, D
                OUT     (DISK_LBA1), A
                XOR     A
                OUT     (DISK_LBA2), A
                ; Buffer ptr → disk regs
                LD      A, L
                OUT     (DISK_BUF_LO), A
                LD      A, H
                OUT     (DISK_BUF_HI), A
                ; Trigger read; abort to BOOT (sign-on + retry) on error.
                LD      A, DISK_CMD_READ
                OUT     (DISK_CMD), A
                IN      A, (DISK_STATUS)
                AND     DISK_STAT_ERROR
                JP      NZ, BOOT
                POP     HL
                POP     DE
                POP     BC
                ; Advance HL by 128 (one sector) and LBA by 1.
                LD      A, L
                ADD     A, 80H
                LD      L, A
                LD      A, H
                ADC     A, 0
                LD      H, A
                INC     DE
                DJNZ    WBLOOP

                JP      GOCPM

; -----------------------------------------------------------------------------
; Console I/O
;   CONST  → A = 0xFF if a character is ready, 0x00 if not (CP/M convention)
;   CONIN  → A = next character from console (blocking)
;   CONOUT → write C to console (the plugin's UART)
; -----------------------------------------------------------------------------

CONST:
                IN      A, (UART_STATUS)
                AND     UART_RX_AVAIL
                RET     Z               ; not ready → A = 0 already
                LD      A, 0FFH
                RET

CONIN:
                IN      A, (UART_STATUS)
                AND     UART_RX_AVAIL
                JR      Z, CONIN        ; spin until a byte is available
                IN      A, (UART_DATA)
                RET

CONOUT:
                ; TX is always ready in this UART (TX_READY is hard-wired
                ; high in the device), so a status poll is unnecessary —
                ; left here as documentation and a no-cost guard against
                ; future status-bit semantics.
                LD      A, C
                OUT     (UART_DATA), A
                RET

; LIST / PUNCH / READER / LISTST stubs. The Phase 4 plugin has no
; printer / paper-tape devices; LIST is a silent drop, PUNCH is a
; silent drop, READER returns EOF (^Z = 0x1A), LISTST returns 0xFF
; (always ready) so CP/M does not block.
LIST:           RET
PUNCH:          RET
READER:         LD      A, 1AH
                RET
LISTST:         LD      A, 0FFH
                RET

; -----------------------------------------------------------------------------
; Disk addressing — CBIOS keeps track of (track, sector, DMA, drive)
; in working RAM and only contacts the disk hardware in READ / WRITE.
; -----------------------------------------------------------------------------

HOME:
                LD      BC, 0
                ; Fall through into SETTRK to zero the track state.

SETTRK:
                LD      (CUR_TRK), BC
                RET

SETSEC:
                LD      (CUR_SEC), BC
                RET

SETDMA:
                LD      (CUR_DMA), BC
                RET

SELDSK:
                LD      HL, 0           ; default: invalid drive
                LD      A, C
                OR      A
                RET     NZ              ; Phase 4 supports only drive A (C=0)
                LD      HL, DPH_A
                RET

; SECTRAN — logical → physical sector. Phase 4 uses identity mapping
; (no skew). BC = logical sector, DE = pointer to translation table
; (NULL = identity). Returns physical sector in HL.
SECTRAN:
                LD      H, B
                LD      L, C
                RET

; -----------------------------------------------------------------------------
; READ / WRITE — one 128-byte sector.
;
; Caller has set (CUR_TRK), (CUR_SEC), (CUR_DMA) via SET* and HL =
; physical sector from SECTRAN (which BDOS already called). For now
; we compute LBA = CUR_TRK * SPT + CUR_SEC and trigger the disk MMIO.
;
; Returns: A = 0 on success, A = 1 on hard error (per CP/M BIOS spec).
; -----------------------------------------------------------------------------

READ:
                CALL    SETUP_DISK
                LD      A, DISK_CMD_READ
                OUT     (DISK_CMD), A
                JR      FINISH_DISK

WRITE:
                CALL    SETUP_DISK
                LD      A, DISK_CMD_WRITE
                OUT     (DISK_CMD), A
                ; fall through

FINISH_DISK:
                IN      A, (DISK_STATUS)
                AND     DISK_STAT_ERROR
                RET     Z               ; A=0 already from the AND
                LD      A, 1            ; CP/M-style hard-error result
                RET

; Common disk-MMIO setup. Computes the LBA from (CUR_TRK, CUR_SEC) and
; loads it into 0xF0..0xF2, then sets the buffer pointer to CUR_DMA.
;
; LBA = CUR_TRK * DISK_SPT + CUR_SEC. With CUR_TRK 16-bit and SPT 26,
; the worst case for our 256 KiB floppy (77*26 = 2002) fits in 16 bits.
SETUP_DISK:
                ; Compute HL := CUR_TRK * 26 via shift-and-add:
                ;   (HL << 4) + (HL << 3) + (HL << 1) = HL * (16 + 8 + 2)
                ; DE holds the *2 partial, BC holds the *8 partial.
                LD      HL, (CUR_TRK)
                ADD     HL, HL          ; *2
                LD      D, H            ; save *2 in DE
                LD      E, L
                ADD     HL, HL          ; *4
                ADD     HL, HL          ; *8
                LD      B, H            ; save *8 in BC
                LD      C, L
                ADD     HL, HL          ; *16
                ADD     HL, BC          ; *16 + *8 = *24
                ADD     HL, DE          ; *24 + *2 = *26
                ; Add sector (CUR_SEC is 16-bit but range is 0..25; the
                ; high byte is always zero in CP/M 2.2, so we can ignore it)
                LD      A, (CUR_SEC)
                LD      E, A
                LD      D, 0
                ADD     HL, DE
                ; HL is now the LBA. Push to disk regs.
                LD      A, L
                OUT     (DISK_LBA0), A
                LD      A, H
                OUT     (DISK_LBA1), A
                XOR     A
                OUT     (DISK_LBA2), A
                ; Buffer pointer.
                LD      HL, (CUR_DMA)
                LD      A, L
                OUT     (DISK_BUF_LO), A
                LD      A, H
                OUT     (DISK_BUF_HI), A
                RET

; -----------------------------------------------------------------------------
; Sign-on message printed once at cold boot.
; -----------------------------------------------------------------------------

SIGNON_MSG:     DB      0DH, 0AH
                DB      "* emfe Z80 CBIOS (Phase 4 skeleton) *"
                DB      0DH, 0AH, 0

PRINT_STR:
                ; HL → null-terminated string. Trashes A.
PRINT_LOOP:
                LD      A, (HL)
                OR      A
                RET     Z
                LD      C, A
                CALL    CONOUT
                INC     HL
                JR      PRINT_LOOP

; -----------------------------------------------------------------------------
; CBIOS working storage — track / sector / DMA / drive variables.
; -----------------------------------------------------------------------------

CUR_TRK:        DW      0
CUR_SEC:        DW      0
CUR_DMA:        DW      DEFAULT_DMA

; -----------------------------------------------------------------------------
; Disk Parameter Header + Disk Parameter Block for drive A (SSSD-256K)
; -----------------------------------------------------------------------------

; DPH (Disk Parameter Header) layout, per CP/M 2.2 BDOS spec:
;   +0  XLT     translation table pointer (0 = identity)
;   +2  6-byte scratch area used by BDOS
;   +8  DIRBUF  pointer to 128-byte shared directory scratch
;   +10 DPB     pointer to Disk Parameter Block
;   +12 CSV     check vector (size CKS bytes; per-drive)
;   +14 ALV     allocation vector (size (DSM/8)+1 bytes; per-drive)
DPH_A:          DW      0               ; XLT — no translation
                DW      0               ; SCR1
                DW      0               ; SCR2
                DW      0               ; SCR3
                DW      DIRBUF          ; DIRBUF
                DW      DPB_A           ; DPB
                DW      CSV_A           ; CSV
                DW      ALV_A           ; ALV

; DPB (Disk Parameter Block) for SSSD-256K floppy:
;   SPT = 26      sectors per track
;   BSH = 3       block shift (1 KiB blocks)
;   BLM = 7       block mask
;   EXM = 0       extent mask
;   DSM = 251     last block number (252 1-KiB blocks = 252 KiB usable)
;   DRM = 63      directory entries minus 1 (64 entries = 2 KiB)
;   AL0/AL1       directory allocation bitmap (first 2 blocks: 0xC0, 0x00)
;   CKS = 16      check size (DRM+1)/4 for removable media
;   OFF = 2       reserved tracks at start of disk for system / boot
DPB_A:          DW      DISK_SPT        ; SPT
                DB      3               ; BSH
                DB      7               ; BLM
                DB      0               ; EXM
                DW      251             ; DSM
                DW      63              ; DRM
                DB      0C0H, 00H       ; AL0, AL1
                DW      16              ; CKS
                DW      2               ; OFF

DIRBUF:         DS      128             ; shared directory scratch

; ALV / CSV sizes from DPB:
;   ALV bytes = (DSM / 8) + 1 = 252 / 8 + 1 = 32
;   CSV bytes = CKS                              = 16
ALV_A:          DS      32
CSV_A:          DS      16

; -----------------------------------------------------------------------------
; Padding to a known end address keeps the assembled binary's size
; predictable for the Phase 4.3 build pipeline.
; -----------------------------------------------------------------------------

CBIOS_END:
                ; (no padding here — Phase 4.3 will measure the actual
                ; size and adjust the disk-image layout accordingly.)

                END     BOOT
