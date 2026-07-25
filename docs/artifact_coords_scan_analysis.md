# SC2 ArtifactCoords 定位与读取分析

> **目标代码**: `Dll1/core/game_data.cpp` 中的 `ScanCameraManagerGlobal()` 与 `ReadArtifactCoords()`  
> **定位目标**: 保存 CameraManager 根指针的全局槽地址

---

## 一、结论概述

当前代码并不在 `SC2_x64.exe` 中直接搜索“神器”对象，也没有使用固定模块偏移。它利用相机系统中的稳定字符串 `"CameraClearChannel"` 作为锚点，找到引用该字符串的代码，再从附近调用中识别一个形如“读取 RIP-relative 全局变量并返回”的 getter。

完整流程为：

```text
搜索 "CameraClearChannel\0"
    ↓
寻找 LEA reg,[RIP+disp32] 对该字符串的引用
    ↓
在 LEA 前 0x200 字节内寻找 CALL rel32
    ↓
检查 CALL 目标是否为 MOV r64,[RIP+disp32]; RET
    ↓
解析 MOV 的 disp32，得到 CameraManager 全局槽地址
    ↓
连续解引用三次
    ↓
读取最终对象 +0x68 和 +0x6C 的两个 float
    ↓
X = rawX，Y = rawY + 29.0f
```

因此，这套方法可分为两部分：

1. `ScanCameraManagerGlobal()` 负责定位全局槽。
2. `ReadArtifactCoords()` 负责沿指针链读取坐标。

---

## 二、为什么使用 CameraClearChannel

`"CameraClearChannel"` 是相机系统代码中的静态字符串。与容易随链接布局变化的函数 RVA 相比，只要新版客户端仍保留这段名称，它就可以充当稳定的语义锚点。

代码搜索的是包含终止符的完整字节串：

```cpp
static const char kStr[] = "CameraClearChannel";
constexpr size_t kStrLen = sizeof(kStr);
```

因为 `sizeof(kStr)` 包含最后的 `\0`，实际匹配内容是：

```text
43 61 6D 65 72 61 43 6C 65 61 72 43 68 61 6E 6E 65 6C 00
C  a  m  e  r  a  C  l  e  a  r  C  h  a  n  n  e  l \0
```

扫描范围来自 `SC2_x64.exe` PE 头的 `SizeOfImage`，所以搜索覆盖当前进程中整个模块映像，而不是硬编码 `.rdata` 的范围：

```cpp
auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
size_t imgSz = nt->OptionalHeader.SizeOfImage;
```

找到的第一个完整字符串地址保存为 `strAddr`。若字符串不存在，函数直接返回 `0`。

---

## 三、寻找字符串的代码引用

### 3.1 RIP-relative LEA

x64 程序通常通过 RIP-relative `LEA` 获取静态字符串地址：

```asm
lea reg, [rip+disp32]
```

常见机器码结构为：

```text
48 8D modrm xx xx xx xx
```

代码先寻找 `48 8D`，再检查 ModRM 的 `r/m` 字段是否为 `101b`：

```cpp
if (base[i] != 0x48 || base[i + 1] != 0x8D) continue;
if ((base[i + 2] & 0x07) != 0x05) continue;
```

随后读取 `disp32`，按下一条指令地址计算 LEA 的目标：

```cpp
int32_t disp = *reinterpret_cast<const int32_t*>(base + i + 3);
uintptr_t target = reinterpret_cast<uintptr_t>(base + i + 7) + disp;
```

公式为：

```text
target = instructionAddress + instructionLength + sign_extend(disp32)
       = LEA + 7 + sign_extend(disp32)
```

只有 `target == strAddr` 时，才将该 LEA 视为 `"CameraClearChannel"` 的真实引用。这个目标地址比较非常重要，可以过滤模块中偶然出现的 `48 8D` 字节序列。

### 3.2 当前 ModRM 检查的精度

注释要求的是 `mod=00, r/m=101`，即 RIP-relative 寻址；但当前实现只检查了低三位 `r/m=101`，没有显式检查高两位 `mod=00`。

更严格的条件应为：

```cpp
(base[i + 2] & 0xC7) == 0x05
```

当前代码仍会继续用目标地址是否等于 `strAddr` 做二次筛选，因此误判概率较低，但分析和移植时应知道注释描述比实际条件更严格。

---

## 四、从附近 CALL 找到全局 getter

### 4.1 搜索窗口

找到字符串引用后，代码在该 LEA 之前最多 `0x200` 字节内搜索 `E8`：

```cpp
size_t lo = (i > 0x200) ? (i - 0x200) : 0;
for (size_t j = lo; j < i; ++j)
{
    if (base[j] != 0xE8) continue;
```

`E8 disp32` 是 x86-64 的 near relative call：

```asm
call rel32
```

调用目标计算公式为：

```text
callee = CALL + 5 + sign_extend(rel32)
```

对应实现：

```cpp
int32_t rel = *reinterpret_cast<const int32_t*>(base + j + 1);
auto callee = base + j + 5 + rel;
```

计算后还会检查 `callee` 到 `callee+8` 是否位于模块映像内，避免读取模块外地址。

### 4.2 getter 的机器码特征

并非窗口中的任意 CALL 都可接受。其目标必须恰好是一个 8 字节叶函数：

```asm
mov r64, [rip+disp32]
ret
```

机器码结构为：

```text
48 8B ?5 xx xx xx xx C3
```

验证代码为：

```cpp
if (callee[0] == 0x48 && callee[1] == 0x8B &&
    (callee[2] & 0xC7) == 0x05 && callee[7] == 0xC3)
```

这里对 ModRM 使用 `0xC7` 掩码：

- 保留 `mod` 两位，要求为 `00`。
- 忽略中间的 `reg` 三位，因此允许目标寄存器是任意 64 位通用寄存器。
- 保留 `r/m` 三位，要求为 `101`，即 RIP-relative。

它可以匹配以下任意一种 getter：

```asm
mov rax, [rip+disp32]
mov rcx, [rip+disp32]
mov rdx, [rip+disp32]
...
mov rdi, [rip+disp32]
```

只要该函数紧接着 `ret` 即可。

---

## 五、解析 CameraManager 全局槽

getter 中的 `MOV` 长度为 7 字节：

```text
48 8B modrm disp32
```

代码从 `callee+3` 读取有符号位移，并以 MOV 后一条指令的地址为基准计算目标：

```cpp
int32_t ripDisp = *reinterpret_cast<const int32_t*>(callee + 3);
return reinterpret_cast<uintptr_t>(callee + 7) + ripDisp;
```

公式为：

```text
globalSlot = MOV + 7 + sign_extend(disp32)
```

函数返回的是全局变量自身的地址，即保存指针的 qword 槽，不是 CameraManager 对象地址。也就是说：

```text
sCamGlobal = &globalPointer
```

ASLR 只会改变模块实际加载基址和 RIP 位移的运行时结果，不会影响这种现场解析方式。

---

## 六、沿指针链读取 ArtifactCoords

扫描结果被静态局部变量缓存，只在 `ReadArtifactCoords()` 首次调用时扫描一次：

```cpp
static uintptr_t sCamGlobal = ScanCameraManagerGlobal();
```

随后执行三次 64 位解引用：

```cpp
UINT64 addr = ReadMemory<UINT64>(sCamGlobal); // 第一次：读取全局槽
addr = ReadMemory<UINT64>(addr);              // 第二次：+0x00
addr = ReadMemory<UINT64>(addr);              // 第三次：+0x00
```

可表示为：

```text
sCamGlobal
    │
    └─ *sCamGlobal        → level1
          └─ *(level1+0)  → level2
                └─ *(level2+0) → coordinateObject
```

最终对象字段布局按当前代码解释为：

```text
coordinateObject + 0x68  float rawX
coordinateObject + 0x6C  float rawY
```

返回值计算为：

```cpp
result.x = ReadMemory<float>(addr + 0x68);
result.y = ReadMemory<float>(addr + 0x6C) + 29.0f;
```

因此最终暴露给渲染代码的坐标是：

```text
artifactX = rawX
artifactY = rawY + 29.0
```

`+29.0f` 是当前功能中的 Y 轴校正值，并不是从 SC2 对象读取出来的字段。判断 `+0x68/+0x6C` 是否仍表示目标坐标时，应先观察未校正的 `rawX/rawY`，再单独验证校正量。

---

## 七、代码能证明什么

根据当前实现，可以直接确认：

1. 全局槽来自 `"CameraClearChannel"` 引用之前的某个无参数 getter。
2. getter 返回一个 RIP-relative qword 全局变量的值。
3. 代码把该全局变量地址作为三级指针链的根。
4. 最终对象的 `+0x68/+0x6C` 被作为两个 `float` 读取。
5. 这两个值用于调试显示，并通过 `ArtifactToScreen()` 转换后绘制小地图黄点。

但仅靠这段源码不能独立证明：

- 该全局变量在 SC2 内部的正式类型名一定是 `CameraManager`。
- 指针链中的三个对象分别是什么 C++ 类型。
- `+0x68/+0x6C` 在原始类定义中的字段名。
- `+29.0f` 对所有地图和客户端版本都正确。

这些语义需要通过 IDA 调用关系、运行时内存观察和实际小地图位置共同验证。“ArtifactCoords”是本项目赋予最终结果的业务名称，不是扫描时使用的游戏符号。

---

## 八、运行时验证方法

### 8.1 验证扫描结果

在调试日志中记录以下地址：

```text
CameraClearChannel 字符串地址
LEA 指令地址
CALL 指令地址
callee getter 地址
sCamGlobal 全局槽地址
```

然后在 IDA 中逐项确认：

- LEA 的计算目标确实是字符串地址。
- CALL 与 LEA 位于同一函数或同一基本调用上下文中。
- callee 确实只有 `MOV r64,[RIP+disp32]; RET`。
- MOV 的目标是可读的 8 字节全局槽。

### 8.2 验证指针链

分别记录三级解引用结果和原始坐标：

```text
level1
level2
coordinateObject
rawX
rawY
```

检查要求：

- 三个指针均非空、对齐且位于合理的用户态地址范围。
- `rawX/rawY` 是有限浮点数，不是 NaN 或 Infinity。
- 游戏状态稳定时坐标不会随机剧烈跳变。
- 神器位置发生变化时，对应字段以符合预期的方式变化。
- 在小地图上绘制的点与目标位置一致。

### 8.3 验证 Y 轴校正

分别比较 `rawY` 和 `rawY+29.0f` 的投影位置。若不同地图或分辨率需要不同校正，应将 `29.0f` 视为地图坐标校准参数，而不是继续归因于 CameraManager 内存布局。

---

## 九、当前实现的风险与限制

### 9.1 取第一个匹配

字符串扫描返回第一个 `"CameraClearChannel\0"`，LEA 扫描也按地址升序处理。CALL 窗口内同样返回第一个满足 getter 结构的目标。如果模块中存在多个字符串副本、多个引用或多个相同 getter，代码没有做唯一性统计。

### 9.2 CALL 搜索不是反汇编

代码逐字节搜索 `0xE8`，没有解析指令边界。因此立即数或位移中的偶然 `E8` 也会被当作 CALL 候选。callee 的模块范围和 8 字节 getter 结构检查降低了误命中概率，但不能像真正的反汇编器一样完全排除假阳性。

### 9.3 固定搜索窗口

代码假设 getter CALL 位于字符串 LEA 之前 `0x200` 字节内。编译器重排、函数内联或日志代码扩张都可能使调用超出窗口，导致新版本扫描失败。

### 9.4 扫描失败会被永久缓存

`sCamGlobal` 使用静态局部初始化。如果首次调用发生在 `SC2_x64.exe` 或相关状态尚未准备好时，扫描结果 `0` 也会被永久缓存，本次 DLL 生命周期内不会重试。

### 9.5 坐标读取失败不一定使 valid 为 false

`ReadMemory<T>()` 在 `SafeMemcpy()` 失败时返回零值，但 `ReadArtifactCoords()` 只对三级指针做非空检查，没有单独检查两个 float 是否读取成功。只要最终指针非空，即使字段读取失败得到 `0.0f`，代码仍会设置 `valid = true`。

---

## 十、新版本重新定位流程

若 SC2 更新后神器坐标失效，可按以下顺序检查：

1. 搜索 `"CameraClearChannel"` 是否仍存在。
2. 查看该字符串的所有 xref，而不是只检查第一处。
3. 在引用函数中查找返回全局 qword 的短 getter CALL。
4. 确认 getter 是否仍为 `MOV r64,[RIP+disp32]; RET`。
5. 若 getter 被内联，直接从引用函数中解析相应 RIP-relative MOV。
6. 重新确认全局槽后的指针链层数。
7. 在 ReClass、Cheat Engine 或调试器中观察最终对象附近的 float 字段。
8. 分别验证 `+0x68`、`+0x6C` 和 `+29.0f`，不要一次性把三者视为同一结论。
9. 在至少两局或两个地图场景中比较内存值和小地图实际位置。
10. 确认成功后再更新扫描条件、指针层数或字段偏移。

若字符串仍稳定，但附近存在多个候选 getter，推荐升级扫描算法：枚举全部字符串 xref 和 CALL 候选，对每个候选执行严格函数结构检查，再用指针链可读性与有限浮点数范围进行运行时筛选。

---

## 十一、相关文件

| 文件 | 功能 |
|------|------|
| `Dll1/core/game_data.cpp` | 扫描 CameraManager 全局槽并读取坐标 |
| `Dll1/core/game_data.h` | `ArtifactCoords` 数据结构定义 |
| `Dll1/core/memory.h` | `SafeMemcpy()` 与 `ReadMemory<T>()` |
| `Dll1/render/menu.cpp` | 显示坐标并将其投影到小地图 |
| `docs/camera_bounds_scan_analysis.md` | 读取小地图投影所需的运行时地图边界 |