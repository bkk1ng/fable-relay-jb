/*
 * vuln.c — classic stack buffer overflow, educational demo
 *
 * Compile (stack canary and PIE disabled so the overflow is clean):
 *   gcc -std=c11 -g -O0 -fno-stack-protector -no-pie -o vuln vuln.c
 *
 * Enable core dumps before running:
 *   ulimit -c unlimited          # bash / zsh
 *   echo core > /proc/sys/kernel/core_pattern   # or leave default
 *
 * Stack layout inside vuln() on x86-64 System V ABI:
 *
 *   RSP (low) ──► [ buf[0] … buf[63] ]   64 bytes   ← strcpy lands here
 *                 [ saved RBP         ]    8 bytes
 *                 [ return address    ]    8 bytes   ← overflow target
 *
 * Distance from start of buf to return address = 72 bytes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── target function ──────────────────────────────────────────────────────── */

/*
 * win() is the redirect target.  Its address is printed by main() so the
 * exploit script can parse it even when ASLR is off but the binary is
 * position-independent (the -no-pie flag removes that concern here).
 */
void __attribute__((noinline)) win(void)
{
    puts("[+] win() reached — return address overwrite succeeded!");
    fflush(stdout);
    _exit(0);
}

/* ── vulnerable function ──────────────────────────────────────────────────── */

/*
 * strcpy has no length limit, so any input longer than 63 bytes overflows
 * buf and walks up the stack, clobbering saved RBP at +64 and the return
 * address at +72.
 */
static void __attribute__((noinline)) vuln(const char *arg)
{
    char buf[64];
    strcpy(buf, arg);           /* ← intentional overflow; never do this */
    printf("[*] buf = %.64s\n", buf);
}

/* ── entry point ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input>\n", argv[0]);
        return 1;
    }

    /*
     * Print the address of win() so the exploit can read it from stdout
     * rather than hard-coding it.  With -no-pie the address is fixed across
     * runs, but reading it here keeps the technique general.
     */
    printf("[*] win() @ %p\n", (void *)win);
    fflush(stdout);

    vuln(argv[1]);

    puts("[*] returned normally (no overflow)");
    return 0;
}
