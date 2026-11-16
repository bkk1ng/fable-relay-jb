// kmemdrv.c - Windows WDM kernel driver
// Services user-mode IOCTLs for process memory r/w and module base retrieval
// Uses MmCopyVirtualMemory, PsLookupProcessByProcessId, etc.
// Build: cl /c /GS- /Gm- /kernel /Zl kmemdrv.c /link /DRIVER:WDM /ENTRY:DriverEntry /SUBSYSTEM:NATIVE kernel32.lib ntoskrnl.lib

#include <ntddk.h>
#include <wdm.h>

#define DEVICE_NAME      L"\\Device\\KmemDrv"
#define SYMLINK_NAME     L"\\DosDevices\\KmemDrv"
#define POOL_TAG         'mDmK'

#define IOCTL_KDRV_READ_MEM        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KDRV_WRITE_MEM       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KDRV_GET_MODULE_BASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KDRV_GET_MODULE_SIZE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _KDRV_MEM_REQUEST {
    ULONG  ProcessId;
    PVOID  SourceAddress;
    PVOID  TargetAddress;
    SIZE_T Length;
    SIZE_T BytesCopied;
} KDRV_MEM_REQUEST, *PKDRV_MEM_REQUEST;

typedef struct _KDRV_MODULE_REQUEST {
    ULONG  ProcessId;
    WCHAR  ModuleName[256];
    PVOID  BaseAddress;
    ULONG  ImageSize;
} KDRV_MODULE_REQUEST, *PKDRV_MODULE_REQUEST;

typedef struct _KDRV_DEVICE_EXTENSION {
    PDEVICE_OBJECT DeviceObject;
    UNICODE_STRING SymlinkName;
} KDRV_DEVICE_EXTENSION, *PKDRV_DEVICE_EXTENSION;

// Resolve a process by ID with reference
NTSTATUS
KdrvResolveProcess(
    _In_  ULONG ProcessId,
    _Out_ PEPROCESS *Process
)
{
    NTSTATUS status;
    PEPROCESS process = NULL;

    if (ProcessId == 0) {
        *Process = PsGetCurrentProcess();
        ObReferenceObject(*Process);
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)ProcessId, &process);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *Process = process;
    return STATUS_SUCCESS;
}

// Find a module base in a process by walking the PEB->Ldr
NTSTATUS
KdrvFindModuleBase(
    _In_  PEPROCESS TargetProcess,
    _In_  PWCHAR ModuleName,
    _Out_ PVOID *BaseAddress,
    _Out_ PULONG ImageSize
)
{
    NTSTATUS status;
    KAPC_STATE apcState;
    PPEB peb = NULL;
    PPEB_LDR_DATA ldr = NULL;
    PLIST_ENTRY listHead, listEntry;
    PLDR_DATA_TABLE_ENTRY ldrEntry;
    UNICODE_STRING moduleNameUs;
    UNICODE_STRING currentName;
    BOOLEAN found = FALSE;
    SIZE_T bytesRead = 0;
    PVOID base = NULL;
    ULONG size = 0;

    RtlInitUnicodeString(&moduleNameUs, ModuleName);

    // Attach to target process
    KeStackAttachProcess(TargetProcess, &apcState);

    __try {
        peb = PsGetProcessPeb(TargetProcess);
        if (!peb) {
            status = STATUS_NOT_FOUND;
            __leave;
        }

        // Read Ldr pointer from PEB
        ldr = peb->Ldr;
        if (!ldr) {
            status = STATUS_NOT_FOUND;
            __leave;
        }

        // Walk the module list
        listHead = &ldr->InLoadOrderModuleList;
        listEntry = listHead->Flink;

        while (listEntry != listHead) {
            ldrEntry = CONTAINING_RECORD(listEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

            // Compare module name (case-insensitive)
            __try {
                if (ldrEntry->BaseDllName.Buffer) {
                    ProbeForRead(ldrEntry->BaseDllName.Buffer, ldrEntry->BaseDllName.MaximumLength, 1);
                    currentName.Buffer = ldrEntry->BaseDllName.Buffer;
                    currentName.Length = ldrEntry->BaseDllName.Length;
                    currentName.MaximumLength = ldrEntry->BaseDllName.MaximumLength;

                    if (RtlEqualUnicodeString(&moduleNameUs, &currentName, TRUE)) {
                        base = ldrEntry->DllBase;
                        size = ldrEntry->SizeOfImage;
                        found = TRUE;
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Skip invalid entries
            }

            listEntry = listEntry->Flink;
            if (listEntry == NULL) break;
        }

        status = found ? STATUS_SUCCESS : STATUS_NOT_FOUND;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    KeUnstackDetachProcess(&apcState);

    if (found) {
        *BaseAddress = base;
        *ImageSize = size;
    }

    return status;
}

// Memory read using MmCopyVirtualMemory
NTSTATUS
KdrvReadMemory(
    _In_  PEPROCESS TargetProcess,
    _In_  PVOID SourceAddress,
    _Out_ PVOID TargetBuffer,
    _In_  SIZE_T Length
)
{
    SIZE_T bytesCopied;
    PVOID sourceAddr = SourceAddress;
    PVOID targetAddr = TargetBuffer;
    SIZE_T size = Length;

    return MmCopyVirtualMemory(
        TargetProcess,
        sourceAddr,
        PsGetCurrentProcess(),
        targetAddr,
        size,
        KernelMode,
        &bytesCopied
    );
}

// Memory write using MmCopyVirtualMemory
NTSTATUS
KdrvWriteMemory(
    _In_  PEPROCESS TargetProcess,
    _In_  PVOID TargetAddress,
    _In_  PVOID SourceBuffer,
    _In_  SIZE_T Length
)
{
    SIZE_T bytesCopied;
    PVOID sourceAddr = SourceBuffer;
    PVOID targetAddr = TargetAddress;
    SIZE_T size = Length;

    return MmCopyVirtualMemory(
        PsGetCurrentProcess(),
        sourceAddr,
        TargetProcess,
        targetAddr,
        size,
        KernelMode,
        &bytesCopied
    );
}

// IOCTL handler
NTSTATUS
KdrvDeviceControl(
    _In_  PDEVICE_OBJECT DeviceObject,
    _In_  PIRP Irp
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION irpStack;
    ULONG ioControlCode;
    PVOID inputBuffer;
    PVOID outputBuffer;
    ULONG inputBufferLength;
    ULONG outputBufferLength;
    ULONG bytesReturned = 0;
    PEPROCESS targetProcess = NULL;

    UNREFERENCED_PARAMETER(DeviceObject);

    irpStack = IoGetCurrentIrpStackLocation(Irp);
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

    inputBuffer = Irp->AssociatedIrp.SystemBuffer;
    outputBuffer = Irp->AssociatedIrp.SystemBuffer;
    inputBufferLength = irpStack->Parameters.DeviceIoControl.InputBufferLength;
    outputBufferLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (ioControlCode) {
    case IOCTL_KDRV_READ_MEM:
    {
        PKDRV_MEM_REQUEST request;
        PVOID kernelBuffer;

        if (inputBufferLength < sizeof(KDRV_MEM_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        request = (PKDRV_MEM_REQUEST)inputBuffer;
        if (!request->TargetAddress || !request->Length || !request->SourceAddress) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        status = KdrvResolveProcess(request->ProcessId, &targetProcess);
        if (!NT_SUCCESS(status)) break;

        kernelBuffer = ExAllocatePoolWithTag(NonPagedPool, request->Length, POOL_TAG);
        if (!kernelBuffer) {
            ObDereferenceObject(targetProcess);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(kernelBuffer, request->Length);

        status = KdrvReadMemory(targetProcess, request->SourceAddress, kernelBuffer, request->Length);
        if (NT_SUCCESS(status)) {
            SIZE_T copySize = request->Length;
            if (copySize > outputBufferLength - sizeof(KDRV_MEM_REQUEST)) {
                copySize = outputBufferLength - sizeof(KDRV_MEM_REQUEST);
            }
            RtlCopyMemory((PUCHAR)outputBuffer + sizeof(KDRV_MEM_REQUEST), kernelBuffer, copySize);
            request->BytesCopied = copySize;
            bytesReturned = (ULONG)(sizeof(KDRV_MEM_REQUEST) + copySize);
        }

        ExFreePoolWithTag(kernelBuffer, POOL_TAG);
        ObDereferenceObject(targetProcess);
        break;
    }
    case IOCTL_KDRV_WRITE_MEM:
    {
        PKDRV_MEM_REQUEST request;
        PVOID writeData;

        if (inputBufferLength < sizeof(KDRV_MEM_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        request = (PKDRV_MEM_REQUEST)inputBuffer;
        if (!request->TargetAddress || !request->Length) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (inputBufferLength < sizeof(KDRV_MEM_REQUEST) + request->Length) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = KdrvResolveProcess(request->ProcessId, &targetProcess);
        if (!NT_SUCCESS(status)) break;

        writeData = (PUCHAR)inputBuffer + sizeof(KDRV_MEM_REQUEST);

        status = KdrvWriteMemory(targetProcess, request->TargetAddress, writeData, request->Length);
        if (NT_SUCCESS(status)) {
            request->BytesCopied = request->Length;
            bytesReturned = sizeof(KDRV_MEM_REQUEST);
        }

        ObDereferenceObject(targetProcess);
        break;
    }
    case IOCTL_KDRV_GET_MODULE_BASE:
    case IOCTL_KDRV_GET_MODULE_SIZE:
    {
        PKDRV_MODULE_REQUEST request;

        if (inputBufferLength < sizeof(KDRV_MODULE_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        request = (PKDRV_MODULE_REQUEST)inputBuffer;

        status = KdrvResolveProcess(request->ProcessId, &targetProcess);
        if (!NT_SUCCESS(status)) break;

        status = KdrvFindModuleBase(targetProcess, request->ModuleName, &request->BaseAddress, &request->ImageSize);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(KDRV_MODULE_REQUEST);
        }

        ObDereferenceObject(targetProcess);
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

// Dispatch handlers
NTSTATUS
KdrvCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
KdrvUnsupported(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
}

// Driver unload
VOID
KdrvUnload(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    PKDRV_DEVICE_EXTENSION devExt;
    UNICODE_STRING symlinkName;

    if (DriverObject->DeviceObject) {
        devExt = (PKDRV_DEVICE_EXTENSION)DriverObject->DeviceObject->DeviceExtension;

        RtlInitUnicodeString(&symlinkName, SYMLINK_NAME);
        IoDeleteSymbolicLink(&symlinkName);

        if (devExt->DeviceObject) {
            IoDeleteDevice(devExt->DeviceObject);
        }
    }

    DbgPrint("[KmemDrv] Driver unloaded\n");
}

// Driver entry
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    PKDRV_DEVICE_EXTENSION devExt;
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;

    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint("[KmemDrv] Loading driver\n");

    // Set up dispatch routines
    DriverObject->MajorFunction[IRP_MJ_CREATE] = KdrvCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = KdrvCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KdrvDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_READ] = KdrvUnsupported;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = KdrvUnsupported;
    DriverObject->DriverUnload = KdrvUnload;

    RtlInitUnicodeString(&deviceName, DEVICE_NAME);
    RtlInitUnicodeString(&symlinkName, SYMLINK_NAME);

    status = IoCreateDevice(
        DriverObject,
        sizeof(KDRV_DEVICE_EXTENSION),
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("[KmemDrv] IoCreateDevice failed: 0x%X\n", status);
        return status;
    }

    // Set up device extension
    devExt = (PKDRV_DEVICE_EXTENSION)deviceObject->DeviceExtension;
    devExt->DeviceObject = deviceObject;
    RtlCopyMemory(&devExt->SymlinkName, &symlinkName, sizeof(UNICODE_STRING));

    // Create symbolic link for user-mode access
    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[KmemDrv] IoCreateSymbolicLink failed: 0x%X\n", status);
        IoDeleteDevice(deviceObject);
        return status;
    }

    // Set buffered I/O
    deviceObject->Flags |= DO_BUFFERED_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("[KmemDrv] Driver loaded successfully\n");
    DbgPrint("[KmemDrv] Device: %wZ\n", &deviceName);
    DbgPrint("[KmemDrv] Symlink: %wZ\n", &symlinkName);

    return STATUS_SUCCESS;
}
