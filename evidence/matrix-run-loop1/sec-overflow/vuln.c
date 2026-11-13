/*
 * vuln.c — classic stack buffer overflow (C11, x86-64 Linux)
 *
 * Compile (disable mitigations for demonstration purposes):
 *   gcc -std=c11 -m64 -fno-stack-protector -no-pie -z execstack \
 *       -o vuln vuln.c
 *
 * The binary prints win()'s address so exploit.py can build the payload
 * without having to parse the ELF or rely on a fixed load address.
 *
 * Stack layout inside vuln() (gcc -O0, no canary):
 *
 *   higher addresses
 *   ┌─────────────────┐  rbp + 0x08  ← return address  (8 bytes)
 *   ├─────────────────┤  rbp + 0x00  ← saved RBP       (8 bytes)
 *   ├─────────────────┤  rbp - 0x40  ← buf[64]         (64 bytes)
 *   └─────────────────┘
 *   lower addresses
 *
 * strcpy() has no length limit, so feeding 72+ bytes overwrites the
 * return address.  Redirect it to win() and the function "returns" there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Target function.  The goal of the exploit is to redirect execution here
 * without ever calling it directly.
 * ---------------------------------------------------------------------- */
void win(void)
{
    puts("[+] win() reached — stack smash successful");
    exit(0);
}

/* -------------------------------------------------------------------------
 * Vulnerable function.  buf is 64 bytes; strcpy() performs no bounds check.
 * ---------------------------------------------------------------------- */
static void vuln(const char *input)
{
    char buf[64];           /* 64-byte destination on the stack            */
    strcpy(buf, input);     /* ← unsafe: overwrites past buf if len > 63  */
    printf("buf @ %p: %.32s...\n", (void *)buf, buf);
}

/* -------------------------------------------------------------------------
 * Entry point.  Prints win()'s address so the exploit can read it.
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <input>\n", argv[0]);
        return 1;
    }

    /* Leak the target address.  exploit.py parses this line. */
    printf("win  @ %p\n", (void *)win);
    fflush(stdout);

    vuln(argv[1]);

    puts("returned normally — no overflow triggered");
    return 0;
}
