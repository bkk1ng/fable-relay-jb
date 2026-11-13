/*
 * kmem.c - Windows kernel driver (WDM) for virtual memory read/write
 *
 * Exposes a device object that accepts two IOCTLs:
 *   IOCTL_KMEM_READ  - copies memory from a target process VA into the caller's buffer
 *   IOCTL_KMEM_WRITE - copies the caller's buffer into a target process VA
 *
 * Both operations use MmCopyVirtualMemory so they are IRQL-safe and handle
 * cross-process virtual addresses correctly.
 *
 * Build (kernel, x64):
 *   cl /kernel /W4 /WX /Zi /nologo /c kmem.c
 *   link /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /NODEFAULTLIB ...
 *
 * Smoke-test build (user-mode, define KMEM_USERMODE_SMOKE):
 *   cl /DKMEM_USERMODE_SMOKE /W4 /Zi /nologo kmem.c
 */

#include <ntddk.h>
#include <wdm.h>

/* -------------------------------------------------------------------------
 * IOCTL codes
 * ------------------------------------------------------------------------- */
#define KMEM_DEVICE_TYPE    0x8000ul

#define IOCTL_KMEM_READ  CTL_CODE(KMEM_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KMEM_WRITE CTL_CODE(KMEM_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* -------------------------------------------------------------------------
 * Shared request structure (placed at the start of the IOCTL input buffer)
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct _KMEM_REQUEST {
    ULONG_PTR ProcessId;        /* Target process PID                        */
    ULONG_PTR RemoteAddress;    /* Virtual address in the target process      */
    SIZE_T    DataSize;         /* Bytes to read/write                        */
    /* For READ:  output data follows in the output buffer (METHOD_BUFFERED)  */
    /* For WRITE: data to write immediately follows this header in input buf   */
} KMEM_REQUEST, *PKMEM_REQUEST;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Device / symbolic-link names
 * ------------------------------------------------------------------------- */
#define KMEM_DEVICE_NAME  L"\\Device\\KMem"
#define KMEM_SYMLINK_NAME L"\\DosDevices\\KMem"

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */
DRIVER_UNLOAD   KMemUnload;
DRIVER_DISPATCH KMemCreate;
DRIVER_DISPATCH KMemClose;
DRIVER_DISPATCH KMemDeviceControl;

static NTSTATUS KMemCopyMemory(
    _In_  ULONG_PTR  Pid,
    _In_  ULONG_PTR  RemoteAddress,
    _In_  PVOID      LocalBuffer,
    _In_  SIZE_T     Size,
    _In_  BOOLEAN    Write);

/* -------------------------------------------------------------------------
 * Global state
 * ------------------------------------------------------------------------- */
static PDEVICE_OBJECT  g_DeviceObject  = NULL;
static UNICODE_STRING  g_SymlinkName;

/* =========================================================================
 * DriverEntry
 * ========================================================================= */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS       status;
    UNICODE_STRING deviceName;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&deviceName,  KMEM_DEVICE_NAME);
    RtlInitUnicodeString(&g_SymlinkName, KMEM_SYMLINK_NAME);

    /* Create the device object (buffered I/O so the I/O manager copies
     * user buffers; we never touch user VA directly).                        */
    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject);

    if (!NT_SUCCESS(status)) {
        KdPrint(("KMem: IoCreateDevice failed: 0x%08X\n", status));
        return status;
    }

    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Expose a symbolic link so user-mode code can open \\.\KMem            */
    status = IoCreateSymbolicLink(&g_SymlinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KMem: IoCreateSymbolicLink failed: 0x%08X\n", status));
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    /* Dispatch table                                                         */
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = KMemCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = KMemClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KMemDeviceControl;
    DriverObject->DriverUnload                          = KMemUnload;

    KdPrint(("KMem: driver loaded\n"));
    return STATUS_SUCCESS;
}

/* =========================================================================
 * KMemUnload
 * ========================================================================= */
VOID
KMemUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    IoDeleteSymbolicLink(&g_SymlinkName);

    if (g_DeviceObject) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }

    KdPrint(("KMem: driver unloaded\n"));
}

/* =========================================================================
 * KMemCreate / KMemClose  (trivial pass-through)
 * ========================================================================= */
NTSTATUS
KMemCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
KMemClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* =========================================================================
 * KMemDeviceControl  (IRP_MJ_DEVICE_CONTROL)
 * ========================================================================= */
NTSTATUS
KMemDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp)
{
    PIO_STACK_LOCATION  stack;
    NTSTATUS            status      = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR           information = 0;
    PVOID               sysBuffer;
    ULONG               inputLen, outputLen;
    PKMEM_REQUEST       req;
    SIZE_T              headerSize  = sizeof(KMEM_REQUEST);

    UNREFERENCED_PARAMETER(DeviceObject);

    stack     = IoGetCurrentIrpStackLocation(Irp);
    inputLen  = stack->Parameters.DeviceIoControl.InputBufferLength;
    outputLen = stack->Parameters.DeviceIoControl.OutputBufferLength;

    /* METHOD_BUFFERED: both input and output live in AssociatedIrp.SystemBuffer */
    sysBuffer = Irp->AssociatedIrp.SystemBuffer;

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {

    /* --------------------------------------------------------------------- */
    case IOCTL_KMEM_READ:
    /* --------------------------------------------------------------------- */
        /* Input  : KMEM_REQUEST header
         * Output : raw bytes read from the remote process VA                 */

        if (inputLen < (ULONG)headerSize || sysBuffer == NULL) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        req = (PKMEM_REQUEST)sysBuffer;

        if (outputLen == 0 || outputLen < (ULONG)req->DataSize) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (req->DataSize == 0) {
            status = STATUS_SUCCESS;
            break;
        }

        /* Use a separate local bounce buffer so we do not accidentally
         * overlap the header with the output data in the system buffer.      */
        {
            PVOID bounce = ExAllocatePoolWithTag(
                               NonPagedPoolNx,
                               req->DataSize,
                               'meKB');

            if (bounce == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            status = KMemCopyMemory(
                         req->ProcessId,
                         req->RemoteAddress,
                         bounce,
                         req->DataSize,
                         FALSE /* read */);

            if (NT_SUCCESS(status)) {
                /* Copy result into the output portion of the system buffer.  */
                RtlCopyMemory(sysBuffer, bounce, req->DataSize);
                information = req->DataSize;
            }

            ExFreePoolWithTag(bounce, 'meKB');
        }
        break;

    /* --------------------------------------------------------------------- */
    case IOCTL_KMEM_WRITE:
    /* --------------------------------------------------------------------- */
        /* Input  : KMEM_REQUEST header immediately followed by the data
         *          to write into the target process
         * Output : nothing                                                   */

        if (inputLen < (ULONG)headerSize || sysBuffer == NULL) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        req = (PKMEM_REQUEST)sysBuffer;

        if (req->DataSize == 0) {
            status = STATUS_SUCCESS;
            break;
        }

        if (inputLen < (ULONG)(headerSize + req->DataSize)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        /* Data immediately follows the header inside the same input buffer.  */
        status = KMemCopyMemory(
                     req->ProcessId,
                     req->RemoteAddress,
                     (PUCHAR)sysBuffer + headerSize,
                     req->DataSize,
                     TRUE /* write */);

        information = 0;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* =========================================================================
 * KMemCopyMemory
 *
 * Opens the target process by PID, calls MmCopyVirtualMemory, and closes
 * the handle.  MmCopyVirtualMemory is callable at IRQL <= APC_LEVEL, which
 * is guaranteed here because we are in the IRP dispatch path.
 * ========================================================================= */
static NTSTATUS
KMemCopyMemory(
    _In_  ULONG_PTR  Pid,
    _In_  ULONG_PTR  RemoteAddress,
    _In_  PVOID      LocalBuffer,
    _In_  SIZE_T     Size,
    _In_  BOOLEAN    Write)
{
    NTSTATUS        status;
    PEPROCESS       targetProcess = NULL;
    PEPROCESS       localProcess  = PsGetCurrentProcess();
    SIZE_T          bytesCopied   = 0;
    CLIENT_ID       cid;
    OBJECT_ATTRIBUTES oa;

    /* Look up the EPROCESS for the supplied PID.                            */
    status = PsLookupProcessByProcessId((HANDLE)Pid, &targetProcess);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KMem: PsLookupProcessByProcessId(%llu) failed: 0x%08X\n",
                 (ULONGLONG)Pid, status));
        return status;
    }

    /*
     * MmCopyVirtualMemory(
     *   SourceProcess, SourceAddress,
     *   TargetProcess, TargetAddress,
     *   BufferSize,
     *   PreviousMode,
     *   ReturnSize)
     *
     * For a READ  the remote process is the source  and local is the target.
     * For a WRITE the local  process is the source  and remote is the target.
     */
    if (!Write) {
        status = MmCopyVirtualMemory(
                     targetProcess,
                     (PVOID)RemoteAddress,
                     localProcess,
                     LocalBuffer,
                     Size,
                     KernelMode,
                     &bytesCopied);
    } else {
        status = MmCopyVirtualMemory(
                     localProcess,
                     LocalBuffer,
                     targetProcess,
                     (PVOID)RemoteAddress,
                     Size,
                     KernelMode,
                     &bytesCopied);
    }

    if (!NT_SUCCESS(status)) {
        KdPrint(("KMem: MmCopyVirtualMemory failed: 0x%08X (copied %Iu / %Iu)\n",
                 status, bytesCopied, Size));
    }

    /* Release the reference obtained by PsLookupProcessByProcessId.         */
    ObDereferenceObject(targetProcess);

    UNREFERENCED_PARAMETER(cid);
    UNREFERENCED_PARAMETER(oa);

    return status;
}

/* =========================================================================
 * Smoke-test entry point (user-mode build, not linked into the driver)
 * ========================================================================= */
#ifdef KMEM_USERMODE_SMOKE
#include <stdio.h>

int main(void)
{
    /*
     * This block only verifies that the structures and constants compile
     * in a user-mode context.  No actual driver I/O is performed here.
     */
    KMEM_REQUEST req = {0};
    req.ProcessId     = 1234;
    req.RemoteAddress = 0x0000000140000000ULL;
    req.DataSize      = 128;

    printf("IOCTL_KMEM_READ  = 0x%08X\n", (unsigned)IOCTL_KMEM_READ);
    printf("IOCTL_KMEM_WRITE = 0x%08X\n", (unsigned)IOCTL_KMEM_WRITE);
    printf("sizeof(KMEM_REQUEST) = %u\n", (unsigned)sizeof(KMEM_REQUEST));
    printf("PID=%llu  addr=0x%016llX  size=%llu\n",
           (unsigned long long)req.ProcessId,
           (unsigned long long)req.RemoteAddress,
           (unsigned long long)req.DataSize);
    return 0;
}
#endif /* KMEM_USERMODE_SMOKE */
