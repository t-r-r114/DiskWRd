/*
 * DiskWRd - Windows 磁盘读写驱动
 * Copyright (c) 2026 t-r-r114
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define POOL_TAG_NAME 'emaN'
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "driver.h"
#include "queue.tmh"

EXTERN_C_START

NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
);

NTKERNELAPI
PDEVICE_OBJECT
IoGetLowerDeviceObject(
    _In_ PDEVICE_OBJECT DeviceObject
);

EXTERN_C_END

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, WriterQueueInitialize)
#endif

//辅助函数，寻找栈中某个特定驱动程序对应的设备对象，返回时保持引用计数，调用者负责释放
PDEVICE_OBJECT
FindLowerDeviceByDriverName(
    _In_ PDEVICE_OBJECT TopDeviceObject,
    _In_ PCUNICODE_STRING TargetDriverName
)
{
    PDEVICE_OBJECT currentDevice = TopDeviceObject;
    PDEVICE_OBJECT targetDevice = NULL;
    PDEVICE_OBJECT lowerDevice = NULL;

    DbgPrint("[MBR_Debug] ---> FindLowerDeviceByDriverName Enter. TopDeviceObject: 0x%p, TargetDriverName: %wZ\n", TopDeviceObject, TargetDriverName);

    // 为了在循环中安全地 Dereference，我们先给起点增加一次引用计数
    ObReferenceObject(currentDevice);

    while (currentDevice != NULL) {

        // 检查当前设备对象所属的驱动程序
        if (currentDevice->DriverObject != NULL) {
            ULONG returnLength = 0;
            ULONG bufferSize = 512;

            // 分配内存来获取对象名称
            POBJECT_NAME_INFORMATION nameInfo = (POBJECT_NAME_INFORMATION)
                ExAllocatePoolWithTag(NonPagedPool, bufferSize, POOL_TAG_NAME);

            if (nameInfo != NULL) {
                // 查询驱动对象的名字
                NTSTATUS status = ObQueryNameString(
                    currentDevice->DriverObject,
                    nameInfo,
                    bufferSize,
                    &returnLength
                );

                if (NT_SUCCESS(status) && nameInfo->Name.Buffer != NULL) {
                    DbgPrint("[MBR_Debug] 正在检查设备(0x%p)所属驱动: %wZ\n", currentDevice, &nameInfo->Name);
                    // 打印当前设备所属驱动的名字，帮助调试和验证
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "正在检查设备所属驱动: %wZ", &nameInfo->Name);

                    // 比较驱动名是否是我们寻找的目标 (例如 L"\\Driver\\Disk")
                    if (RtlCompareUnicodeString(&nameInfo->Name, TargetDriverName, TRUE) == 0) {
                        DbgPrint("[MBR_Debug] 成功匹配目标驱动名! 目标设备对象: 0x%p\n", currentDevice);
                        targetDevice = currentDevice; // 找到了！保留它的引用计数
                        ExFreePoolWithTag(nameInfo, POOL_TAG_NAME);
                        break;
                    }
                }
                else {
                    DbgPrint("[MBR_Debug] ObQueryNameString 失败或无返回名称, 状态码: 0x%08X\n", status);
                }
                ExFreePoolWithTag(nameInfo, POOL_TAG_NAME);
            }
            else {
				DbgPrint("[MBR_Debug] 内存分配失败，无法获取对象名称! 请求大小: %lu 字节\n", bufferSize);
            }
        }

        // 获取栈中的下一个底层设备对象
        lowerDevice = IoGetLowerDeviceObject(currentDevice);
        DbgPrint("[MBR_Debug] 获取下层设备对象: 0x%p\n", lowerDevice);

        // 释放对当前设备的引用
        ObDereferenceObject(currentDevice);

        // 继续向下层检查
        currentDevice = lowerDevice;
    }

    DbgPrint("[MBR_Debug] <--- FindLowerDeviceByDriverName Exit. 返回目标设备对象: 0x%p\n", targetDevice);
    // 注意：如果找到了 targetDevice，调用者使用完毕后必须对其调用 ObDereferenceObject
    return targetDevice;
}

//
// 1. 定义手动 IRP 的上下文结构
// 用于在完成例程和主派发线程之间传递状态和同步事件
//
typedef struct _MANUAL_IRP_CONTEXT {
    KEVENT Event;
    NTSTATUS Status;
} MANUAL_IRP_CONTEXT, * PMANUAL_IRP_CONTEXT;


//
// 2. 定义手动 IRP 的完成例程
// 必须清理手动分配的资源，防止内存泄漏
//
IO_COMPLETION_ROUTINE WriterManualIrpCompletionRoutine;
NTSTATUS
WriterManualIrpCompletionRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PMANUAL_IRP_CONTEXT irpContext = (PMANUAL_IRP_CONTEXT)Context;

    DbgPrint("[MBR_Debug] ---> WriterManualIrpCompletionRoutine 被调用. IRP: 0x%p, 返回状态: 0x%08X\n", Irp, Irp->IoStatus.Status);

    irpContext->Status = Irp->IoStatus.Status;

    KeSetEvent(&irpContext->Event, IO_NO_INCREMENT, FALSE);

    DbgPrint("[MBR_Debug] <--- WriterManualIrpCompletionRoutine Exit. 已设置事件，通知主线程继续处理.\n", "DiskWRd");
    return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS
WriterQueueInitialize(
    _In_ WDFDEVICE Device
)
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;

    PAGED_CODE();

    DbgPrint("[MBR_Debug] ---> WriterQueueInitialize Enter. Device: 0x%p\n", Device);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel
    );

    queueConfig.EvtIoDeviceControl = WriterEvtIoDeviceControl;
    queueConfig.EvtIoStop = WriterEvtIoStop;

    status = WdfIoQueueCreate(
        Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );

    if (!NT_SUCCESS(status)) {
        DbgPrint("[MBR_Debug] 错误: WdfIoQueueCreate 失败! 状态码: 0x%08X\n", status);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "WdfIoQueueCreate failed %!STATUS!", status);
        return status;
    }

    DbgPrint("[MBR_Debug] <--- WriterQueueInitialize Exit. Queue创建成功: 0x%p\n", queue);
    return status;
}

// 3. 处理 IOCTL_DISK_WRITE_COMMAND 的核心函数
VOID
WriterEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDISK_WRITE_PARAMS params = NULL;
    size_t length = 0;

    DbgPrint("[MBR_Debug] ---> WriterEvtIoDeviceControl Enter. Queue: 0x%p, Request: 0x%p, IoControlCode: 0x%08X\n", Queue, Request, IoControlCode);
    DbgPrint("[MBR_Debug] 输入缓冲长度: %Iu, 输出缓冲长度: %Iu\n", InputBufferLength, OutputBufferLength);

    TraceEvents(TRACE_LEVEL_INFORMATION,
        TRACE_QUEUE,
        "%!FUNC! Queue 0x%p, Request 0x%p OutputBufferLength %d InputBufferLength %d IoControlCode %d",
        Queue, Request, (int)OutputBufferLength, (int)InputBufferLength, IoControlCode);

    if (IoControlCode == IOCTL_DISK_WRITE_COMMAND) {

		DbgPrint("[MBR_Debug] 收到 IOCTL_DISK_WRITE_COMMAND 控制码，准备处理写入请求...\n", "DiskWRd");

        // 获取结构体固定头部的大小 (不包含变长数组部分)
        size_t minRequiredSize = FIELD_OFFSET(DISK_WRITE_PARAMS, Data);

        // 以最小所需大小获取输入缓冲区
        status = WdfRequestRetrieveInputBuffer(Request, minRequiredSize, (PVOID*)&params, &length);
        DbgPrint("[MBR_Debug] WdfRequestRetrieveInputBuffer 状态: 0x%08X, 获取到的长度: %Iu\n", status, length);

        if (NT_SUCCESS(status) && params != NULL) {

            DbgPrint("[MBR_Debug] 解析参数 -> DiskNumber: %u, ByteOffset: 0x%llX, WriteLength: %u\n",
                params->DiskNumber, params->ByteOffset, params->WriteLength);

            // 安全校验：确保 R3 传来的总内存大小 >= (头部大小 + 声明要写入的长度)
            if (length < minRequiredSize || params->WriteLength >(length - minRequiredSize)) {
                DbgPrint("[MBR_Debug] 严重错误: 输入缓冲区太小或发生整数溢出! minRequiredSize: %Iu\n", minRequiredSize);
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "输入缓冲区太小或发生整数溢出!");
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }

            UNICODE_STRING diskName;
            WCHAR diskNameBuffer[64];
            PFILE_OBJECT fileObject = NULL;
            PDEVICE_OBJECT deviceObject = NULL;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
                "收到写入指令(纯手动IRP模式)! DiskNumber: %d, Offset: 0x%llX, Length: %d",
                params->DiskNumber, params->ByteOffset, params->WriteLength);

            // 格式化物理磁盘设备路径
            RtlStringCbPrintfW(diskNameBuffer, sizeof(diskNameBuffer), L"\\Device\\Harddisk%lu\\Partition0", params->DiskNumber);
            RtlInitUnicodeString(&diskName, diskNameBuffer);
            DbgPrint("[MBR_Debug] 格式化目标设备路径: %wZ\n", &diskName);

            // 获取底层设备对象和文件对象（此时获取的是顶层设备对象，我们后续会在栈中寻找真正的磁盘驱动设备对象）
            status = IoGetDeviceObjectPointer(
                &diskName,
                FILE_WRITE_DATA,
                &fileObject,
                &deviceObject
            );
            DbgPrint("[MBR_Debug] IoGetDeviceObjectPointer 状态: 0x%08X. FileObject: 0x%p, DeviceObject: 0x%p\n", status, fileObject, deviceObject);

            if (NT_SUCCESS(status)) {
                PDEVICE_OBJECT targetDiskDevice = NULL;

                DbgPrint("[MBR_Debug] fileObject->DeviceObject 指向物理实体: 0x%p\n", fileObject->DeviceObject);

                // fileObject->DeviceObject 指向真实的物理磁盘设备（被 RAW 文件系统挂载的那个底层实体）
                // IoGetAttachedDeviceReference 获取该物理磁盘所在“存储栈”的顶层对象
                PDEVICE_OBJECT storageStackTop = IoGetAttachedDeviceReference(fileObject->DeviceObject);
                DbgPrint("[MBR_Debug] 存储栈顶层对象 (storageStackTop): 0x%p\n", storageStackTop);

                // 优先级 1：尝试寻找 \Driver\Disk (绕过上层过滤)
                UNICODE_STRING targetName = RTL_CONSTANT_STRING(L"\\Driver\\Disk");
                DbgPrint("[MBR_Debug] 优先在存储栈中寻找 \\Driver\\Disk...\n", "DiskWRd");
                targetDiskDevice = FindLowerDeviceByDriverName(storageStackTop, &targetName);

                if (targetDiskDevice != NULL) {
                    DbgPrint("[MBR_Debug] 成功命中核心磁盘驱动 \\Driver\\Disk!\n", "DiskWRd");
					TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "成功找到 \\Driver\\Disk 类驱动设备: 0x%p", targetDiskDevice);
                }
                else {
					DbgPrint("[MBR_Debug] 没找到 \\Driver\\Disk，尝试寻找 \\Driver\\partmgr...\n", "DiskWRd");
                    // 优先级 2：如果没找到 Disk，降级寻找 \Driver\partmgr
                    RtlInitUnicodeString(&targetName, L"\\Driver\\partmgr");
                    targetDiskDevice = FindLowerDeviceByDriverName(storageStackTop, &targetName);

                    if (targetDiskDevice != NULL) {
						DbgPrint("[MBR_Debug] 没找到 \\Driver\\Disk，但找到了 \\Driver\\partmgr，继续使用它进行写入操作（可能会被 partmgr 拦截）\n");
						TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE, "没找到 \\Driver\\Disk，但找到了 \\Driver\\partmgr，继续使用它进行写入操作（可能会被 partmgr 拦截）");
                    }
                }

                // 无论成功与否，先释放我们对存储栈顶层设备的引用，因为我们后续会根据找到的设备对象继续操作
                ObDereferenceObject(storageStackTop);

                // 如果两个都没有找到，记录错误并退出，因为没有底层设备我们无法继续构造 IRP 了
                if (targetDiskDevice == NULL) {
                    DbgPrint("[MBR_Debug] 严重错误：没找到 partmgr，也没找到 Disk！中止操作。\n", "DiskWRd");
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "未能找到 \\Driver\\Disk 原生设备！");
                    ObDereferenceObject(fileObject);
                    WdfRequestComplete(Request, STATUS_NOT_FOUND);
                    return;
                }

                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "成功找到底层原生 Disk 设备对象: 0x%p", targetDiskDevice);

                MANUAL_IRP_CONTEXT irpContext;
                KeInitializeEvent(&irpContext.Event, NotificationEvent, FALSE);
                irpContext.Status = STATUS_UNSUCCESSFUL;

                LARGE_INTEGER startingOffset;
                startingOffset.QuadPart = params->ByteOffset;

                // 3. 手动分配空白 IRP
                DbgPrint("[MBR_Debug] 开始手动分配 IRP，StackSize: %d\n", targetDiskDevice->StackSize);
                PIRP irp = IoAllocateIrp(targetDiskDevice->StackSize, FALSE);

                if (irp != NULL) {
                    DbgPrint("[MBR_Debug] IoAllocateIrp 成功，IRP地址: 0x%p\n", irp);

                    // 4. 为 WDF 提供的内核缓冲区分配 MDL
                    PMDL mdl = IoAllocateMdl(params->Data, params->WriteLength, FALSE, FALSE, irp);

                    if (mdl != NULL) {
                        DbgPrint("[MBR_Debug] IoAllocateMdl 成功，MDL地址: 0x%p\n", mdl);

                        // 5. 告知系统此内存已在内核非分页池中，无需锁定与探针！
                        MmBuildMdlForNonPagedPool(mdl);
                        DbgPrint("[MBR_Debug] MmBuildMdlForNonPagedPool 执行完毕\n");

                        // 6. 填充下层驱动的 IRP 栈参数
                        PIO_STACK_LOCATION nextStack = IoGetNextIrpStackLocation(irp);
                        nextStack->MajorFunction = IRP_MJ_WRITE;
                        nextStack->Parameters.Write.Length = params->WriteLength;
                        nextStack->Parameters.Write.ByteOffset = startingOffset;

                        DbgPrint("[MBR_Debug] IRP 栈参数设置完毕 -> MajorFunction: IRP_MJ_WRITE, Offset: 0x%llX, Length: %u\n",
                            startingOffset.QuadPart, params->WriteLength);

                        // 【新增的反拦截魔法：强制物理绕过】
                        #ifndef SL_FORCE_DIRECT_WRITE
                        #define SL_FORCE_DIRECT_WRITE 0x20
                        #endif

                        // 1. 绕过 partmgr 的直接写入保护，强制放行
                        nextStack->Flags |= SL_FORCE_DIRECT_WRITE;

                        // 2. 声明为无缓存的物理底层 IO，避免被缓存管理器拦截
                        irp->Flags |= IRP_NOCACHE;

                        // 必须为下层栈提供 FileObject！
                        nextStack->FileObject = fileObject;

                        // 为 IRP 附加当前线程，底层驱动常依赖此字段进行状态检查或 APC 投递
                        irp->Tail.Overlay.Thread = PsGetCurrentThread();

                        // 明确指明请求来源为内核模式
                        irp->RequestorMode = KernelMode;

                        // 7. 设置完成例程
                        IoSetCompletionRoutine(
                            irp,
                            WriterManualIrpCompletionRoutine,
                            &irpContext,
                            TRUE, TRUE, TRUE
                        );
                        DbgPrint("[MBR_Debug] IoSetCompletionRoutine 设置完毕\n");

                        // =========================================================================
                        // 【强制绕过核心魔法：Write 改造】
                        // 不调用 IoCallDriver，直接硬定位并呼叫底层派遣函数
                        // =========================================================================

                        DbgPrint("[MBR_Debug] 准备使用直接修改栈指针+定位原始函数的方式投递 WRITE IRP...\n");

                        // 8.1 手动修改 IRP 栈指针：模拟 IoCallDriver 的内部推栈行为
                        // 将 CurrentStackLocation 向下移动一层，使底层驱动能正确读到我们刚才设置的参数
                        IoSetNextIrpStackLocation(irp);

                        // 8.2 定位原始函数：从目标底层设备的驱动对象中，直接提取 IRP_MJ_WRITE 的函数指针
                        PDRIVER_DISPATCH originalDispatchWrite = targetDiskDevice->DriverObject->MajorFunction[IRP_MJ_WRITE];

                        DbgPrint("[MBR_Debug] 成功提取原始 DispatchWrite 函数地址: 0x%p\n", originalDispatchWrite);

                        // 8.3 硬调用：绕过系统 API，直接将设备对象和 IRP 拍给原始函数
                        status = originalDispatchWrite(targetDiskDevice, irp);

                        DbgPrint("[MBR_Debug] 直接调用 DispatchWrite 立刻返回的状态码: 0x%08X\n", status);
                        // =========================================================================

                        // 9. 无条件等待
                        DbgPrint("[MBR_Debug] KeWaitForSingleObject 开始等待完成例程触发事件...\n");
                        KeWaitForSingleObject(&irpContext.Event, Executive, KernelMode, FALSE, NULL);
                        DbgPrint("[MBR_Debug] KeWaitForSingleObject 等待结束！\n");

                        // 统一在完成例程和这里清理 IRP 和 MDL，确保无论成功与否都不会泄漏资源
                        if (irp->MdlAddress != NULL) {
                            DbgPrint("[MBR_Debug] 正在清理 MDL: 0x%p\n", irp->MdlAddress);
                            IoFreeMdl(irp->MdlAddress);
                            irp->MdlAddress = NULL;
                        }
                        DbgPrint("[MBR_Debug] 正在清理 IRP: 0x%p\n", irp);
                        IoFreeIrp(irp);

                        // 从我们自定义的上下文中获取真实的底层完成状态
                        status = irpContext.Status;
                        DbgPrint("[MBR_Debug] IRP 实际最终完成状态: 0x%08X\n", status);

                        if (NT_SUCCESS(status)) {
                            DbgPrint("[MBR_Debug] 成功: 完全手动构造的 IRP 写入完成!\n");
                            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "通过完全手动构造的 IRP 写入成功!");
                        }
                        else {
                            DbgPrint("[MBR_Debug] 错误: 底层硬件处理 IRP 拒绝或失败! 状态码: 0x%08X\n", status);
                            TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "底层硬件处理 IRP 拒绝或失败: %!STATUS!", status);
                        }

                    }
                    else {
                        DbgPrint("[MBR_Debug] 错误: IoAllocateMdl 分配失败!\n");
                        IoFreeIrp(irp); // MDL 分配失败需清理 IRP
                        status = STATUS_INSUFFICIENT_RESOURCES;
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "IoAllocateMdl 失败!");
                    }
                }
                else {
                    DbgPrint("[MBR_Debug] 错误: IoAllocateIrp 分配失败!\n");
                    status = STATUS_INSUFFICIENT_RESOURCES;
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "IoAllocateIrp 失败!");
                }

                // 最后，清理我们在这个过程中获取的对象引用，防止内存泄漏
                DbgPrint("[MBR_Debug] 解引用目标磁盘设备对象: 0x%p\n", targetDiskDevice);
                ObDereferenceObject(targetDiskDevice);

                // 10. 解引用文件对象
                DbgPrint("[MBR_Debug] 解引用 FileObject: 0x%p\n", fileObject);
                ObDereferenceObject(fileObject);
            }
            else {
                DbgPrint("[MBR_Debug] 错误: IoGetDeviceObjectPointer 找不到磁盘! 状态码: 0x%08X\n", status);
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "IoGetDeviceObjectPointer 找不到磁盘: %!STATUS!", status);
            }

            DbgPrint("[MBR_Debug] 完成 WDF Request. 返回状态: 0x%08X, 信息量: %u\n", status, NT_SUCCESS(status) ? params->WriteLength : 0);
            WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? params->WriteLength : 0);
            return;
        }
        else {
            DbgPrint("[MBR_Debug] WdfRequestRetrieveInputBuffer 失败或 params 为 NULL\n");
        }
    }
    else if (IoControlCode == IOCTL_DISK_READ_COMMAND) {
        PDISK_READ_PARAMS readParams = NULL;
        DbgPrint("[MBR_Debug] 捕获到目标控制码: IOCTL_DISK_READ_COMMAND\n");

        size_t minRequiredSize = FIELD_OFFSET(DISK_READ_PARAMS, Data);

        // 使用 OutputBuffer 即可，因为是 METHOD_BUFFERED，它与 InputBuffer 共享同一块内存
        status = WdfRequestRetrieveOutputBuffer(Request, minRequiredSize, (PVOID*)&readParams, &length);

        if (NT_SUCCESS(status) && readParams != NULL) {

            // 校验应用层分配的总缓冲池是否足够装下声明要读取的长度
            if (length < minRequiredSize || readParams->ReadLength >(length - minRequiredSize)) {
                DbgPrint("[MBR_Debug] 严重错误: 缓冲区不足以容纳请求的读取长度!\n");
                WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
                return;
            }

            UNICODE_STRING diskName;
            WCHAR diskNameBuffer[64];
            PFILE_OBJECT fileObject = NULL;
            PDEVICE_OBJECT deviceObject = NULL;

            RtlStringCbPrintfW(diskNameBuffer, sizeof(diskNameBuffer), L"\\Device\\Harddisk%lu\\Partition0", readParams->DiskNumber);
            RtlInitUnicodeString(&diskName, diskNameBuffer);

            status = IoGetDeviceObjectPointer(&diskName, FILE_READ_DATA, &fileObject, &deviceObject);

            if (NT_SUCCESS(status)) {
                PDEVICE_OBJECT storageStackTop = IoGetAttachedDeviceReference(fileObject->DeviceObject);
                PDEVICE_OBJECT targetDiskDevice = NULL;
                UNICODE_STRING targetName = RTL_CONSTANT_STRING(L"\\Driver\\Disk");

                targetDiskDevice = FindLowerDeviceByDriverName(storageStackTop, &targetName);
                if (targetDiskDevice == NULL) {
                    RtlInitUnicodeString(&targetName, L"\\Driver\\partmgr");
                    targetDiskDevice = FindLowerDeviceByDriverName(storageStackTop, &targetName);
                }
                ObDereferenceObject(storageStackTop);

                if (targetDiskDevice != NULL) {
                    MANUAL_IRP_CONTEXT irpContext;
                    KeInitializeEvent(&irpContext.Event, NotificationEvent, FALSE);
                    irpContext.Status = STATUS_UNSUCCESSFUL;

                    LARGE_INTEGER startingOffset;
                    startingOffset.QuadPart = readParams->ByteOffset;

                    PIRP irp = IoAllocateIrp(targetDiskDevice->StackSize, FALSE);
                    if (irp != NULL) {
                        PMDL mdl = IoAllocateMdl(readParams->Data, readParams->ReadLength, FALSE, FALSE, irp);
                        if (mdl != NULL) {
                            MmBuildMdlForNonPagedPool(mdl);

                            PIO_STACK_LOCATION nextStack = IoGetNextIrpStackLocation(irp);
                            nextStack->MajorFunction = IRP_MJ_READ;
                            nextStack->Parameters.Read.Length = readParams->ReadLength;
                            nextStack->Parameters.Read.ByteOffset = startingOffset;
                            nextStack->FileObject = fileObject;

                            irp->Flags |= IRP_NOCACHE;
                            irp->Tail.Overlay.Thread = PsGetCurrentThread();
                            irp->RequestorMode = KernelMode;

                            IoSetCompletionRoutine(irp, WriterManualIrpCompletionRoutine, &irpContext, TRUE, TRUE, TRUE);

                            // =========================================================================
                            // 【强制读取绕过：修改指针 + 定位原始函数】
                            // =========================================================================
                            IoSetNextIrpStackLocation(irp);

                            PDRIVER_DISPATCH originalDispatchRead = targetDiskDevice->DriverObject->MajorFunction[IRP_MJ_READ];

                            DbgPrint("[MBR_Debug] 绕过 IoCallDriver，直接硬调用底层 DispatchRead 函数地址: 0x%p\n", originalDispatchRead);

                            status = originalDispatchRead(targetDiskDevice, irp);
                            // =========================================================================

                            // 修复隐患：必须无条件等待，因为完成例程返回了 STATUS_MORE_PROCESSING_REQUIRED
                            KeWaitForSingleObject(&irpContext.Event, Executive, KernelMode, FALSE, NULL);

                            if (irp->MdlAddress != NULL) {
                                IoFreeMdl(irp->MdlAddress);
                            }
                            IoFreeIrp(irp);

                            status = irpContext.Status;
                            DbgPrint("[MBR_Debug] 强制读取 IRP 最终完成状态: 0x%08X\n", status);

                        }
                        else {
                            IoFreeIrp(irp);
                            status = STATUS_INSUFFICIENT_RESOURCES;
                        }
                    }
                    else {
                        status = STATUS_INSUFFICIENT_RESOURCES;
                    }
                    ObDereferenceObject(targetDiskDevice);
                }
                else {
                    status = STATUS_NOT_FOUND;
                }
                ObDereferenceObject(fileObject);
            }

            size_t bytesReturned = NT_SUCCESS(status) ? (minRequiredSize + readParams->ReadLength) : 0;
            WdfRequestCompleteWithInformation(Request, status, bytesReturned);
            return;
        }
    }
    else {
        DbgPrint("[MBR_Debug] 未识别的控制码: 0x%08X\n", IoControlCode);
	    TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE, "未识别的控制码: 0x%08X", IoControlCode);
    }

    DbgPrint("[MBR_Debug] <--- WriterEvtIoDeviceControl Exit. 完成请求并返回状态: 0x%08X\n", status);
    WdfRequestComplete(Request, status);
    return;
}

VOID
WriterEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
    DbgPrint("[MBR_Debug] ---> WriterEvtIoStop Enter. Queue: 0x%p, Request: 0x%p, ActionFlags: %u\n", Queue, Request, ActionFlags);
    TraceEvents(TRACE_LEVEL_INFORMATION,
        TRACE_QUEUE,
        "%!FUNC! Queue 0x%p, Request 0x%p ActionFlags %d",
        Queue, Request, ActionFlags);
    return;
}