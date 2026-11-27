# EXR 有损存档中的数值恢复：B44/B44A 与 PXR24 的实际读写路径分析

本文只讨论本仓库 `src/lib/OpenEXRCore/`（真正执行编解码的代码）与 `src/lib/OpenEXR/`（C++ 门面）中的实现，
所有数值均由编译本仓库后实测得到，可用文末脚本在不编译的情况下逐位复算。

结论先行：

* **通道在文件里的类型**（`Channel.type` / `exr_attr_chlist_entry_t.pixel_type`）决定走哪个编码分支。
  这是唯一在"落盘"阶段就决定信息是否可逆的因素。
* **感知线性标记 `pLinear`** 只对 **B44/B44A 且文件通道为 HALF** 有影响；对 PXR24 完全无效；对 B44 的 FLOAT/UINT 通道也无效。
* **读入目标类型**（framebuffer `Slice.type`）是**独立的第二次舍入**，与压缩无关。它在"读入帧缓冲之前"就已经不可逆——
  即使文件通道是 FLOAT 且被 B44 原样保存，读成 HALF 也会丢失。
* `pLinear` 在 B44 中同时改变三件事（不只是量化精度）：log/exp 映射、块最大值是否精确保留（`exactmax`）、
  以及（仅 B44A）flat-field 3 字节块能否触发。因此**任何在落盘后修改该标记的行为都会把数值解坏**，而不是"换个精度"。
* 小块 / 边缘 tile 可能整块未压缩（raw fallback），此时 `pLinear` 完全不起作用。判断"是否被有损处理"要看 chunk 的
  `packed_size != unpacked_size`，不能只看 header 的 compression 字段。

---

## 1. 三个因素各自发生在哪一阶段

### 1.1 阶段划分

数据从合成师的内存到磁盘再回到帧缓冲，经过 4 个阶段：

| 阶段 | 代码位置 | 看到的信息 | 是否已有损 |
|---|---|---|---|
| A. 打包（pack） | `src/lib/OpenEXRCore/pack.c:21` `default_pack` | 帧缓冲类型 `user_data_type` → 文件类型 `data_type` | 若两者不同则在此**第一次**有损/无损转换 |
| B. 压缩（compress） | `internal_b44.c:340` `compress_b44_impl` / `internal_pxr24.c:90` `apply_pxr24_impl` | 仅文件类型 `data_type`、`p_linear` | 有损编码在这里发生 |
| C. 解压（uncompress） | `internal_b44.c:523` `uncompress_b44_impl` / `internal_pxr24.c:261` `undo_pxr24_impl` | 文件类型、`p_linear`、`data_type` | 解出的已是"文件里存的近似值" |
| D. 解包（unpack） | `src/lib/OpenEXRCore/unpack.c`（宏 `UNPACK_*`，见 `unpack.c:1092-1138`） | 文件类型 `data_type` → 帧缓冲类型 `user_data_type` | 若两者不同则在此**第二次**有损（HALF 舍入） |

关键点：B 阶段**只读 `data_type` 和 `p_linear`，从不看 `user_data_type`**。
读入目标类型 `user_data_type` 是 D 阶段的事，与压缩算法完全无关。这两件事可以在同一次读操作中叠乘。

`exr_coding_channel_info_t` 的字段定义把这一点写得很直白：

```
src/lib/OpenEXRCore/openexr_coding.h:78   uint8_t  p_linear;          // "Linear flag from channel definition (used by b44)"
src/lib/OpenEXRCore/openexr_coding.h:86   uint16_t data_type;         // 文件里的类型（由 header 决定，代码侧只读）
src/lib/OpenEXRCore/openexr_coding.h:102  uint16_t user_data_type;    // 帧缓冲里的类型（调用者填写）
```

`coding.c:51` / `coding.c:107` 从 header 的 `curc->p_linear` 填 `p_linear`；
`coding.c:57-58` 把 `user_bytes_per_element` / `user_data_type` 默认成与文件相同。
C++ 侧只有在 `ImfScanLineInputFile.cpp:678-679`、`ImfTiledInputFile.cpp:950-951` 才把 `Slice.type` 写进 `user_data_type`。

### 1.2 文件通道类型决定分支

B44 编码器对每个通道先判类型：

```
src/lib/OpenEXRCore/internal_b44.c:410   if (curc->data_type != EXR_PIXEL_HALF)
                                          { memcpy(out, scratch, nBytes); ... continue; }   // FLOAT/UINT 原样搬运
src/lib/OpenEXRCore/internal_b44.c:421   for (y += 4) for (x += 4) { ... pack(...) }        // 仅 HALF 走 4x4 有损块
```

所以 **B44 对 FLOAT / UINT 通道是位精确的**（实测见 §4.2）。
PXR24 反过来：只有 FLOAT 通道是有损的，HALF / UINT 按元素做整数差分、拆成字节平面后交给 deflate，是**无损**的：

```
src/lib/OpenEXRCore/internal_pxr24.c:148  case EXR_PIXEL_HALF:  // 16-bit 位模式做整数差分，拆成 2 个字节平面，deflate 后无损
src/lib/OpenEXRCore/internal_pxr24.c:114  case EXR_PIXEL_UINT:  // 32-bit 整数差分，拆成 4 个字节平面，无损
src/lib/OpenEXRCore/internal_pxr24.c:176  case EXR_PIXEL_FLOAT: // float_to_float24(): 尾数截到 15 位
src/lib/OpenEXRCore/internal_pxr24.c:203      pixel24 = float_to_float24 (v.f);
```

`float_to_float24()`（`internal_pxr24.c:18-85`）保留 sign+exponent+15 位尾数（带一次 round-to-nearest，最低位为隐藏位的一半时进位），
即 float32 的 24 个高位，然后整体左移 8 位写盘。**它不读 `p_linear`**。

### 1.3 `pLinear` 只在 B44 的 HALF 分支起作用

全仓库 `p_linear` 的字段定义与读取点只有下面这些：

```
src/lib/OpenEXRCore/coding.c:51,107     填充（两处）
src/lib/OpenEXRCore/openexr_coding.h:78 定义
src/lib/OpenEXRCore/internal_b44.c:484-486   ← 唯一实际影响数值的读取
src/lib/OpenEXRCore/internal_b44.c:586       ← 解码侧对称
src/lib/OpenEXRCore/internal_dwa_compressor.h:419,1237   ← DWA 的 LUT 选择
```

B44 编码：

```c
// src/lib/OpenEXRCore/internal_b44.c:484-486
if (curc->p_linear) convertFromLinear (s);        // s[i] = expLut[s[i]]
wcount = pack (s, out, flat_field, !(curc->p_linear));
```

B44 解码：

```c
// src/lib/OpenEXRCore/internal_b44.c:586
if (curc->p_linear) convertToLinear (s);          // s[i] = logLut[s[i]]
```

LUT 定义在 `internal_b44_table_init.c:17-41`：

```c
b44_convertFromLinear(x):  NaN/Inf -> 0;  x >= 0x558c -> 0x7bff (HALF_MAX);  否则 half(exp(half(x)/8))
b44_convertToLinear(x):    NaN/Inf -> 0;  x  > 0x8000 -> 0;                 否则 half(8*ln(half(x)))  // 无零值保护：ln(±0) -> -inf -> 0xfc00
```

于是 `pLinear=1` 在 B44 中同时改变三件事：

1. **log/exp 映射**（上面两个 LUT）。
2. **`exactmax` 反转**：`pack()` 的第 4 个实参是 `!(curc->p_linear)`。
   `exactmax=1` 时 `t[0] = tMax - (d[0] << shift)`（`internal_b44.c:203-210`），把块内最大值钉得更准；
   `pLinear=1` 时 `exactmax=0`，不做这个修正。这一项与 LUT 无关：随机取样 20000 个块，
   在 LUT 已应用的前提下再把 `exactmax` 从 1 改成 0，有 18363 个块的输出继续改变。
   所以**即使 LUT 恰好是恒等映射，开关本身也会改变输出**（注意：§3 的定例恰好是少数不受 `exactmax`
   影响的块，那里两种 `exactmax` 给出相同结果——不能因此认为 `exactmax` 无影响）。
3. **flat-field 3 字节块**：`flatfields` 参数只在 B44A 为 1（`internal_b44.c:509/515`）。
   `pLinear=1` 时 LUT 会把相近的暗部值压成同一个值，从而可能让原本非平坦的块满足 `rMin==rMax==bias`
   而退化成 3 字节块（`internal_b44.c:187-200`）——这会让 B44A 的文件更小，同时把暗部差异抹平。

`PXR24` 与 `B44` 的 FLOAT/UINT 分支都没有任何 `p_linear` 引用。

### 1.4 读入目标类型是压缩之外的舍入

`unpack.c` 的 `UNPACK_*` 宏按 `data_type × user_data_type` 分支。与本问题最相关的一条：

```c
// src/lib/OpenEXRCore/unpack.c:1121-1129
#define UNPACK_FLOAT_TO_HALF_SAMPLES(samps) \
    ... store_native16 (cdata, float_to_half_int (fint)); ...
```

也就是说，**文件里是 FLOAT（B44 位精确保存、或 PXR24 保存了 24 位）时，读成 HALF 依然会丢精度**。
这一步发生在解压之后、写帧缓冲的那一瞬，无从恢复。

反向（文件 HALF → 读成 FLOAT）是**无损扩展**：`UNPACK_HALF_TO_FLOAT_SAMPLES`（`unpack.c:1101-1109`）只是
`half_to_float_int`，精度不会比文件里更好，也不会更差。

另外两个与"读"有关但容易被误认为"读入类型问题"的点：

* `pLinear` 在文件 header 的 channel 表里（`ImfChannelListAttribute.cpp:72/112`、`write_header.c:200`）。
  **读的时候取的是文件里的标记**，不是调用者可以覆盖的东西。C++ `InputFile` 没有覆盖入口。
* `pLinear` 不是"读的时候再做一次映射"的开关：编码时若 `pLinear=1`，内存值先被 exp 映射才进入 pack；
  解码时先 unpack 再 log 映回。落盘后改标记，等于用错的逆映射去解，结果不是"精度变差"，而是**数值完全错位**（§4.3 实测）。

---

## 2. 一组可复算的输入

为了让每种组合都有唯一答案，固定同一组 4×4 数据（row0..row3，即 `s[0..3]` 是第 0 行）：

* 一个高光：`z[0] = 1000.0`
* 其余全为暗部：`z[1..15] = 0.001`

HALF 位模式：`z[0] = 0x63d0`，`z[1] = 0x1419`（0.001 在 HALF 里最近的表示是 0.0010004）。

这个输入踩中两个边界：高光远高于 `pLinear=1` 的 88.75 上限；暗部低于 `pLinear=1` 下塌缩为 0 的 0.0039 阈值。

它之所以在 `pLinear=0` 下也丢暗部，原因不是"绝对动态范围大"，而是 **B44 的 `shift` 由块内最大绝对差决定**：
`pack()` 每轮把 `tMax - t[i]` 右移一位，直到所有 6-bit 差分落入 0..63。块内跨度约 6 个数量级时 `shift` 被推到 10，
暗部相邻像素的差（几个计数）整段右移后归零。反过来，**低动态范围的块可以完全无损**：
实测全部 1000.0 的平块、1000 与 999 混排的块、以及 0.25 / 1.0 的平块，在 `pLinear=0` 下都逐位精确往返。
所以"B44 只保相对精度"这个直觉是对的，但它的单位是**块内跨度**，不是图像的整体位深。

---

## 3. 逐位模型（与代码一一对应）

下面这段 Python 只用标准库，`python3 -I` 直接运行（`-I` 避免加载工作目录里的同名模块）。
它逐行复刻：

* `internal_b44_table_init.c:17-41` 的两个 LUT
* `internal_b44.c:104-233` 的 `pack()`（含 `shiftAndRound`、`exactmax`、flat-field）
* `internal_b44.c:238-335` 的 `unpack14()` / `unpack3()`
* `internal_pxr24.c:18-85` 的 `float_to_float24()`

```python
import struct, math

def f2h(x):
    try: return struct.unpack('<H', struct.pack('<e', x))[0]
    except OverflowError: return 0x7c00 if x > 0 else 0xfc00
def h2f(b): return struct.unpack('<e', struct.pack('<H', b & 0xffff))[0]

# internal_b44_table_init.c: b44_convertFromLinear / b44_convertToLinear
def expLut(x):
    if (x & 0x7c00) == 0x7c00: return 0
    if 0x558c <= x < 0x8000:   return 0x7bff
    return f2h(math.exp(h2f(x) / 8.0))
def logLut(x):
    if (x & 0x7c00) == 0x7c00: return 0
    if x > 0x8000:             return 0
    f = h2f(x)
    if f == 0.0: return 0xfc00          # 8*logf(+-0) = -inf -> half(-inf)
    return f2h(8.0 * math.log(f))

# internal_b44.c: pack()/unpack14()
def _t(s):
    if (s & 0x7c00) == 0x7c00: return 0x8000
    return (~s) & 0xffff if (s & 0x8000) else (s | 0x8000)
def _sar(x, sh):
    x <<= 1; a = (1 << sh) - 1; sh += 1; b = (x >> sh) & 1
    return (x + a + b) >> sh
def b44_pack(s, flatfields=0, exactmax=1):
    t = [_t(v) for v in s]; tMax = max(t); bias, sh = 0x20, -1
    while True:
        sh += 1
        d = [_sar(tMax - t[i], sh) for i in range(16)]
        r = [d[0]-d[4]+bias, d[4]-d[8]+bias, d[8]-d[12]+bias,
             d[0]-d[1]+bias, d[4]-d[5]+bias, d[8]-d[9]+bias, d[12]-d[13]+bias,
             d[1]-d[2]+bias, d[5]-d[6]+bias, d[9]-d[10]+bias, d[13]-d[14]+bias,
             d[2]-d[3]+bias, d[6]-d[7]+bias, d[10]-d[11]+bias, d[14]-d[15]+bias]
        if min(r) >= 0 and max(r) <= 0x3f: break
    if min(r) == bias and max(r) == bias and flatfields:
        return ('flat3', t[0], 0, None)
    t0 = t[0] if not exactmax else (tMax - (d[0] << sh)) & 0xffff
    return ('b14', t0, sh, r)
def b44_unpack(p):
    kind, t0, sh, r = p
    if kind == 'flat3':
        s0 = (t0 & 0x7fff) if (t0 & 0x8000) else (~t0) & 0xffff
        return [s0] * 16
    bias, s = (0x20 << sh) & 0xffff, [0]*16
    s[0] = t0
    for dst, src, k in [(4,0,0),(8,4,1),(12,8,2),(1,0,3),(5,4,4),(9,8,5),(13,12,6),
                        (2,1,7),(6,5,8),(10,9,9),(14,13,10),(3,2,11),(7,6,12),
                        (11,10,13),(15,14,14)]:
        s[dst] = (s[src] + (r[k] << sh) - bias) & 0xffff
    return [(v & 0x7fff) if (v & 0x8000) else (~v) & 0xffff for v in s]
def b44(src, p_linear, flatfields=0):
    s = [expLut(v) for v in src] if p_linear else list(src)
    p = b44_pack(s, flatfields, 0 if p_linear else 1)
    out = b44_unpack(p)
    return ([logLut(v) for v in out] if p_linear else out), p

# internal_pxr24.c: float_to_float24()
def float24(f):
    u = struct.unpack('<I', struct.pack('<f', f))[0]
    s, e, m = u & 0x80000000, u & 0x7f800000, u & 0x007fffff
    if e == 0x7f800000:
        i = ((e >> 8) | (m >> 8) | (1 if (m >> 8) == 0 else 0)) if m else (e >> 8)
    else:
        i = ((e | m) + (m & 0x00000080)) >> 8
        if i >= 0x7f8000: i = (e | m) >> 8
    return ((s >> 8) | i) << 8

# ---- the counterexample ----
src = [f2h(1000.0)] + [f2h(0.001)] * 15
print('src  z0=0x%04x(%g)  z1=0x%04x(%g)' % (src[0], h2f(src[0]), src[1], h2f(src[1])))
for pl in (0, 1):
    for name, ff in (('B44', 0), ('B44A', 1)):
        out, p = b44(src, pl, ff)
        print('%-4s pLinear=%d shift=%d -> z0=0x%04x(%-9g) z1=0x%04x(%g)'
              % (name, pl, p[2], out[0], h2f(out[0]), out[1], h2f(out[1])))
```

输出（与本仓库编译后实测的 `z0`、`z1` 完全一致）：

```
src  z0=0x63d0(1000)  z1=0x1419(0.0010004)
B44  pLinear=0 shift=10 -> z0=0x63d0(1000     ) z1=0x13d0(0.000953674)
B44A pLinear=0 shift=10 -> z0=0x63d0(1000     ) z1=0x13d0(0.000953674)
B44  pLinear=1 shift=9  -> z0=0x558c(88.75    ) z1=0x9c00(-0.00390625)
B44A pLinear=1 shift=9  -> z0=0x558c(88.75    ) z1=0x9c00(-0.00390625)
```

注意 `z1` 的 `0x9c00` 是 **-0.00390625**，不是 0：`pLinear=1` 下暗部经 exp 映射后落在 `0x3bff`，
pack/unpack 后的往返把它推到 `0x3bff` 之下一档，`logLut` 得到负值。`pLinear=1` 的实际杀伤不是"精度变差"，
而是**暗部被 exp 压缩到 LUT 的平段后不可逆地聚成一簇，且恢复出来的值可以变成负数**。

`logLut` 在源码里是**没有零值保护**的：

```c
// src/lib/OpenEXRCore/internal_b44_table_init.c:37-40
float f = half_to_float (x);
f       = 8 * logf (f);          // f == ±0 时得到 -INFINITY
return float_to_half (f);        // -> 0xfc00
```

所以 `logLut(0x0000) == 0xfc00`（-inf），不是 0。本文的模型最初给这一步加了 `f<=0 -> 0` 的保护，
与真实库对拍后暴露出 308/1600 个块不一致，修正后为 **0/1600**。§3 的脚本已按源码修正。
这一条对 `pLinear=1` 特别重要：**大幅负值**（例如 `-172`，即 `0xd961`）在 `exp(v/8)` 时下溢为 HALF 的 0，
再经 `logLut(0)` 就得到 `-inf`。穷举全部 32768 个含符号的 HALF 位模式，有 16299 个负输入在
`logLut(expLut(x))` 之后落成 0 / -inf / +inf 之一：负的大幅值变成 -inf，负的暗部值变成 0。
所以给含负值的通道（例如某些 Z 或法线分量）开 `pLinear=1` 会改变符号语义，甚至产生 0 或 ±inf。实测平块：

```
pLinear=1 flat -0.0234    src=0xa5fe -> 0xa602 (-0.023468)
pLinear=1 flat -1         src=0xbc00 -> 0xbc02 (-1.00195)
pLinear=1 flat -172.125   src=0xd961 -> 0xfc00 (-inf)      <- exp 下溢后 ln(0)
pLinear=1 flat -2810      src=0xe97d -> 0xfc00 (-inf)
```

---

## 4. 反例与判据

### 4.1 何时改标记会改变落盘结果

只有 **B44/B44A + 文件通道 HALF** 这一格：

| 文件通道类型 | B44 / B44A | PXR24 |
|---|---|---|
| HALF | **`pLinear` 参与编码**（LUT、exactmax、flat-field 三条都受影响） | `pLinear` 完全无效（HALF 走整数差分 + deflate，无损） |
| FLOAT | `pLinear` 无效（`internal_b44.c:410` 直接 memcpy） | `pLinear` 无效（`float_to_float24` 不读它） |
| UINT | `pLinear` 无效（同上） | `pLinear` 无效 |

实测对同一份 FLOAT 通道文件把 `pLinear` 从 0 改成 1，读到 FLOAT 帧缓冲后逐位相同；
对 PXR24 的 HALF 文件做同样的事也逐位相同；只有 B44/B44A 的 HALF 通道在**压缩块**上会变
（raw 块见 §4.2 情况四、§4.3，标记在那里是空操作）。

`pLinear=1` 的量化上限来自 LUT 的饱和钳位：HALF 位模式 `>= 0x558c`（即 88.75）的值在 exp 映射时直接变成 `0x7bff`。
实测平块（全部像素同值，排除差分误差干扰）：

```
      v      pLin=0     pLin=1
    0.001    0.0010004      0        <- exp 映射后落在 0x3c00 附近，logLut 回来是 0
     0.01    0.0100021  0.00780869
      0.5    0.5        0.499756
      1      1          0.997559
     88      88         88
     88.72   88.75      88.75      <- 88.72 的 HALF 表示已是 0x558c，进入饱和段
      89      89         88.75
     100     100         88.75
    1000    1000         88.75
   10000   10000         88.75      <- 高光被压到 88.75
```

对 0x7bff（HALF_MAX）以下的 31744 个非负 HALF 位模式做 `logLut(expLut(x))` 穷举：
**26883 个被改变（仅 4861 个是不动点），其中 7167 个塌缩为 0.0**，后者恰好覆盖 `0x0001..0x1bff`
（约 6e-8 .. 0.0039）这一整段暗部。
也就是说 `pLinear=1` 不是"暗部精度降低"，而是**约 0.004 以下的整个区间被压成恰好 0**。
（这些暗部值在解回时是 0 而不是 -inf，因为它们经 exp 映射后是正数、pack 后仍是正数，落在 `logLut` 的非零分支。）
高光侧则在 88.75 处出现硬天花板。对暗部，`pLinear=1` 的相对误差在 0.004 附近出现过 **95%** 的偏差
（0.004 → 0.00780869），因为该值恰好跨过 exp 映射的一个量化台阶。这不是随机噪声，是确定性的映射失真。

### 4.2 何时差异在读入帧缓冲之前已不可逆

**情况一：文件通道 HALF + B44 + 任何 pLinear 值。** 落盘时就已经是 4×4 块的近似，
即使之后读成 FLOAT 帧缓冲，也不会比文件里的值更准。上面 `z1 = 0x13d0`（pLinear=0）或 `0x9c00`（pLinear=1）
就是文件里真实存的值；改读入类型只能在两者之上再加一层 half→float 的无损扩展，不能找回原始 `0x1419`。

**情况二：文件通道 FLOAT + PXR24。** 落盘时尾数被截到 15 位，这一步在任何读取之前完成。
更隐蔽的是：**截断可能跨越 HALF 的舍入边界**，导致"读成 HALF"比"直接读原始 float 再转 HALF"差一档。
实测：

```
源 float   0.889396846  = 0x3f63af83, half(src) = 0x3b1d
PXR24 落盘              = 0x3f63b000, half(pxr24) = 0x3b1e
PXR24 文件 -> HALF 帧缓冲  读回 0x3b1e   （比 half(src) 高一档）
PXR24 文件 -> FLOAT 帧缓冲 读回 0x3f63b000
```

即 PXR24 的 float→float24 舍入与 float→half 的舍入在这里方向相反，最终 HALF 结果差了 1 个 ULP。
这类差异在**写盘那一刻**就定了，读的阶段无法纠正。

**情况三：文件通道 FLOAT + 读入 HALF。** 与压缩无关的纯读侧损失。B44 的 FLOAT 通道位精确，
但 `UNPACK_FLOAT_TO_HALF_SAMPLES` 会把 1000.0 读成 1000（正好可表示）、把 0.1 读成 0.0999756、把 1e6 读成 `inf`。
这一层损失在函数进入帧缓冲前发生，之后无论做什么都无法恢复。

**情况四：B44 的 raw fallback。** 当 B44 压缩后**没有变小**时，编码器直接存原始字节（`internal_b44.c:496`）：

```c
if (nOut >= encode->packed_bytes) { memcpy(encode->compressed_buffer, encode->packed_buffer, ...); }
```

解码侧遇到 `packed_size == unpacked_size` 会跳过解压函数（`compression.c:676`），
这些像素**完全没经过 B44 的有损编码**，数值与写盘前一致。

PXR24 同理（`internal_pxr24.c:230`，`compbufsz > encode->packed_bytes`）。编码侧用的是严格不等号
`>`，而解码侧判 raw 用的是相等 `packed_size == unpacked_size`（`compression.c:676`），两者在
deflate 输出长度恰好等于输入长度时会不一致。实测穷举 1、2 字节的全部输入（65536+256 组）加 3–16 字节
的结构化输入共 131114 组，deflate 从未输出与输入等长的结果，所以这条边界没有被触发；但它不是靠不变式保证的。

实测 4×4 B44 一个 HALF 通道：32 字节未压缩 → 14 字节，会压缩；
但 1×1、2×2 等小块压缩后反而变大（2×2 = 8 字节 raw，压缩块最小 14 字节），于是**整个块原样存盘**。
所以 `pLinear` 对一个 2×2 的 B44 图像完全没有作用——数据根本没进 4×4 编码器。实测（平块，全部 1000.0）：

```
 1x1 B44: packed=2  unpacked=2  raw     pLin=1 -> 0x63d0 EXACT
 2x2 B44: packed=8  unpacked=8  raw     pLin=1 -> 0x63d0 EXACT
 3x3 B44: packed=14 unpacked=18 压缩    pLin=1 -> 0x558c lossy
 4x4 B44: packed=14 unpacked=32 压缩    pLin=1 -> 0x558c lossy
```

平铺图像中边缘 tile 只有 1 列或 1 行时也会触发同样的 raw 存储：
5×5 图像、tile 4×4 时，tile(0,0) 是 14 字节压缩块，tile(1,0) / (0,1) / (1,1) 都是 raw，于是同一张图里
**只有左上 tile 受 `pLinear` 影响**。这对判断"这张图到底有没有被有损处理"非常关键。

### 4.3 何时改标记会改变落盘结果 —— 具体到 flag flip

如果文件已经写好、只是把 header 里的 `pLinear` 位改掉，分两种块：

1. **raw 块**（`packed_size == unpacked_size`）：编码器虽然先做了 exp 映射和 pack，但一旦发现没变小就
   用 `packed_buffer` 的原始字节覆盖（`internal_b44.c:496`），那份映射结果被丢弃；解码时
   `decompress_data()` 在派发到 `internal_exr_undo_b44` **之前**就因 `packsz == unpacksz` 直接 memcpy 返回
   （`compression.c:676-682`），`convertToLinear` 根本不会执行。所以 raw 块上 `pLinear` 双向都是空操作。
   实测 2×2 平块（全是 1000.0）翻转标记：`0x63d0 → 0x63d0`，**不变**。
2. **压缩块**：标记翻转 = 用错的逆映射去解 `unpack14` 的结果，实测：

```
canonical block: z[0]=1000 (0x63d0), z[1..15]=0.001 (0x1419)

written pLinear=0: z0=0x63d0(1000) z1=0x13d0(0.000953674)
  | flag flipped at read: z0=0x52e8(55.25) z1=0xd2f5(-55.6562)   (corrupted)
written pLinear=1: z0=0x558c(88.75) z1=0x9c00(-0.00390625)
  | flag flipped at read: z0=0x7bff(65504) z1=0x3bff(0.999512)   (corrupted)
```

两个方向都得到完全无意义的数值。在 64×64 的渐变遮罩（左 8 列为 0.001，其余 0..1 的斜坡）上翻转整张图的标记，
4096/4096 像素全部改变。**所以 `pLinear` 是语义位，不是精度旋钮；落盘后不可安全修改。**

### 4.4 一图多通道时各通道独立

同一 chunk 内不同通道各自按自己的 `data_type` 与 `p_linear` 处理（`internal_b44.c` 的通道循环，
`internal_pxr24.c:104` 的通道循环）。实测 8×8，A=HALF/0.001 与 0.5，R=HALF 含 1000 高光：

```
B44  A=HALF pLin0, R=HALF pLin0    | A 暗 0.001 -> 0.0010004   A 中 0.5 -> 0.5     | R 高光 1000 -> 1000
B44  A=HALF pLin1, R=HALF pLin0    | A 暗 0.001 -> 0             A 中 0.5 -> 0.499756 | R 高光 1000 -> 1000
B44  A=FLOAT       R=HALF pLin0    | A 暗 0.001 -> 0.0010004   A 中 0.5 -> 0.5     | R 高光 1000 -> 1000
PXR24 A=HALF       R=FLOAT         | A 暗 0.001 -> 0.0010004   A 中 0.5 -> 0.5     | R 高光 1000 -> 1000（24 位）
```

所以合成师可以安全地：把需要保数值的遮罩存成 **FLOAT**（B44 位精确）或用 **PXR24 的 HALF**（无损）；
把颜色存成 **FLOAT + PXR24**（24 位，保留高光）。**不要**给遮罩开 `pLinear`。

### 4.5 生成性：重复保存是否稳定

实测 64×64 B44（pLinear 0 与 1 各一次）写→读→再写→再读，第二代与第一代逐像素相同。
即在这两个样本上 B44 表现为**幂等**（第二次保存不再继续劣化），但第一次的损失已经不可逆。
这不是普遍保证：一般的 B44 实现在"读出的值再作为输入"时不保证不动点，本测试只覆盖了上述两个块结构。
PXR24 的 HALF 通道在全 65536 个位模式随机采样（4096 像素）中 0 差异，确认无损；
PXR24 的 FLOAT 通道每次保存都会再截一次尾数，但因为 24 位结果再截仍是自身，**幂等**。

---

## 5. 给合成师的操作判据

1. **先看 `exrheader` 的 `plinear` 与类型。** `src/bin/exrheader/main.cpp:176` 会把 `plinear` 打出来。
2. **遮罩/AOV 要保数值：**
   * 用 B44/B44A → 通道类型必须是 FLOAT（位精确，任何 `pLinear` 取值都不影响）。
     HALF 通道只要块内跨度小就是无损的（§2），跨度大时按 `shift` 丢暗部低位；
     `pLinear=1` 会让 ≤0.0039 的暗部归零（含负值时的符号语义改变，§4.1）。
   * 用 PXR24 → HALF 通道无损；FLOAT 通道只保 24 位。
3. **颜色/高光：**
   * B44 的 HALF + `pLinear=1` 会把所有 ≥88.75 的高光压成 88.75，**绝不可用于 HDR**。
   * B44 的 HALF + `pLinear=0` 会把块内最大值精确恢复（实测最大值放在 4×4 块内任一位置都精确，
     这是 `exactmax=1` 的作用），暗部则按块内跨度舍入。
4. **读的时候：** 帧缓冲类型要与文件类型一致才能避免第二次舍入。文件 FLOAT 就读 FLOAT；
   文件 HALF 读 FLOAT 只是无损扩展，不会补偿任何压缩损失。
5. **不要事后改 `pLinear`。** 它是语义位；改了等于用错的 LUT 逆变换，数值会完全错位。
6. **小块/边缘 tile 可能根本没压缩**（raw fallback）。判断"是否有损"要看 chunk 的 `packed_size != unpacked_size`，
   不能只看 header 的 compression 字段。`exr_read_scanline_chunk_info` / `exr_read_tile_chunk_info` 可查。

---

## 6. 复算脚本

把 §3 的代码保存为 `b44_pxr24_roundtrip.py`，在任意目录执行：

```
python3 -I b44_pxr24_roundtrip.py
```

输出应与 §3 末尾一致。该脚本不依赖本仓库的编译产物，只依赖 Python 标准库的 `struct`（浮点→half 用 `'e'` 格式，
其舍入为 round-to-nearest-even，与 `float_to_half` 的实现一致）。

模型与真实库的交叉验证（本机编译本仓库后逐位对拍）：

* B44/B44A 的 HALF 通道：400 个随机 4×4 块（跨 1e-4..1e4、含符号与零）× {B44, B44A} × {pLinear 0,1}
  = 1600 组，模型与库**逐位一致 0 差异**。
* PXR24 的 FLOAT 通道：256 个随机位模式的 float32（含负值、零、次正规数）经 float→24 位→float 往返，
  以及"再读成 HALF"，模型与库**逐位一致 0 差异**。
* PXR24 的 HALF 通道：同一批数据确认无损（读回 HALF 与源相同）。

若 `'e'` 格式在目标 Python 上不可用（3.6 以下），可改用 `numpy.float16` 或直接查 `internal_coding.h:183` 的 `float_to_half`。

文中所有"实测"数据都由以下两类程序之一产生：直接写/读 `.exr` 并用 `ImfInputFile`/`ImfOutputFile` 读回位模式；
或用 `exr_read_scanline_chunk_info` / `exr_read_tile_chunk_info` / `exr_read_chunk` 直接查看落盘的
`packed_size`、`unpacked_size` 与 chunk 字节。
