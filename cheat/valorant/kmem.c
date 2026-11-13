/*
 * kmem.c — WDM kernel driver: cross-process virtual memory read/write.
 *
 * IOCTL_KMEM_READ
 *   In:  KMEM_REQUEST { ProcessId, Address, Size }
 *   Out: <Size bytes copied from target VA into caller buffer>
 *
 * IOCTL_KMEM_WRITE
 *   In:  KMEM_REQUEST { ProcessId, Address, Size } immediately followed
 *        by <Size bytes of data to write>
 *   Out: ULONG — bytes actually written
 *
 * ProcessId == 0 targets the calling process.
 *
 * Build (WDK / MSBuild):
 *   Add kmem.c to a WDM Kernel Mode Driver (.sys) project.
 *   Link: ntoskrnl.lib (implicit via WDK).
 *
 * Install (elevated):
 *   sc create KMem type= kernel binPath= "C:\path\to\kmem.sys"
 *   sc start  KMem
 *
 * Stop / remove:
 *   sc stop   KMem
 *   sc delete KMem
 */

#include <ntddk.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define DEVICE_NAME   L"\\Device\\KMem"
#define SYMLINK_NAME  L"\\DosDevices\\KMem"

/*
 * CTL_CODE layout:
 *   DeviceType  = FILE_DEVICE_UNKNOWN  (0x22)
 *   Function    = 0x800 / 0x801        (user-defined range starts 0x800)
 *   Method      = METHOD_BUFFERED      (I/O manager allocates system buffer)
 *   Access      = FILE_ANY_ACCESS
 */
#define IOCTL_KMEM_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KMEM_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Maximum single-transfer size (16 MB). Guards against absurd allocations. */
#define KMEM_MAX_SIZE (16u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* Public IOCTL structures (share with user-mode callers)              */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct _KMEM_REQUEST {
    ULONG_PTR ProcessId;  /* target PID; 0 = calling process            */
    ULONG_PTR Address;    /* virtual address in the target process       */
    ULONG     Size;       /* byte count to transfer                      */
} KMEM_REQUEST, *PKMEM_REQUEST;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Undocumented ntoskrnl export — stable from XP through Windows 11.   */
/* Performs a guarded copy across two process address spaces.          */
/* ------------------------------------------------------------------ */
NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(
    _In_  PEPROCESS       SourceProcess,
    _In_  PVOID           SourceAddress,
    _In_  PEPROCESS       TargetProcess,
    _Out_ PVOID           TargetAddress,
    _In_  SIZE_T          BufferSize,
    _In_  KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T         ReturnSize
);

/* ------------------------------------------------------------------ */
/* Driver globals                                                       */
/* ------------------------------------------------------------------ */

static UNICODE_STRING  g_DeviceName;
static UNICODE_STRING  g_SymlinkName;
static PDEVICE_OBJECT  g_DeviceObject = NULL;

/* ------------------------------------------------------------------ */
/* Internal helper: reference a process by PID                         */
/* ------------------------------------------------------------------ */

static NTSTATUS
KmemLookupProcess(
    _In_  ULONG_PTR  Pid,
    _Out_ PEPROCESS *OutProcess
)
{
    if (Pid == 0) {
        /* Caller wants the current process; take an extra reference so
           callers can always call ObDereferenceObject unconditionally. */
        *OutProcess = PsGetCurrentProcess();
        ObReferenceObject(*OutProcess);
        return STATUS_SUCCESS;
    }
    return PsLookupProcessByProcessId((HANDLE)Pid, OutProcess);
}

/* ------------------------------------------------------------------ */
/* IRP_MJ_CREATE / IRP_MJ_CLOSE                                        */
/* ------------------------------------------------------------------ */

static NTSTATUS
KmemDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* IRP_MJ_DEVICE_CONTROL                                               */
/* ------------------------------------------------------------------ */

static NTSTATUS
KmemDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG  code   = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG  inLen  = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG  outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;

    /*
     * With METHOD_BUFFERED the I/O manager allocates a single kernel
     * buffer of max(InputBufferLength, OutputBufferLength) bytes, copies
     * the caller's input into it before dispatch, and copies the first
     * Irp->IoStatus.Information bytes back to the caller's output buffer
     * on completion.
     */
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;

    NTSTATUS  status = STATUS_INVALID_PARAMETER;
    ULONG_PTR info   = 0;

    switch (code) {

    /* ----------------------------------------------------------------
     * IOCTL_KMEM_READ
     *   Copies <req.Size> bytes from <req.Address> in <req.ProcessId>
     *   into the caller's output buffer.
     * -------------------------------------------------------------- */
    case IOCTL_KMEM_READ: {

        if (inLen < sizeof(KMEM_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        KMEM_REQUEST req;
        RtlCopyMemory(&req, buf, sizeof(KMEM_REQUEST));

        if (req.Size == 0) {
            status = STATUS_SUCCESS;
            break;
        }
        if (req.Size > KMEM_MAX_SIZE) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        if (outLen < req.Size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PEPROCESS target = NULL;
        status = KmemLookupProcess(req.ProcessId, &target);
        if (!NT_SUCCESS(status))
            break;

        SIZE_T copied = 0;
        status = MmCopyVirtualMemory(
            target,              /* source process            */
            (PVOID)req.Address,  /* source VA                 */
            PsGetCurrentProcess(), /* destination: this driver */
            buf,                 /* destination: system buf   */
            (SIZE_T)req.Size,
            KernelMode,
            &copied
        );

        ObDereferenceObject(target);

        if (NT_SUCCESS(status))
            info = (ULONG_PTR)copied;

        break;
    }

    /* ----------------------------------------------------------------
     * IOCTL_KMEM_WRITE
     *   Input buffer layout:
     *     [0 .. sizeof(KMEM_REQUEST)-1]  : KMEM_REQUEST header
     *     [sizeof(KMEM_REQUEST) .. end]  : payload bytes to write
     *
     *   Copies <req.Size> payload bytes to <req.Address> in
     *   <req.ProcessId>.  Returns a ULONG byte count in the output buf.
     * -------------------------------------------------------------- */
    case IOCTL_KMEM_WRITE: {

        if (inLen < sizeof(KMEM_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        KMEM_REQUEST req;
        RtlCopyMemory(&req, buf, sizeof(KMEM_REQUEST));

        if (req.Size == 0) {
            status = STATUS_SUCCESS;
            break;
        }
        if (req.Size > KMEM_MAX_SIZE) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        /*
         * Overflow-safe size check: we already know inLen >= sizeof header,
         * so inLen - sizeof(header) cannot underflow.
         */
        if (req.Size > inLen - (ULONG)sizeof(KMEM_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PVOID payload = (PUCHAR)buf + sizeof(KMEM_REQUEST);

        PEPROCESS target = NULL;
        status = KmemLookupProcess(req.ProcessId, &target);
        if (!NT_SUCCESS(status))
            break;

        SIZE_T copied = 0;
        status = MmCopyVirtualMemory(
            PsGetCurrentProcess(), /* source: this driver       */
            payload,               /* source: payload in sysbuf */
            target,                /* destination process       */
            (PVOID)req.Address,    /* destination VA            */
            (SIZE_T)req.Size,
            KernelMode,
            &copied
        );

        ObDereferenceObject(target);

        if (NT_SUCCESS(status) && outLen >= sizeof(ULONG)) {
            *(PULONG)buf = (ULONG)copied;
            info = sizeof(ULONG);
        }

        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ------------------------------------------------------------------ */
/* DriverUnload                                                         */
/* ------------------------------------------------------------------ */

static VOID
KmemUnload(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);

    IoDeleteSymbolicLink(&g_SymlinkName);

    if (g_DeviceObject != NULL) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* DriverEntry                                                          */
/* ------------------------------------------------------------------ */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&g_DeviceName,  DEVICE_NAME);
    RtlInitUnicodeString(&g_SymlinkName, SYMLINK_NAME);

    /* Create the device object. No device extension needed. */
    NTSTATUS status = IoCreateDevice(
        DriverObject,
        0,
        &g_DeviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,              /* not exclusive: multiple handles allowed */
        &g_DeviceObject
    );
    if (!NT_SUCCESS(status))
        return status;

    /* Expose a Win32-accessible path under \DosDevices. */
    status = IoCreateSymbolicLink(&g_SymlinkName, &g_DeviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    /*
     * DO_BUFFERED_IO: the I/O manager owns the system buffer for every
     * DeviceIoControl call, so our dispatch routines never touch user
     * memory directly.
     * Clear DO_DEVICE_INITIALIZING last so the device becomes accessible.
     */
    g_DeviceObject->Flags |=  DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DriverObject->DriverUnload                         = KmemUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = KmemDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = KmemDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KmemDispatchDeviceControl;

    return STATUS_SUCCESS;
}
