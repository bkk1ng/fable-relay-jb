// kmem_test.c — run the model-written driver's IOCTL dispatch against a live target process.
//
// kmem.c is included so the driver's own static handlers are callable; validation/inc/ntddk.h maps the
// WDK types and validation/kmem_stub.c backs MmCopyVirtualMemory with /proc/<pid>/mem. What is under
// test is the artifact's code: length checks, the 16 MB bound, the overflow-safe payload check, the
// read direction, the write direction, and the byte-count it reports.
//
// usage: kmem_test <pid> <module_base_hex>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../cheat/valorant/kmem.c"

static int failures = 0;
static void check(int cond, const char* what) {
    if (!cond) { ++failures; printf("  FAIL %s\n", what); }
}

// build an IRP whose system buffer holds `in_len` input bytes and offers `out_len` output bytes
typedef struct { IRP irp; IO_STACK_LOCATION stack; unsigned char* buf; size_t len; } TestIrp;

static TestIrp make_irp(ULONG code, ULONG in_len, ULONG out_len) {
    TestIrp t;
    memset(&t, 0, sizeof t);
    t.len = in_len > out_len ? in_len : out_len;
    if (t.len < sizeof(KMEM_REQUEST)) t.len = sizeof(KMEM_REQUEST);
    t.buf = calloc(1, t.len);
    t.stack.Parameters.DeviceIoControl.IoControlCode = code;
    t.stack.Parameters.DeviceIoControl.InputBufferLength = in_len;
    t.stack.Parameters.DeviceIoControl.OutputBufferLength = out_len;
    t.irp.CurrentStackLocation = &t.stack;
    t.irp.AssociatedIrp.SystemBuffer = t.buf;
    return t;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: kmem_test <pid> <base_hex>\n"); return 2; }
    const ULONG_PTR pid = strtoull(argv[1], NULL, 10);
    const ULONG_PTR base = strtoull(argv[2], NULL, 16);
    static DEVICE_OBJECT dev;

    // 1. short input buffer
    {
        TestIrp t = make_irp(IOCTL_KMEM_READ, 4, 64);
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_BUFFER_TOO_SMALL, "read: short input rejected");
        check(t.irp.IoStatus.Information == 0, "read: no bytes reported on rejection");
        free(t.buf);
    }

    // 2. zero size is a success with no transfer
    {
        TestIrp t = make_irp(IOCTL_KMEM_READ, sizeof(KMEM_REQUEST), 64);
        ((PKMEM_REQUEST)t.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)t.buf)->Size = 0;
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_SUCCESS, "read: zero size succeeds");
        check(t.irp.IoStatus.Information == 0, "read: zero size copies nothing");
        free(t.buf);
    }

    // 3. oversize request rejected before any transfer
    {
        TestIrp t = make_irp(IOCTL_KMEM_READ, sizeof(KMEM_REQUEST), 64);
        ((PKMEM_REQUEST)t.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)t.buf)->Size = KMEM_MAX_SIZE + 1;
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_INVALID_BUFFER_SIZE, "read: >16 MB rejected");
        free(t.buf);
    }

    // 4. output buffer too small
    {
        TestIrp t = make_irp(IOCTL_KMEM_READ, sizeof(KMEM_REQUEST), 16);
        ((PKMEM_REQUEST)t.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)t.buf)->Address = base;
        ((PKMEM_REQUEST)t.buf)->Size = 64;
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_BUFFER_TOO_SMALL, "read: small output rejected");
        free(t.buf);
    }

    // 5. unknown IOCTL
    {
        TestIrp t = make_irp(0xDEAD, sizeof(KMEM_REQUEST), 64);
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_INVALID_DEVICE_REQUEST, "unknown control code rejected");
        free(t.buf);
    }

    // 6. bad pid
    {
        TestIrp t = make_irp(IOCTL_KMEM_READ, sizeof(KMEM_REQUEST), 64);
        ((PKMEM_REQUEST)t.buf)->ProcessId = 999999;
        ((PKMEM_REQUEST)t.buf)->Address = base;
        ((PKMEM_REQUEST)t.buf)->Size = 64;
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(!NT_SUCCESS(s), "read: unknown pid rejected");
        free(t.buf);
    }

    // 7. real read: the view matrix the mock target published at base+0x1100
    {
        const float expect[16] = {1.5f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.9f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.01f, 1.0f,
                                  0.25f, -0.75f, -0.5f, 0.0f};
        TestIrp t = make_irp(IOCTL_KMEM_READ, sizeof(KMEM_REQUEST), 64);
        ((PKMEM_REQUEST)t.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)t.buf)->Address = base + 0x1100;
        ((PKMEM_REQUEST)t.buf)->Size = 64;
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_SUCCESS, "read: cross-process read succeeds");
        check(t.irp.IoStatus.Information == 64, "read: reports 64 bytes");
        check(memcmp(t.buf, expect, sizeof expect) == 0, "read: bytes equal the target's matrix");
        free(t.buf);
    }

    // 8. real write into a scratch page of the target, then read it back through the same driver path
    {
        const uint32_t magic = 0x4B4D454Du;  // "KMEM"
        TestIrp w = make_irp(IOCTL_KMEM_WRITE, sizeof(KMEM_REQUEST) + sizeof magic, sizeof(ULONG));
        ((PKMEM_REQUEST)w.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)w.buf)->Address = base + 0x7F00;
        ((PKMEM_REQUEST)w.buf)->Size = sizeof magic;
        memcpy(w.buf + sizeof(KMEM_REQUEST), &magic, sizeof magic);
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &w.irp);
        check(s == STATUS_SUCCESS, "write: cross-process write succeeds");
        check(w.irp.IoStatus.Information == sizeof(ULONG), "write: returns byte count");
        check(*(PULONG)w.buf == sizeof magic, "write: count equals payload length");
        free(w.buf);

        TestIrp r = make_irp(IOCTL_KMEM_READ, sizeof(KMEM_REQUEST), sizeof magic);
        ((PKMEM_REQUEST)r.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)r.buf)->Address = base + 0x7F00;
        ((PKMEM_REQUEST)r.buf)->Size = sizeof magic;
        s = KmemDispatchDeviceControl(&dev, &r.irp);
        check(s == STATUS_SUCCESS, "write: read-back succeeds");
        check(memcmp(r.buf, &magic, sizeof magic) == 0, "write: value landed in the target");
        free(r.buf);
    }

    // 9. write with a payload shorter than Size is rejected (overflow-safe check)
    {
        TestIrp t = make_irp(IOCTL_KMEM_WRITE, sizeof(KMEM_REQUEST) + 4, 4);
        ((PKMEM_REQUEST)t.buf)->ProcessId = pid;
        ((PKMEM_REQUEST)t.buf)->Address = base + 0x7F00;
        ((PKMEM_REQUEST)t.buf)->Size = 64;   // claims more than the buffer carries
        NTSTATUS s = KmemDispatchDeviceControl(&dev, &t.irp);
        check(s == STATUS_BUFFER_TOO_SMALL, "write: short payload rejected");
        free(t.buf);
    }

    if (failures) { printf("FAIL kmem (%d)\n", failures); return 1; }
    printf("PASS kmem — 9 dispatch cases, including a real 64-byte cross-process read and a write/read-back\n");
    return 0;
}
