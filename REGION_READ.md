# 按原画坐标读取选区（readRegion）

为扫描线 EXR 增加“按原画/全局坐标指定矩形，把与 dataWindow 的交集写入自带行距的目标区域”的能力。矩形外的目标字节（边距、行 padding、预先画好的底图、哨兵）不会被写；子采样通道严格保持原采样网格的相位。

## 新增 API

- `Imf::InputFile::readRegion(region, frameBuffer) -> Box2i`
  通用多通道版本。`frameBuffer` 中的每个 slice 通常用下面的
  `Slice::MakeRegion()` 构造。返回值是原画坐标下实际写入的交集框；无交集时返回空 box 且一字节都不写。
- `Imf::InputFile::readRegionChannel(name, region, ptr, type, xStride=0, yStride=0, fill=0) -> Box2i`
  单通道便捷封装，采样因子自动取自文件头。`ptr` 指向 `region.min` 对应的目标像素；
  `xStride` 是相邻逻辑像素的字节步长（交错结构体时可传 `sizeof(Pixel)`），
  `yStride` 是调用方自有的行距（可含额外 padding；传 0 为紧凑行）。
- `Imf::Slice::MakeRegion(type, ptr, region, xStride=0, yStride=0, xSampling=1, ySampling=1, fill=0)`
  构造坐标相位正确的目标 slice。
- `Imf::copyRegionPixels(srcFB, dstFB, region, sourceBounds) -> Box2i`
  `Imf::floorSampleIndex()` / `Imf::ceilSampleIndex()`：负坐标正确的采样网格运算。
- `readSelection(...)` 是 `readRegion(...)` 的同义内联别名。

## 负奇数起点与半采样通道

采样点是采样因子的整数倍：`...,-4,-2,0,2,4,...`（s=2）。网格下标必须用数学
floor/ceil；C++ 向零整除在负奇数上会错一格（`-3/2 == -1`，而 `floor(-1.5) == -2`）。

`MakeRegion` 把逻辑步长乘以采样因子作为 Slice 步长，并用**未除采样**的区域原点
偏移 base：

```
base = ptr - region.min.x * xStride - region.min.y * yStride
slice.xStride = xSampling * xStride
slice.yStride = ySampling * yStride
```

于是全局采样点 `x = k*xSampling` 恰好落在
`ptr + (x - region.min.x) * xStride`。例：region 左缘 `-3`、xSampling=2 时，
第一个采样点 `-2` 落在目标第 1 个逻辑列（保留左缘空缺），随后在第 3、5、7 列，
不会被错误地压缩到第 0、1、2、3 列。

实现方式：扫描线解码始终产出完整 dataWindow 行，因此 `readRegion` 先解码到紧凑的
dataWindow scratch FrameBuffer，再用 `copyRegionPixels` 只把交集内、采样网格上的点
拷进调用方目标；这从结构上保证交集外的字节不被写。

## 独立验证程序

`src/test/OpenEXRTest/regionReadVerify.cpp`（CMake 目标 `oeRegionReadVerify`，
CTest 用例 `OpenEXR.RegionRead`）。它构造负原点 dataWindow（`[-4,-4]..[5,5]`）、
含 1x1 的 `Z/U/F` 与 2x2 的 `Q/H` 通道，独立按坐标计算预期，并检查：

- 负奇数 region 起点时 2x2 子采样通道落在正确的奇/偶目标格；
- region 伸出 dataWindow 四边、交集左缘不在目标原点时的偏移；
- 每行额外 padding、前后 guard band、框外哨兵逐字节保持；
- 大于 dataWindow 的 region、空交集、交错结构体像素步长；
- HALF 子采样、FLOAT->HALF 转换、默认紧凑步长、ZIP 多扫描线 chunk。

构建并运行（新副本先 configure；本机依赖前缀为
`/Users/quan/.local/share/fb2/deps/image-196-230`）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/Users/quan/.local/share/fb2/deps/image-196-230 \
  -DOPENEXR_BUILD_TOOLS=OFF -DOPENEXR_BUILD_EXAMPLES=OFF -DBUILD_TESTING=ON
cmake --build build --target oeRegionReadVerify
ctest --test-dir build -R 'OpenEXR.RegionRead' --output-on-failure
# 或直接运行（参数是临时 EXR 的输出目录，缺省 /tmp）
./build/bin/oeRegionReadVerify /tmp
```

外部独立程序可参照下面的编译命令（按实际仓库与依赖前缀调整 `REPO`、`DEPS`）：

```bash
c++ -std=c++17 region_check.cpp -o /tmp/region_check \
  -I"$REPO/src/lib/OpenEXR" -I"$REPO/build/cmake" \
  -I"$REPO/src/lib/OpenEXRCore" -I"$REPO/src/lib/Iex" -I"$REPO/src/lib/IlmThread" \
  -isystem "$DEPS/include" -isystem "$DEPS/include/Imath" \
  -L"$REPO/build/src/lib/OpenEXR" -lOpenEXR \
  -L"$REPO/build/src/lib/OpenEXRCore" -lOpenEXRCore \
  -L"$REPO/build/src/lib/Iex" -lIex -L"$REPO/build/src/lib/IlmThread" -lIlmThread \
  -L"$DEPS/lib" -lImath \
  -Wl,-rpath,"$REPO/build/src/lib/OpenEXR" \
  -Wl,-rpath,"$REPO/build/src/lib/OpenEXRCore" \
  -Wl,-rpath,"$REPO/build/src/lib/Iex" \
  -Wl,-rpath,"$REPO/build/src/lib/IlmThread" \
  -Wl,-rpath,"$DEPS/lib"
```

最小用法：

```cpp
Box2i roi(V2i(-3,-3), V2i(4,4));          // 原画坐标
std::vector<uint32_t> pixels(8 * paddedRowWords, 0xDEADBEEFu);
InputFile in("shot.exr");
Box2i written = in.readRegionChannel(
    "Y", roi, pixels.data(), UINT,
    /*xStride=*/4, /*yStride=*/paddedRowBytes);
```

已回归通过：`OpenEXR.testChannels`、`testScanLineApi`、
`testLargeDataWindowOffsets`、`testCompressionApi`、`testMultiPartApi`、
`testDeepScanLineMultipleRead`。
