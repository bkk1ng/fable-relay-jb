/*
 * vuln.c — classic stack buffer overflow demo (x86-64 Linux)
 *
 * Intentionally vulnerable. For security education only.
 *
 * Build (disable mitigations so the overflow is straightforward):
 *   gcc -O0 -g \
 *       -fno-stack-protector \
 *       -no-pie \
 *       -z execstack \
 *       -o vuln vuln.c
 *
 * Stack layout inside vuln() with -O0 -no-pie -fno-stack-protector:
 *
 *   higher addresses
 *   ┌─────────────────┐
 *   │  return address │  ← [rbp + 8]   ← overwrite target
 *   ├─────────────────┤
 *   │   saved RBP     │  ← [rbp]       (8 bytes)
 *   ├─────────────────┤
 *   │   buf[64]       │  ← [rbp - 64]  ← read() writes here
 *   └─────────────────┘
 *   lower addresses
 *
 * Offset from &buf[0] to return address: 64 + 8 = 72 bytes.
 *
 * win() is printed on startup so exploit.py can read it and build
 * the payload without needing a PIE-aware leak gadget.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/* Target function — redirect here to "win". */
void win(void)
{
    puts("[+] win() reached — return address successfully overwritten!");
    fflush(stdout);
    _exit(0);
}

/*
 * Vulnerable function.
 * read(2) is used instead of strcpy/gets so the binary payload
 * (which contains null bytes from the 64-bit address) passes through
 * without truncation.  256 > 64 is the intentional bug.
 */
void vuln(void)
{
    char buf[64];

    /* BUG: reads up to 256 bytes into a 64-byte stack buffer */
    ssize_t n = read(STDIN_FILENO, buf, 256);
    if (n < 0) {
        perror("read");
        _exit(1);
    }
}

int main(void)
{
    /*
     * Print the runtime address of win() so the exploit script can
     * grab it even if ASLR is on (with -no-pie the text segment is
     * fixed, but printing it makes the script self-contained).
     */
    printf("win() @ %p\n", (void *)win);
    fflush(stdout);

    vuln();

    puts("returned normally (no overflow occurred)");
    return 0;
}
