//go:build linux && amd64 && ws_loadertest

#include "textflag.h"

// The PS5's ELF loader (port 9021) starts a payload like a C function call:
// the return address on the stack and the loader's arguments in DI. Go's
// start-up code wants what a kernel gives a new program instead: argc,
// argv, envp and the aux vector on the stack. ws_entry builds that (one
// argument, the program's name; no environment; the page size) and goes on
// to Go's normal start. scripts/build-ps5.sh makes it the ELF's entry.
TEXT ws_entry(SB),NOSPLIT|NOFRAME,$0
	ANDQ	$~15, SP
	SUBQ	$64, SP
	MOVQ	$1, 0(SP)              // argc
	LEAQ	ws_name<>(SB), AX
	MOVQ	AX, 8(SP)               // argv[0]
	MOVQ	$0, 16(SP)             // end of argv
	MOVQ	$0, 24(SP)             // end of envp
	MOVQ	$6, 32(SP)             // AT_PAGESZ
	MOVQ	$16384, 40(SP)
	MOVQ	$0, 48(SP)             // AT_NULL
	MOVQ	$0, 56(SP)
	JMP	_rt0_amd64_linux(SB)

DATA	ws_name<>+0(SB)/8, $"wirestat"
DATA	ws_name<>+8(SB)/8, $"ion\x00\x00\x00\x00\x00"
GLOBL	ws_name<>(SB), RODATA, $16
