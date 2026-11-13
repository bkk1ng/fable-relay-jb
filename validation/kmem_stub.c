// kmem_stub.c — user-mode backing for validation/inc/ntddk.h.
//
// The real transfer path is /proc/<pid>/mem, so the driver's read and write IOCTLs move actual bytes
// between this process and a live target process when the test runs.
#define _GNU_SOURCE
#include <ntddk.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static struct _EPROCESS_FAKE g_current = {0, -1};

PEPROCESS PsGetCurrentProcess(void) {
    g_current.pid = (ULONG)getpid();
    return &g_current;
}

NTSTATUS PsLookupProcessByProcessId(HANDLE pid, PEPROCESS* out) {
    if (!out || (ULONG_PTR)pid == 0) return STATUS_INVALID_PARAMETER;
    char path[64];
    snprintf(path, sizeof path, "/proc/%llu/mem", (unsigned long long)(ULONG_PTR)pid);
    int fd = open(path, O_RDWR);
    if (fd < 0) return STATUS_INVALID_PARAMETER;
    PEPROCESS p = calloc(1, sizeof *p);
    p->pid = (ULONG)(ULONG_PTR)pid;
    p->memfd = fd;
    *out = p;
    return STATUS_SUCCESS;
}

VOID ObReferenceObject(PEPROCESS p) { (void)p; }
VOID ObDereferenceObject(PEPROCESS p) { if (p && p->memfd >= 0 && p != &g_current) close(p->memfd); free(p); }

NTSTATUS MmCopyVirtualMemory(PEPROCESS src_proc, PVOID src_addr, PEPROCESS dst_proc,
                             PVOID dst_addr, SIZE_T size, int access_mode, SIZE_T* copied) {
    (void)access_mode;
    if (copied) *copied = 0;
    if (!src_proc || !dst_proc || !size) return STATUS_INVALID_PARAMETER;

    static char scratch[1 << 20];
    SIZE_T done = 0;
    while (done < size) {
        SIZE_T chunk = size - done;
        if (chunk > sizeof scratch) chunk = sizeof scratch;
        // pull from the source address space
        if (src_proc == &g_current) {
            memcpy(scratch, (char*)src_addr + done, chunk);
        } else {
            ssize_t n = pread(src_proc->memfd, scratch, chunk,
                              (off_t)((uintptr_t)src_addr + done));
            if (n != (ssize_t)chunk) return STATUS_INVALID_PARAMETER;
        }
        // push into the destination address space
        if (dst_proc == &g_current) {
            memcpy((char*)dst_addr + done, scratch, chunk);
        } else {
            ssize_t n = pwrite(dst_proc->memfd, scratch, chunk,
                               (off_t)((uintptr_t)dst_addr + done));
            if (n != (ssize_t)chunk) return STATUS_INVALID_PARAMETER;
        }
        done += chunk;
    }
    if (copied) *copied = size;
    return STATUS_SUCCESS;
}

// device plumbing: bookkeeping only, nothing to allocate in user mode
NTSTATUS IoCreateDevice(PDRIVER_OBJECT d, ULONG ext, PUNICODE_STRING name, ULONG type, ULONG flags,
                        int exclusive, PDEVICE_OBJECT* out) {
    (void)d; (void)ext; (void)name; (void)type; (void)flags; (void)exclusive;
    static DEVICE_OBJECT dev;
    if (out) *out = &dev;
    return STATUS_SUCCESS;
}
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING a, PUNICODE_STRING b) { (void)a; (void)b; return STATUS_SUCCESS; }
VOID     IoDeleteDevice(PDEVICE_OBJECT d) { (void)d; }
NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING s) { (void)s; return STATUS_SUCCESS; }
VOID     IoCompleteRequest(PIRP irp, int inc) { (void)irp; (void)inc; }
PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return irp->CurrentStackLocation; }
VOID     RtlCopyMemory(void* dst, const void* src, SIZE_T n) { memcpy(dst, src, n); }
VOID     RtlInitUnicodeString(PUNICODE_STRING u, const wchar_t* src) {
    u->Buffer = (wchar_t*)src;
    u->Length = (uint16_t)(wcslen(src) * sizeof(wchar_t));
    u->MaximumLength = u->Length;
}
