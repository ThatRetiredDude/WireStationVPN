/* The payload SDK's start-up code runs this, and it runs the Go program
 * (node/, built as a C archive). See scripts/build-ps5.sh.
 *
 * Copyright (C) 2026 ThatRetiredDude. GPL-3.0-or-later. */

int WSMain(void);

int main(void) { return WSMain(); }
