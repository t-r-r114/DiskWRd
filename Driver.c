#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include "driver.h"
#include "queue.tmh"
#include "driver.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, WriterEvtDriverContextCleanup)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDRIVER hDriver;

    // 初始化 WPP 跟踪
    WPP_INIT_TRACING(DriverObject, RegistryPath);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = WriterEvtDriverContextCleanup;

    // 1. 核心修改：声明为非 PnP 驱动，不需要 EvtDeviceAdd
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNonPnpDriver;

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, &hDriver);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    // 2. 分配控制设备 (Control Device) 初始化结构
    // 【安全修复】设置安全描述符：SDDL_DEVOBJ_SYS_ALL_ADM_ALL 
    // 彻底禁止普通用户访问，仅限系统(SYSTEM)和管理员(ADMINISTRATORS)进行读写
    PWDFDEVICE_INIT pInit = WdfControlDeviceInitAllocate(hDriver, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (pInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // 3. 为设备分配内核名字
    DECLARE_CONST_UNICODE_STRING(ntDeviceName, L"\\Device\\MyWriter");
    status = WdfDeviceInitAssignName(pInit, &ntDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pInit);
        return status;
    }

    // 4. 创建设备对象
    WDFDEVICE hDevice;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&pInit, &deviceAttributes, &hDevice);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pInit);
        return status;
    }

    // 5. 创建 DOS 符号链接（这正是 Qt 中 CreateFileW("\\\\.\\MyWriter") 寻找的名字）
    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, L"\\DosDevices\\MyWriter");
    status = WdfDeviceCreateSymbolicLink(hDevice, &symbolicLinkName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "CreateSymbolicLink failed %!STATUS!", status);
        return status; // 如果失败，框架会在卸载时自动清理已创建的 hDevice
    }

    // 6. 初始化 IO 队列（处理来自 R3 的 IOCTL 请求）
    status = WriterQueueInitialize(hDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // 7. 必须调用：完成控制设备的初始化
    WdfControlFinishInitializing(hDevice);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit Success");
    return STATUS_SUCCESS;
}

VOID
WriterEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}