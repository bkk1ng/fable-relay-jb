/*
 * vuln.c — classic stack buffer overflow demo
 * C11, x86-64 Linux
 *
 * Build (disable mitigations for the demo):
 *   gcc -std=c11 -O0 -g \
 *       -fno-stack-protector \
 *       -no-pie \
 *       -z execstack \
 *       -o vuln vuln.c
 *
 * Run:
 *   ./vuln <input>
 *
 * The program prints the address of win() so the exploit can build its
 * payload even when ASLR is on (because -no-pie keeps the text segment
 * at a fixed base, but the helper address is printed anyway for clarity).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── target function ──────────────────────────────────────────────────────── */

void win(void)
{
    puts("[+] win() reached — execution redirected successfully.");
}

/* ── vulnerable function ──────────────────────────────────────────────────── */

/*
 * Stack frame layout inside vuln() on x86-64 System V ABI:
 *
 *   higher addresses
 *   ┌─────────────────────────┐
 *   │  return address (8 B)   │  ← overwrite target
 *   ├─────────────────────────┤
 *   │  saved RBP      (8 B)   │
 *   ├─────────────────────────┤
 *   │  buf[64]                │  ← strcpy writes here, no bounds check
 *   └─────────────────────────┘
 *   lower addresses
 *
 * Total padding to reach the return address: 64 + 8 = 72 bytes.
 */
static void vuln(const char *arg)
{
    char buf[64];

    printf("[i] buf  @ %p\n", (void *)buf);

    /* !! intentionally unsafe — do not use strcpy in real code !! */
    strcpy(buf, arg);

    printf("[i] buf contains: %.64s\n", buf);
}

/* ── entry point ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Print the marker address so exploit.py can parse it. */
    printf("[i] win() @ %p\n", (void *)win);

    vuln(argv[1]);

    puts("[i] returned normally (no overflow occurred).");
    return EXIT_SUCCESS;
}
