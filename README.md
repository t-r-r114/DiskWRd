# DiskWRd

> Windows 平台基于 KMDF 的低层磁盘读写驱动（Low-Level Disk Read/Write Driver）

DiskWRd 是一个 Windows 内核模式驱动（KMDF），允许应用程序以 **IOCTL** 方式对物理磁盘进行**绕过文件系统层**的直接扇区级读写。其核心实现完全脱离 `IoCallDriver` 的传统投递方式，通过**手动构造 IRP + 直接调用底层磁盘驱动的派遣函数**，实现真正穿透存储栈的读写操作。

---

## ✨ 核心特性

| 特性 | 说明 |
| --- | --- |
| 🔓 **存储栈穿透** | 手动分配 IRP（`IoAllocateIrp`），直接调用 `\Driver\Disk` 的原始 `IRP_MJ_READ / IRP_MJ_WRITE` 派遣函数，绕过 `partmgr`、卷过滤驱动等上层拦截 |
| 🛡️ **访问控制** | 控制设备使用 `SDDL_DEVOBJ_SYS_ALL_ADM_ALL` 安全描述符，仅允许 SYSTEM 和管理员访问 |
| 📐 **结构体对齐** | 通信结构体强制 8 字节对齐（`#pragma pack(8)`），兼容 32 位应用与 64 位驱动之间的通信 |
| 🧵 **同步完成** | 通过完成例程 + `KEVENT` 无条件等待 IRP 完成，确保读写结果可靠返回 |
| 🔍 **栈内设备定位** | `FindLowerDeviceByDriverName()` 沿存储设备栈向下遍历，优先定位 `\Driver\Disk`，未命中时降级定位 `\Driver\partmgr` |
| 📦 **变长数据** | 读写参数采用 `ANYSIZE_ARRAY` 变长结构，支持动态长度的数据传输 |

---

## ⚙️ 工作原理

```javascript
┌─────────────────────────────────────────────────────────────┐
│  Ring3 应用 (CreateFileW + DeviceIoControl)                 │
└──────────────────────────────┬──────────────────────────────┘
                               │ IOCTL_DISK_WRITE / READ_COMMAND
                               ▼
┌─────────────────────────────────────────────────────────────┐
│  Ring0  DiskWRd.sys                                         │
│                                                             │
│  ┌─────────────────┐                                        │
│  │ WDF IO Queue    │  (EvtIoDeviceControl)                  │
│  └────────┬────────┘                                        │
│           ▼                                                 │
│  1. IoGetDeviceObjectPointer("\Device\HarddiskN\Partition0")│
│  2. IoGetAttachedDeviceReference → 存储栈顶层               │
│  3. FindLowerDeviceByDriverName → \Driver\Disk (或 partmgr)│
│  4. IoAllocateIrp + IoAllocateMdl + MmBuildMdlForNonPagedPool
│  5. 设置 SL_FORCE_DIRECT_WRITE / IRP_NOCACHE / KernelMode   │
│  6. IoSetNextIrpStackLocation + 硬调用原始 Dispatch 函数    │
│  7. KeWaitForSingleObject 等待完成事件                      │
│  8. IoFreeIrp / IoFreeMdl 清理，返回结果                    │
└─────────────────────────────────────────────────────────────┘
```

### 关键绕过技术

- **`SL_FORCE_DIRECT_WRITE (0x20)`**：写入请求栈参数标志，绕过 `partmgr` 对分区边界写入的保护。
- **`IRP_NOCACHE`**：声明为无缓存物理 I/O，避免缓存管理器介入。
- **`RequestorMode = KernelMode`**：明确标记请求来源为内核模式。
- **跳过 `IoCallDriver`**：手动推栈（`IoSetNextIrpStackLocation`）后，从目标设备驱动对象的 `MajorFunction[]` 表中取出原始函数指针直接调用，彻底绕过中间层过滤驱动。

---

## 🛠️ 编译环境

- **操作系统**：Windows 10 / 11（x64）
- **开发工具**：Visual Studio 2019 / 2022
- **SDK / WDK**：Windows Driver Kit（WDK），版本与 SDK 匹配
- **工程文件**：`DiskWRd.sln`

```powershell
# 使用 Visual Studio Developer 命令行编译（Release | x64）
msbuild DiskWRd.sln /p:Configuration=Release /p:Platform=x64
```

> ⚠️ 驱动需经过**测试签名**或**WHQL 签名**方可加载。开发阶段可使用测试模式：
> ```powershell
> bcdedit /set testsigning on
> # 重启后安装驱动
> ```

---

## 📥 安装与卸载

```powershell
# 安装（需要管理员权限）
sc create DiskWRd type= kernel binPath= C:\path\to\DiskWRd.sys
sc start DiskWRd

# 卸载
sc stop DiskWRd
sc delete DiskWRd
```

驱动创建控制设备 `\Device\MyWriter`，并建立符号链接 `\\.\MyWriter`，应用程序可直接打开。

---

## 💻 应用程序调用示例

驱动与应用通过两个 IOCTL 通信（定义见 `Public.h`）：

```cpp
#include "Public.h"

HANDLE hDevice = CreateFileW(
    L"\\\\.\\MyWriter",
    GENERIC_READ | GENERIC_WRITE,
    0, NULL, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL, NULL);

// 读取 Disk 1 从 0 开始的 512 字节（MBR）
DISK_READ_PARAMS readReq{};
readReq.DiskNumber = 1;
readReq.ByteOffset = 0;
readReq.ReadLength = 512;

DWORD bytesReturned = 0;
BYTE buffer[FIELD_OFFSET(DISK_READ_PARAMS, Data) + 512];
memcpy(buffer, &readReq, FIELD_OFFSET(DISK_READ_PARAMS, Data));

if (DeviceIoControl(hDevice, IOCTL_DISK_READ_COMMAND,
        buffer, sizeof(buffer),
        buffer, sizeof(buffer),
        &bytesReturned, NULL)) {
    // buffer + FIELD_OFFSET(DISK_READ_PARAMS, Data) 即为读到的数据
}

// 写入
DISK_WRITE_PARAMS writeReq{};
writeReq.DiskNumber = 1;
writeReq.ByteOffset = 0;
writeReq.WriteLength = 512;

BYTE wbuf[FIELD_OFFSET(DISK_WRITE_PARAMS, Data) + 512];
memcpy(wbuf, &writeReq, FIELD_OFFSET(DISK_WRITE_PARAMS, Data));
memcpy(wbuf + FIELD_OFFSET(DISK_WRITE_PARAMS, Data), data, 512);

DeviceIoControl(hDevice, IOCTL_DISK_WRITE_COMMAND,
    wbuf, sizeof(wbuf), NULL, 0, &bytesReturned, NULL);
```

### IOCTL 一览

| 控制码 | 功能 |
| --- | --- |
| `IOCTL_DISK_WRITE_COMMAND` | 向指定物理磁盘偏移写入任意长度数据 |
| `IOCTL_DISK_READ_COMMAND` | 从指定物理磁盘偏移读取任意长度数据 |

参数结构（8 字节对齐）：

```c
typedef struct _DISK_WRITE_PARAMS {
    ULONG     DiskNumber;      // 物理磁盘编号 (如 1 = Disk 1)
    ULONGLONG ByteOffset;      // 起始字节偏移（须为扇区大小的倍数）
    ULONG     WriteLength;     // 写入长度
    UCHAR     Data[ANYSIZE_ARRAY]; // 变长数据区
} DISK_WRITE_PARAMS;
```

---

## ⚠️ 安全与免责声明

> **本驱动可绕过操作系统的磁盘写入保护机制，属于高危内核组件。**

- 直接写入正在使用的磁盘可能导致 **数据损坏、系统无法启动甚至硬件层面故障**；
- 仅授权给 **SYSTEM 和 Administrators**，普通用户进程无法访问；
- 请仅在测试环境或虚拟机中使用，**勿在生产环境或对存放重要数据的磁盘进行操作**；
- 作者不对任何因使用本驱动造成的损失负责，使用者需自行承担全部风险。

---

## 📂 项目结构

```javascript
DiskWRd/
├── Driver.c / Driver.h     # 驱动入口，非 PnP 控制设备创建（\Device\MyWriter）
├── Device.c / Device.h     # 设备对象创建、符号链接、设备接口注册
├── Queue.c / Queue.h       # IO 队列、IOCTL 分发、手动 IRP 构造与投递（核心逻辑）
├── Public.h                # 应用与驱动共享的 IOCTL 码和通信结构体
├── Trace.h                 # WPP 跟踪（WPP_INIT_TRACING / TraceEvents）
├── DiskWRd.inf             # 驱动安装信息文件
└── DiskWRd.sln / .vcxproj  # Visual Studio 工程文件
```

---

## 📜 许可证

本项目基于 [Apache License 2.0](LICENSE) 开源。
