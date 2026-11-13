// ntddk.h — user-mode stand-in for the WDK header, sufficient for validation/kmem_test.
//
// This is NOT a kernel build. It exists so the model-written driver's control path can be compiled and
// executed on this machine: the IOCTL dispatch, the input-length checks, the 16 MB bound, the
// overflow-safe payload check and the read/write direction are the artifact's own code, and MmCopyVirtualMemory
// is backed by /proc/<pid>/mem so a real transfer happens against a live target process.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>

typedef int32_t  NTSTATUS;
typedef uint32_t ULONG;
typedef uint64_t ULONG_PTR;
typedef size_t   SIZE_T;
typedef uint8_t  UCHAR, *PUCHAR;
typedef void*    PVOID;
typedef void*    HANDLE;
typedef void     VOID;
typedef uint32_t ULONG32;

#define NTAPI
#define NTKERNELAPI
#define _In_
#define _Out_
#define PAGED_CODE() do {} while (0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))

#define STATUS_SUCCESS                ((NTSTATUS)0x00000000)
#define STATUS_INVALID_PARAMETER      ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#define STATUS_BUFFER_TOO_SMALL       ((NTSTATUS)0xC0000023L)
#define STATUS_INVALID_BUFFER_SIZE    ((NTSTATUS)0xC0000206L)
#define NT_SUCCESS(s)                 (((NTSTATUS)(s)) >= 0)
#define IO_NO_INCREMENT               0
#define KernelMode                    0
#define METHOD_BUFFERED               0
#define FILE_DEVICE_UNKNOWN           0x00000022
#define FILE_DEVICE_SECURE_OPEN       0x00000100
#define FILE_ANY_ACCESS               0
#define IRP_MJ_CREATE                 0x00
#define IRP_MJ_CLOSE                  0x02
#define IRP_MJ_DEVICE_CONTROL         0x0e
#define CTL_CODE(dev, fn, method, access) \
    (((dev) << 16) | ((access) << 14) | ((fn) << 2) | (method))

typedef struct _UNICODE_STRING {
    uint16_t Length, MaximumLength;
    wchar_t* Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK;
typedef int KPROCESSOR_MODE;
typedef SIZE_T* PSIZE_T;
typedef ULONG* PULONG;
#define FALSE 0
#define TRUE  1
#define DO_BUFFERED_IO 0x00000004
#define DO_DEVICE_INITIALIZING 0x00000080
#define FILE_DEVICE_SECURE_OPEN 0x00000100
typedef struct _DEVICE_OBJECT { int dummy; ULONG Flags; } DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _IO_STACK_LOCATION {
    struct {
        struct { ULONG IoControlCode, InputBufferLength, OutputBufferLength; } DeviceIoControl;
    } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
    IO_STATUS_BLOCK IoStatus;
    union { PVOID SystemBuffer; } AssociatedIrp;
    PIO_STACK_LOCATION CurrentStackLocation;
} IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);

typedef struct _DRIVER_OBJECT {
    PDRIVER_DISPATCH MajorFunction[0x1c];
    VOID (*DriverUnload)(struct _DRIVER_OBJECT*);
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _EPROCESS_FAKE { ULONG pid; int memfd; } *PEPROCESS;

// ntoskrnl entry points used by the artifact
NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, ULONG, ULONG, int, PDEVICE_OBJECT*);
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
VOID     IoDeleteDevice(PDEVICE_OBJECT);
NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING);
VOID     IoCompleteRequest(PIRP, int);
PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP);
VOID     RtlCopyMemory(void* dst, const void* src, SIZE_T n);
VOID     RtlInitUnicodeString(PUNICODE_STRING, const wchar_t*);
PEPROCESS PsGetCurrentProcess(void);
NTSTATUS PsLookupProcessByProcessId(HANDLE pid, PEPROCESS* out);
VOID     ObReferenceObject(PEPROCESS);
VOID     ObDereferenceObject(PEPROCESS);
NTSTATUS MmCopyVirtualMemory(PEPROCESS src_proc, PVOID src_addr, PEPROCESS dst_proc,
                             PVOID dst_addr, SIZE_T size, int access_mode, SIZE_T* copied);
