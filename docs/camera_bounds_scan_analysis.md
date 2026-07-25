# SC2 Camera Bounds 定位与特征码扫描分析

> **涉及版本**: Base96921、Base96999、Base97579 (`SC2_x64.exe`)  
> **目标代码**: `Dll1/core/game_data.cpp` 中的 `ScanCameraBoundsGetter()` 与 `ReadCurrentMapBounds()`

---

## 一、目标：读取当前地图的 Camera Bounds

`CameraSetBounds` 会取得一个由四个 `int32_t` 组成的矩形，并用它更新相机可移动范围。当前实现需要定位负责返回该矩形地址的内部 getter：

```cpp
using BoundsGetterFn = uintptr_t(__fastcall*)(uint8_t index);
```

返回地址指向以下布局：

```text
+0x00  int32_t minX
+0x04  int32_t minY
+0x08  int32_t maxX
+0x0C  int32_t maxY
```

部分版本以普通地图坐标保存这些值，部分版本使用 `1/4096` 定点数。`BuildBoundsFromRect()` 会根据数值大小选择缩放比例，并检查矩形宽高和坐标范围。

---

## 二、从 CameraSetBounds 定位 getter

### 2.1 找到 CameraSetBounds

优先从 IDA 的 native 注册表、函数名、字符串交叉引用或旧版本已知地址定位 `CameraSetBounds`。若新版没有符号，可采用旧版与新版二进制对比，寻找包含以下行为的函数：

1. 接收四个边界坐标或一个边界对象。
2. 调用一个只使用 `CL` 作为索引的短函数。
3. 对返回地址执行四个连续的 32 位读取。
4. 将结果写入相机 clamp/viewport 状态。

Base96921 的分析中，`CameraSetBounds` 通过 `sub_14055CB00` 解析相机边界矩形。进入该调用链后，应继续跟踪到实际计算并返回矩形地址的叶函数，而不是停在只负责转发参数的包装函数。

### 2.2 确认 getter 的函数语义

目标叶函数具有三个稳定特征：

1. 第一个参数来自 `RCX`，但函数只读取 `CL`，因此参数实际是 `uint8_t`。
2. 索引经过加偏置和左移 4 位，说明每个槽大小为 16 字节。
3. 函数返回 `computedBase + slotIndex * 16`，正好指向四个 `int32_t`。

典型后半段反汇编为：

```asm
movzx eax, cl              ; uint8_t index
add   rax, slotBias
shl   rax, 4               ; 每个槽 16 字节
add   rax, computedBase
retn
```

其等价伪代码为：

```cpp
return computedBase + (static_cast<uint8_t>(index) + slotBias) * 16;
```

在 Base97579 中 `slotBias` 为 `0x1F`。该立即数可能随版本变化，不能作为函数身份的唯一依据。

---

## 三、在 IDA 中提取 pattern

### 3.1 Base97579 函数形态

Base97579 的 getter 前半段通过四个 RIP-relative 全局值计算基址：

```asm
mov eax, [rip+globalA]
mov edx, [rip+globalB]
not eax
add eax, [rip+globalC]
add edx, [rip+globalD]
mov [rsp+10h], eax
movzx eax, cl
add rax, 1Fh
mov [rsp+14h], edx
shl rax, 4
add rax, [rsp+10h]
retn
```

从 IDA Hex View 或反汇编的 opcode 列复制机器码后，原始字节结构为：

```text
8B 05 xx xx xx xx       mov eax,[rip+globalA]
8B 15 xx xx xx xx       mov edx,[rip+globalB]
F7 D0                   not eax
03 05 xx xx xx xx       add eax,[rip+globalC]
03 15 xx xx xx xx       add edx,[rip+globalD]
89 44 24 10             mov [rsp+10h],eax
0F B6 C1                movzx eax,cl
48 83 C0 1F             add rax,1Fh
89 54 24 14             mov [rsp+14h],edx
48 C1 E0 04             shl rax,4
48 03 44 24 10          add rax,[rsp+10h]
C3                      retn
```

将四处 RIP-relative `disp32` 和版本相关的 `slotBias` 替换为通配符，得到：

```text
8B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? F7 D0
03 05 ?? ?? ?? ?? 03 15 ?? ?? ?? ?? 89 44 24 10
0F B6 C1 48 83 C0 ?? 89 54 24 14 48 C1 E0 04
48 03 44 24 10 C3
```

### 3.2 为什么这些字节必须通配

RIP-relative 指令的目标计算公式为：

```text
target = nextInstructionAddress + sign_extend(disp32)
```

链接布局、代码大小或全局变量位置变化都会改变 `disp32`，所以以下指令后的四个字节必须写成 `?? ?? ?? ??`：

```text
8B 05 [disp32]
8B 15 [disp32]
03 05 [disp32]
03 15 [disp32]
```

`48 83 C0 1F` 的最后一个字节是槽偏置。它不决定函数的核心结构，因此也通配为 `48 83 C0 ??`。

其余字节描述寄存器数据流、16 字节槽大小和返回方式，保留它们可以降低误命中概率。

### 3.3 Base96999 的变体

Base96999 重新内联了基址计算：一部分全局运算被替换为 `add eax, imm32`，随后通过 `add edx, eax` 合并：

```asm
mov eax, [rip+globalA]
mov edx, [rip+globalB]
add eax, versionConstant
add edx, eax
movzx eax, cl
mov [rsp+10h], edx
add rax, slotBias
mov edx, [rip+globalC]
add edx, [rip+globalD]
mov [rsp+14h], edx
shl rax, 4
add rax, [rsp+10h]
retn
```

对应 pattern 将全局位移、`versionConstant` 和 `slotBias` 全部通配：

```text
8B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 05 ?? ?? ?? ??
03 D0 0F B6 C1 89 54 24 10 48 83 C0 ??
8B 15 ?? ?? ?? ?? 03 15 ?? ?? ?? ?? 89 54 24 14
48 C1 E0 04 48 03 44 24 10 C3
```

### 3.4 Base96921 及更早版本

旧版本采用另一套基址混淆运算，并包含当时固定的 `add eax, 0x6B3E6FE3`：

```text
8B 05 ?? ?? ?? ?? 2B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ??
05 E3 6F 3E 6B 03 15 ?? ?? ?? ?? 89 44 24 14 0F B6 C1
48 83 C0 ?? 89 54 24 10 48 C1 E0 04 48 03 44 24 10 C3
```

该常量目前被保留以提高旧版 pattern 的唯一性。若要支持更早的未知版本，应先确认只通配该常量仍能保持唯一命中，再修改代码。

---

## 四、确认 pattern 找对了函数

仅在二进制中命中并不足以证明目标正确，应进行以下静态和运行时验证。

### 4.1 静态验证

- pattern 在目标 `SC2_x64.exe` 中应当只命中一次。
- 命中地址应是函数入口，而不是另一个函数内部的相似指令片段。
- IDA 应将其识别为短叶函数，尾部为 `shl rax,4`、基址相加和 `retn`。
- 调用者传入的第一个参数应位于 `RCX`，目标函数只读取 `CL`。
- 调用者应把返回值当作地址，并从中读取四个连续的 `int32_t`。

如果 pattern 命中多次，不应直接使用 `PatternScan()` 返回的第一个地址。应增加调用者、函数边界或返回数据验证，或者保留更多稳定 opcode 来收紧特征。

### 4.2 运行时验证

当前代码先调用索引 `16`。它在已分析版本中代表全局/当前地图的 camera clamp：

```cpp
uintptr_t rect = getter(16);
```

返回值需要满足：

1. 地址非空且 16 字节可读。
2. 四个值可解释为 `minX, minY, maxX, maxY`。
3. 若原始绝对值大于 `4096`，按 `1/4096` 缩放。
4. 宽高均在 `32` 到 `512` 之间。
5. 坐标位于 `[-64, 640]` 的合理范围。

索引 `16` 验证失败时，当前实现还会尝试 `0..15`，以兼容槽位选择变化。成功结果会记录实际使用的 `index`、getter 地址、矩形地址和四个原始值，便于日志诊断。

函数调用由 SEH 包裹，错误地址或 getter 内部异常会返回空指针，避免直接使注入进程崩溃。但 SEH 只负责容错，不能替代 pattern 唯一性和返回数据验证。

---

## 五、新版本重新定位流程

SC2 更新后若三套 pattern 都失配，可按以下顺序重新定位：

1. 在旧版 IDA 中打开已确认的 getter，记录完整反汇编、调用者和函数图。
2. 在新版中定位 `CameraSetBounds`，沿调用链进入实际返回矩形地址的叶函数。
3. 若名称和字符串均不可用，使用 BinDiff/Diaphora，或搜索稳定尾部 `movzx ?,cl`、索引加偏置、`shl ?,4`、基址相加、`ret`。
4. 检查函数返回值是否被读取为四个连续的 32 位整数。
5. 从新版函数入口复制完整机器码。
6. 通配所有 RIP-relative `disp32`、绝对地址、重定位项和已确认会变化的立即数。
7. 保留描述数据流的 opcode、16 字节步长和函数结尾。
8. 在整个模块中统计命中数，要求唯一命中。
9. 注入后先记录 getter 地址和 `getter(16)` 的四个原始值，不立即依赖结果执行其他功能。
10. 对照游戏地图边界并通过 `BuildBoundsFromRect()` 校验后，再加入正式 fallback 链。

若新版编译器仅改变寄存器分配或指令顺序，继续堆叠整函数 pattern 会越来越脆弱。此时更稳妥的方案是从 `CameraSetBounds` 的稳定字符串/native 注册入口定位调用者，再解析其 `CALL rel32`，最后对被调函数做结构验证。

---

## 六、常见误区

### 6.1 直接使用 IDA 显示地址

`0x140...` 是特定构建中的 VA，模块受 ASLR 和版本布局影响。运行时必须使用 pattern、RVA 加模块基址，或解析 RIP-relative 引用，不能硬编码完整 VA。

### 6.2 把所有立即数都保留下来

全局位移和混淆常量经常随构建变化。保留过多会导致每次更新都失配；通配过多则会误命中。判断标准应是该字节是否表达稳定语义，而不是它在当前版本中是否恰好固定。

### 6.3 只验证“地址可读”

错误函数也可能返回可读地址。必须同时验证四个值的顺序、宽高、坐标范围，并最好在不同大小地图上对比结果。

### 6.4 把 fallback 索引当成定位依据

`0..16` 中某个索引偶然返回合理矩形，并不能证明 getter 正确。索引探测只用于适配槽位变化，函数身份仍需由调用链、反汇编结构和唯一命中共同确认。

---

## 七、现有实现对应关系

| 实现 | 作用 |
|------|------|
| `ScanCameraBoundsGetter()` | 按 Base97579、Base96999、Legacy 顺序扫描 getter |
| `CallBoundsGetter()` | 以 `uint8_t` 参数调用 getter，并用 SEH 捕获异常 |
| `BuildBoundsFromRect()` | 读取四个原始值、处理定点缩放并验证矩形 |
| `ReadCurrentMapBounds()` | 优先尝试索引 16，失败后尝试 0 到 15 |
| `PatternScan()` | 解析 IDA 风格 pattern，并在模块映像中返回首个命中地址 |

---

## 八、新版本验证清单

- [ ] 在新版中重新确认 `CameraSetBounds` 调用链。
- [ ] 确认命中函数只使用 `CL` 作为槽索引。
- [ ] 确认槽步长仍为 16 字节，即尾部仍有左移 4 位或等价运算。
- [ ] 确认返回值仍指向四个连续的 `int32_t`。
- [ ] 确认所用 pattern 在完整模块中唯一命中。
- [ ] 记录索引 16 和 0 到 15 的原始返回值。
- [ ] 在至少两张边界尺寸不同的地图中验证 `left/top/right/bottom`。
- [ ] 验证 pattern 未命中、getter 异常和矩形拒绝三种失败状态。
- [ ] 将新 pattern 放在对应版本 fallback 之前，并保留旧版兼容项。

---

## 九、相关文件

| 文件 | 功能 |
|------|------|
| `Dll1/core/game_data.cpp` | getter 扫描、调用和矩形验证 |
| `Dll1/core/game_data.h` | `MapBounds` 与 `MapBoundsStatus` 定义 |
| `Dll1/core/memory.h` | `PatternScan()`、`SafeMemcpy()` 和安全内存读取 |