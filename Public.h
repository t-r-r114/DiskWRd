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

//
// Define an Interface Guid so that apps can find the device and talk to it.
//
DEFINE_GUID(GUID_DEVINTERFACE_Writer,
    0x1facfca1, 0x9f67, 0x49e8, 0xa9, 0xa8, 0xbf, 0x86, 0xe9, 0x7d, 0x13, 0xa3);
// {1facfca1-9f67-49e8-a9a8-bf86e97d13a3}

// 定义 IOCTL 控制码
#define IOCTL_DISK_WRITE_COMMAND \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 强制 8 字节对齐，防止 32位 EXE 和 64位 SYS 通信时结构体错位
#pragma pack(push, 8)
typedef struct _DISK_WRITE_PARAMS {
    ULONG     DiskNumber;      // 物理磁盘编号 (例如 1 代表 Disk 1)
    ULONGLONG ByteOffset;      // 写入的起始位置 (必须是扇区大小 512 或 4096 的倍数)
    ULONG     WriteLength;     // 写入的数据长度 (由 App 动态指定)
    UCHAR     Data[ANYSIZE_ARRAY]; // 变长数组占位符，接收动态长度数据
} DISK_WRITE_PARAMS, * PDISK_WRITE_PARAMS;
#pragma pack(pop)

// 定义读取的 IOCTL 控制码
#define IOCTL_DISK_READ_COMMAND \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 读取参数结构体 (App 发送此结构体，驱动将数据填入 Data 并返回)
#pragma pack(push, 8)
typedef struct _DISK_READ_PARAMS {
    ULONG     DiskNumber;      // 物理磁盘编号
    ULONGLONG ByteOffset;      // 读取的起始位置
    ULONG     ReadLength;      // 期望读取的长度
    UCHAR     Data[ANYSIZE_ARRAY]; // 变长数组，用于向应用层返回读取到的数据
} DISK_READ_PARAMS, * PDISK_READ_PARAMS;
#pragma pack(pop)
