# SC2 "4.25秒+180.exe" 逆向分析：180精通 & 秒杀实现原理

## 目标文件信息

| 属性 | 值 |
|------|-----|
| 文件名 | 4.25秒+180.exe |
| MD5 | fed16d91a9a26947345aff5662884dcb |
| 加壳方式 | UPX（LZMA 变体，无法直接 `upx -d` 解包） |
| 真实 OEP | `0x4A4EAA` |
| 代码段 | `0x401000 – 0x687000`（名为 `xy666_`，约 2.6 MB） |
| 数据段 | `0x687000 – 0x11A7000`（名为 `_xy666_`，约 11 MB） |
| 目标进程 | `SC2_x64.exe`（64 位星际争霸 2） |

---

## 程序整体架构

程序是一个**Galaxy VM 容器 + 外挂注入器**的组合体，包含三层：

```
4.25秒+180.exe
│
├── Galaxy VM 运行时（完整 SC2 Galaxy 脚本引擎副本）
│   └── 通过 Galaxy native dispatch 调用游戏内触发器变量
│
├── 嵌入 PE①（0x77FA7A，x86，约 220 KB）
│   └── 内存补丁执行器（读取 SC2 模块基址，写入 shellcode）
│
└── 嵌入 PE②（0x7B5A82，x86，约 6 MB）
    └── 辅助注入模块
```

---

## 一、180 精通实现

### 1.1 原理概述

"180" 指两名指挥官精通总点数上限：
- 每位指挥官有 **3 个精通槽**，每槽上限 **30 点**
- 2 名指挥官 × 3 槽 × 30 点 = **180 点**

程序通过 **Hook SC2 精通值写入函数**，在每次游戏更新精通值时强制将其覆盖为最大值。

### 1.2 关键地址

| 项目 | 值 |
|------|-----|
| SC2 精通更新函数偏移 | `SC2_x64.exe + 0xEFD6FB` |
| 返回地址（hook 后跳回） | `SC2_x64.exe + 0xEFD709` |
| 精通值字段①偏移 | `[RDI + 0x168]`（16 位整数） |
| 精通值字段②偏移 | `[RDI + 0x16A]`（16 位整数） |
| 写入值 | `0x7FFF`（int16 最大值 = 32767） |

### 1.3 注入的 Shellcode（写入游戏进程）

原始位置（`EFD6FB`）被替换为：

```asm
; ===== 写入 SC2_x64.exe+EFD6FB =====

66 C7 87 68 01 00 00 FF 7F   ; MOV WORD PTR [RDI+0x168], 0x7FFF
66 C7 87 6A 01 00 00 FF 7F   ; MOV WORD PTR [RDI+0x16A], 0x7FFF
FF 25 00 00 00 00            ; JMP QWORD PTR [RIP+0]
                             ;   → 跳回 EFD709（原函数继续执行）
```

### 1.4 关闭时的恢复

关闭精通满级时，将 hook 处恢复为原始字节（9 个 NOP）：

```
90 90 90 90 90 90 90 90 90   ; NOP × 9（还原原始代码）
```

### 1.5 触发流程

```
程序启动
  └─ sub_4010DA()                          ← 找到 SC2 游戏窗口/Galaxy 会话
       └─ FindWindow("StarCraft II")
       └─ 获取 Galaxy VM 游戏对象指针
  └─ 用户点击"精通全满"按钮
       └─ sub_4015CC()                      ← 检查当前状态
            └─ sub_4272AA(lib=0x51EB0001, func=369181117, ...)
                 └─ 读取 Galaxy 变量判断是否已开启
       └─ 调用嵌入 PE 中的补丁函数
            └─ GetModuleHandleA("SC2_x64.exe") → 获取模块基址
            └─ base + 0xEFD6FB → 目标地址
            └─ VirtualProtect(target, 0xE, PAGE_EXECUTE_READWRITE)
            └─ WriteProcessMemory / memcpy → 写入 shellcode
            └─ VirtualProtect(target, 0xE, 原属性) → 恢复保护
       └─ 显示 "精通全满开启成功" / "精通全满开启失败"
```

### 1.6 精通值计算逻辑

游戏在读取精通值时会将 `0x7FFF` clamp 到每槽最大值（30），因此：

```
0x7FFF = 32767 → clamp → 30（满级）
两个字段 × 3 槽 = 6 槽 × 30 = 180 点
```

---

## 二、秒杀实现

### 2.1 关键地址

| 项目 | 值 |
|------|-----|
| SC2 伤害计算函数偏移 | `SC2_x64.exe + 0x747BE1` |
| 返回地址 | `SC2_x64.exe + 0x747BE9` |

### 2.2 注入的 Shellcode

```asm
; ===== 写入 SC2_x64.exe+747BE1 =====

41 80 7C 24 40 01   ; CMP BYTE PTR [R12+0x40], 1    ← 判断技能类型
74 14               ; JE  +0x14                     ← 类型1跳转
41 80 7C 24 40 02   ; CMP BYTE PTR [R12+0x40], 2    ← 判断技能类型
74 0C               ; JE  +0x0C                     ← 类型2跳转
41 C7 84 24 C8 01 00 00 00 61 79 FE
                    ; MOV DWORD PTR [R12+0x1C8], 0xFE796100  ← 写入伤害值
41 03 84 24 C8 01 00 00
                    ; ADD EAX, [R12+0x1C8]
FF 25 00 00 00 00   ; JMP QWORD PTR [RIP+0] → 跳回 747BE9
```

### 2.3 触发流程

```
用户点击"秒杀开启"
  └─ 获取 SC2_x64.exe 基址
  └─ 定位 base + 0x747BE1
  └─ 写入 shellcode（检查技能类型 → 强制设置超大伤害值）
  └─ 显示 "秒杀开启成功" / "秒杀开启失败"

游戏内攻击时：
  SC2 调用 0x747BE1
    └─ Hook 检查 [R12+0x40] 技能类型
    └─ 向 [R12+0x1C8] 写入 0xFE796100（约 4.3 亿伤害）
    └─ 跳回原函数继续 → 目标 HP 归零
```

---

## 三、Galaxy VM 层（辅助控制）

程序同时通过内置的 Galaxy VM 引擎控制游戏触发器变量。

### 核心调用接口

```c
// Galaxy native dispatch
sub_4272AA(
    library_id,   // 0x51EB0001 = SC2 游戏库
    func_id,      // 功能函数 ID
    type,         // 19 = 设置整数变量
    -1,           // 标志
    value,        // 3 = 开启, 0 = 关闭
    0
);
```

### 批量变量控制（sub_4015CC）

```c
// 开启时：将 18 个 Galaxy 整数变量全部设为 3
for (func_id = 369181088; func_id <= 369181117; ...) {
    sub_4272AA(0x51EB0001, func_id, 19, -1, 3, 0);
}

// 关闭时：将这 18 个变量全部设为 0
for (...) {
    sub_4272AA(0x51EB0001, func_id, 19, -1, 0, 0);
}
```

这 18 个变量控制游戏内各类技能/效果的倍率系数。

---

## 四、反调试机制

程序在 `debug132` 段（`0x1011845D`）设置了一个除零陷阱：

```asm
xor eax, eax
div eax          ; EAX=0 → 触发 #DE 异常
                 ; → MSVC CRT 的 assert handler → abort()
```

**绕过方法**：调试时让 IDA 把异常**传递回程序的 SEH 处理链**（不拦截），
由程序自带的 SEH handler 处理该异常后继续运行，最终到达 OEP `0x4A4EAA`。

---

## 五、偏移汇总

| 功能 | SC2_x64.exe 偏移 | 作用 |
|------|-----------------|------|
| 精通写入 hook 点 | `+0xEFD6FB` | 精通值更新函数入口 |
| 精通 hook 返回点 | `+0xEFD709` | 原函数继续执行 |
| 秒杀 hook 点 | `+0x747BE1` | 伤害计算函数入口 |
| 秒杀 hook 返回点 | `+0x747BE9` | 原函数继续执行 |
| 字库路径相关 | `+0x8F90C5` | |
| 字库基址 | `+0x8F90F5` | |
| 资源偏移① | `+0x7680F8` | |
| 资源偏移② | `+0x9E6BA1` | |
| 注入 shellcode 存储 | `+0x59A806` | |

---

## 六、复现思路（用于自己实现类似功能）

```cpp
// 1. 获取 SC2 进程句柄
HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, sc2_pid);

// 2. 获取模块基址
HMODULE hMod = GetModuleHandleA("SC2_x64.exe"); // 或通过 EnumProcessModules
uintptr_t base = (uintptr_t)hMod;

// 3. 精通满级 hook
uintptr_t mastery_target = base + 0xEFD6FB;
BYTE mastery_patch[] = {
    0x66, 0xC7, 0x87, 0x68, 0x01, 0x00, 0x00, 0xFF, 0x7F,  // MOV WORD [RDI+0x168], 0x7FFF
    0x66, 0xC7, 0x87, 0x6A, 0x01, 0x00, 0x00, 0xFF, 0x7F,  // MOV WORD [RDI+0x16A], 0x7FFF
    // + JMP 跳回 EFD709 的 trampoline
};
WriteProcessMemory(hProcess, (LPVOID)mastery_target, mastery_patch, sizeof(mastery_patch), NULL);

// 4. 还原时写 NOP
BYTE nop9[] = {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
WriteProcessMemory(hProcess, (LPVOID)mastery_target, nop9, 9, NULL);
```

> **注意**：SC2 有反作弊机制，直接 `WriteProcessMemory` 在较新版本可能被检测。
> 原程序通过嵌入 PE 注入 + VirtualProtect 绕过页面保护来完成写入。
