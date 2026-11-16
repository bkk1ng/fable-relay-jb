// KernelVmAccess.c - WDM driver for arbitrary virtual memory read/write
// Uses MmCopyVirtualMemory to copy between a caller-chosen process and a user buffer.
// Build with Windows Driver Kit (WDK) 10 or later.

#include <ntddk.h>

// ------------------------------------------------------------------
// IOCTL definitions
// ------------------------------------------------------------------
#define FILE_DEVICE_VMACCESS  0x8000

#define IOCTL_VM_READ   CTL_CODE(FILE_DEVICE_VMACCESS, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VM_WRITE  CTL_CODE(FILE_DEVICE_VMACCESS, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ------------------------------------------------------------------
// Request structures (buffered I/O)
// ------------------------------------------------------------------
typedef struct _VM_READ_REQUEST {
    ULONG_PTR ProcessId;
    ULONG_PTR Address;
    SIZE_T    Size;
} VM_READ_REQUEST, *PVM_READ_REQUEST;

typedef struct _VM_WRITE_REQUEST {
    ULONG_PTR ProcessId;
    ULONG_PTR Address;
    SIZE_T    Size;
    // Followed by Size bytes of data to write
} VM_WRITE_REQUEST, *PVM_WRITE_REQUEST;

// ------------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------------
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD DriverUnload;   // Actually WDM uses DRIVER_UNLOAD function type
DRIVER_UNLOAD DriverUnloadRoutine;

__drv_dispatchType(IRP_MJ_CREATE)
__drv_dispatchType(IRP_MJ_CLOSE)
__drv_dispatchType(IRP_MJ_CLEANUP)
DRIVER_DISPATCH DispatchCreateClose;

__drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH DispatchDeviceControl;

// ------------------------------------------------------------------
// DriverEntry
// ------------------------------------------------------------------
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS        status;
    PDEVICE_OBJECT  deviceObject = NULL;
    UNICODE_STRING  deviceName, symLinkName;

    RtlInitUnicodeString(&deviceName, L"\\Device\\KernelVmAccess");
    RtlInitUnicodeString(&symLinkName, L"\\DosDevices\\KernelVmAccess");

    // Create the device object
    status = IoCreateDevice(
        DriverObject,
        0,                      // DeviceExtensionSize
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &deviceObject
        );
    if (!NT_SUCCESS(status))
        return status;

    // Use buffered I/O to avoid manual probing
    deviceObject->Flags |= DO_BUFFERED_IO;

    // Create symbolic link for user-mode access
    status = IoCreateSymbolicLink(&symLinkName, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(deviceObject);
        return status;
    }

    // Set up dispatch functions
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    DriverObject->DriverUnload                         = DriverUnloadRoutine;

    DbgPrint("KernelVmAccess: Driver loaded successfully.\n");
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------------
// Unload routine
// ------------------------------------------------------------------
VOID
DriverUnloadRoutine(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNICODE_STRING symLinkName;

    RtlInitUnicodeString(&symLinkName, L"\\DosDevices\\KernelVmAccess");
    IoDeleteSymbolicLink(&symLinkName);

    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    DbgPrint("KernelVmAccess: Driver unloaded.\n");
}

// ------------------------------------------------------------------
// Create / Close / Cleanup handler
// ------------------------------------------------------------------
NTSTATUS
DispatchCreateClose(
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

// ------------------------------------------------------------------
// IOCTL handler
// ------------------------------------------------------------------
NTSTATUS
DispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION  stack   = IoGetCurrentIrpStackLocation(Irp);
    ULONG               ioctl   = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS            status  = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR           info    = 0;

    switch (ioctl)
    {
    case IOCTL_VM_READ:
    {
        PVM_READ_REQUEST pRead = (PVM_READ_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        ULONG inputSize  = stack->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outputSize = stack->Parameters.DeviceIoControl.OutputBufferLength;

        if (inputSize < sizeof(VM_READ_REQUEST) || outputSize < pRead->Size)
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PEPROCESS targetProcess = NULL;
        status = PsLookupProcessByProcessId((HANDLE)pRead->ProcessId, &targetProcess);
        if (!NT_SUCCESS(status))
            break;

        SIZE_T bytesCopied = 0;
        __try
        {
            // Copy from target process -> current process (user buffer via SystemBuffer)
            status = MmCopyVirtualMemory(
                targetProcess,
                (PVOID)pRead->Address,
                PsGetCurrentProcess(),
                Irp->AssociatedIrp.SystemBuffer,   // write the data here
                pRead->Size,
                KernelMode,
                &bytesCopied
            );
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = GetExceptionCode();
            bytesCopied = 0;
        }

        ObDereferenceObject(targetProcess);
        info = bytesCopied;
        break;
    }

    case IOCTL_VM_WRITE:
    {
        PVM_WRITE_REQUEST pWrite = (PVM_WRITE_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        ULONG inputSize = stack->Parameters.DeviceIoControl.InputBufferLength;

        if (inputSize < sizeof(VM_WRITE_REQUEST) + pWrite->Size)
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PEPROCESS targetProcess = NULL;
        status = PsLookupProcessByProcessId((HANDLE)pWrite->ProcessId, &targetProcess);
        if (!NT_SUCCESS(status))
            break;

        SIZE_T bytesCopied = 0;
        PVOID  sourceData  = (PUCHAR)Irp->AssociatedIrp.SystemBuffer + sizeof(VM_WRITE_REQUEST);

        __try
        {
            // Copy from current process (sourceData) -> target process address
            status = MmCopyVirtualMemory(
                PsGetCurrentProcess(),
                sourceData,
                targetProcess,
                (PVOID)pWrite->Address,
                pWrite->Size,
                KernelMode,
                &bytesCopied
            );
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = GetExceptionCode();
            bytesCopied = 0;
        }

        ObDereferenceObject(targetProcess);
        info = bytesCopied;
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        info   = 0;
        break;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
