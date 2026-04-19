# Z8000 Sample Programs

Each sample is a Python generator (`gen_*.py`) that emits a 64 KB raw memory
image (`*.bin`). Load the `.bin` via the emfe frontend's **File → Load Binary**
at address 0, or from code with `emfe_load_binary(inst, path, 0)`.

All samples target Z8002 (non-segmented) mode. The Program Status Area at
$0002/$0004 holds the initial FCW ($2000 = System mode) and PC ($0100).

## hello/

Prints `"Hello, Z8000!\r\n"` to the UART (port $FE00), then halts.

Uses: LD immediate, LDB @Rs, OUTB #port,RBs, ADD, JR cc, HALT.

```
  $0100: LD   R15,#$F000
         LD   R2,#1                   ; pointer increment
         LD   R3,#msg
  $010C: LDB  RL0,@R3                 ; loop: load byte
         ORB  RL0,RL0
         JR   Z,done
         OUTB #$FE00,RL0
         ADD  R3,R2
         JR   T,loop
  $011A: HALT
         "Hello, Z8000!\r\n\0"
```

## echo/

UART echo via polling. Prints a banner, then repeatedly polls the status
register for RX-ready, reads the incoming byte, and echoes it back. Expands
CR to CR+LF and reprints the "> " prompt.

Uses: CALR subroutines, CPB, ANDB, INB #port, OUTB #port, conditional JR.

Subroutines: `print_str` (takes R2=pointer), `wait_rx` (spin until RX-ready).

## fibonacci/

Computes the first 11 Fibonacci numbers iteratively (0, 1, 1, 2, 3, 5, 8,
13, 21, 34, 55) and prints each as decimal. Written entirely with the
Phase 1 ISA — no INC/DEC, no shifts, no multiply/divide.

Decimal conversion peels digits by repeated subtraction against
(10000, 1000, 100, 10, 1), with leading-zero suppression. Digit→ASCII uses
ADDB with a pre-loaded RL7 = `'0'` constant.

Expected output:
```
Fibonacci:
0
1
1
2
3
5
8
13
21
34
55
HALT
```

## Regenerating

```bash
cd examples/hello && python gen_hello.py
cd examples/echo  && python gen_echo.py
cd examples/fibonacci && python gen_fibonacci.py
```

Generator scripts have no external dependencies beyond CPython 3.8+.

## Verification

The test harness (`tests/test_z8000.cpp`) loads each `.bin` and checks the
UART output against expected content. Run with the examples directory as
a hint:

```bash
export EMFE_Z8000_EXAMPLES_DIR=$PWD/examples
./build/bin/Release/test_z8000.exe
```

When the env var is unset the example tests are skipped silently.
