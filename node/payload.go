//go:build ws_payload

package main

// The way into the program when it's linked with the PS5 payload SDK's
// start-up code (scripts/build-ps5.sh, the "sdk" build): payload/main.c
// calls this.

import "C"

//export WSMain
func WSMain() C.int { return C.int(run(defaultDir)) }
