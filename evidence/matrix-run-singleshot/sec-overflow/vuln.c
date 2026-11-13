/*
 * vuln.c — classic stack buffer overflow demo
 *
 * Build (disable mitigations for the demo):
 *   gcc -o vuln vuln.c -fno-stack-protector -no-pie -z execstack -w
 *
 * What it shows:
 *   gets() performs an unbounded copy into a fixed-size buffer — identical in
 *   impact to strcpy(buf, untrusted_src) with no length check.  Both write
 *   past the end of buf when the source is longer than 64 bytes, clobbering
 *   saved RBP and then the return address sitting above it on the frame.
 *
 * Stack frame layout inside vulnerable() on x86-64:
 *
 *   higher addresses
 *   ┌──────────────────────────────┐
 *   │  return address  (8 bytes)   │  ← target: overwrite with win()
 *   ├──────────────────────────────┤
 *   │  saved RBP       (8 bytes)   │
 *   ├──────────────────────────────┤
 *   │  char buf[64]                │  ← overflow starts here
 *   └──────────────────────────────┘
 *   lower addresses
 *
 * Offset to return address: 64 (buf) + 8 (saved RBP) = 72 bytes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- marker / target function ------------------------------------ */

void win(void) {
    puts("[+] win() reached — return address overwrite successful");
    exit(0);
}

/* ---------- vulnerable function ----------------------------------------- */

static void vulnerable(void) {
    char buf[64];

    /*
     * gets() is equivalent to: strcpy(buf, untrusted_input)
     * — no length limit, no bounds check.
     * Input longer than 63 bytes overflows buf into saved RBP and
     * then into the return address.
     */
    if (gets(buf) == NULL) {
        fputs("[-] gets: read error\n", stderr);
        exit(1);
    }

    printf("[*] buf contains: \"%.64s\"\n", buf);
}

/* ---------- main -------------------------------------------------------- */

int main(void) {
    /*
     * Print win()'s address so exploit.py can read it without needing
     * symbols or a separate disassembly step.  -no-pie keeps it stable
     * across runs (no ASLR on the binary itself).
     */
    printf("[*] win() is at %p\n", (void *)win);
    fflush(stdout);

    vulnerable();

    puts("[*] returned normally — no overflow occurred");
    return 0;
}
