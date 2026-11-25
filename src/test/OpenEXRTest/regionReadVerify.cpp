//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

//-----------------------------------------------------------------------------
//
//      regionReadVerify
//
// Independent behavioral check for InputFile::readRegion() and the
// coordinate helpers in ImfFrameBuffer.  It writes small negative-origin
// scanline EXR files with full- and half-sampled channels and verifies:
//
//   * clipping of an original-coordinate rectangle against dataWindow,
//   * preservation of the channel sampling grid for negative odd origins,
//   * caller owned row strides with extra padding,
//   * preservation of every byte outside the intersection (sentinels and
//     guard bands),
//   * interleaved "compositing canvas" layouts with large pixel strides,
//   * empty intersections and FLOAT -> HALF conversion.
//
// The program exits non-zero if any independently computed expectation
// differs.  It is intentionally standalone so it can serve as copy-paste
// documentation for the region API.
//
//-----------------------------------------------------------------------------

#include "ImfChannelList.h"
#include "ImfFrameBuffer.h"
#include "ImfHeader.h"
#include "ImfInputFile.h"
#include "ImfOutputFile.h"

#include <Imath/ImathBox.h>
#include <Imath/half.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace OPENEXR_IMF_NAMESPACE;
using namespace IMATH_NAMESPACE;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        ++g_checks;                                                            \
        if (!(cond))                                                           \
        {                                                                      \
            ++g_failures;                                                      \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__);                 \
            std::printf (__VA_ARGS__);                                        \
            std::printf ("\n");                                                \
        }                                                                      \
    } while (0)

static float
valZ (int x, int y)
{
    return 1000.0f + 13.0f * float (x) + 7.0f * float (y);
}
static uint32_t
valU (int x, int y)
{
    return uint32_t (50000 + x * 100 + y);
}
static uint32_t
valQ (int x, int y)
{
    return uint32_t (900000 + x * 1000 + y * 10);
}
static half
valH (int x, int y)
{
    return half (float (x * 10 + y));
}
static float
valF (int x, int y)
{
    return 1.25f * float (x) - 0.5f * float (y) + 0.375f;
}

static bool
isSample (int x, int y, int sx, int sy)
{
    return (x % sx == 0) && (y % sy == 0);
}

static uint32_t
peekU32 (const uint8_t* p)
{
    uint32_t v;
    std::memcpy (&v, p, 4);
    return v;
}
static uint16_t
peekU16 (const uint8_t* p)
{
    uint16_t v;
    std::memcpy (&v, p, 2);
    return v;
}

static const Box2i DW (V2i (-4, -4), V2i (5, 5));
static const int   DW_W = 10;
static const int   DW_H = 10;

// Build and write a test EXR with the requested compression.
static void
makeFile (const char* path, Compression compression)
{
    Header hdr;
    hdr.displayWindow () = Box2i (V2i (-8, -9), V2i (12, 11));
    hdr.dataWindow ()    = DW;
    hdr.compression ()   = compression;

    hdr.channels ().insert ("Z", Channel (FLOAT, 1, 1));
    hdr.channels ().insert ("U", Channel (UINT, 1, 1));
    hdr.channels ().insert ("Q", Channel (UINT, 2, 2));
    hdr.channels ().insert ("H", Channel (HALF, 2, 2));
    hdr.channels ().insert ("F", Channel (FLOAT, 1, 1));

    std::vector<float>    z (DW_W * DW_H);
    std::vector<uint32_t> u (DW_W * DW_H);
    std::vector<uint32_t> q (5 * 5);
    std::vector<half>     h (5 * 5);
    std::vector<float>    f (DW_W * DW_H);

    for (int y = DW.min.y; y <= DW.max.y; ++y)
        for (int x = DW.min.x; x <= DW.max.x; ++x)
        {
            int idx         = (y - DW.min.y) * DW_W + (x - DW.min.x);
            z[size_t (idx)] = valZ (x, y);
            u[size_t (idx)] = valU (x, y);
            f[size_t (idx)] = valF (x, y);
        }

    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 5; ++i)
        {
            int x                       = DW.min.x + 2 * i;
            int y                       = DW.min.y + 2 * j;
            q[size_t (j * 5 + i)]       = valQ (x, y);
            h[size_t (j * 5 + i)]       = valH (x, y);
        }

    FrameBuffer fb;
    fb.insert (
        "Z",
        Slice::Make (
            FLOAT, z.data (), DW, sizeof (float), DW_W * sizeof (float)));
    fb.insert (
        "U",
        Slice::Make (
            UINT, u.data (), DW, sizeof (uint32_t),
            DW_W * sizeof (uint32_t)));
    fb.insert (
        "F",
        Slice::Make (
            FLOAT, f.data (), DW, sizeof (float), DW_W * sizeof (float)));

    // 2x subsampled packed 5x5 planes.
    int64_t qoffx = floorSampleIndex (DW.min.x, 2) * 4;
    int64_t qoffy = floorSampleIndex (DW.min.y, 2) * int64_t (5 * 4);
    char* qbase   = reinterpret_cast<char*> (
        reinterpret_cast<intptr_t> (q.data ()) - qoffx - qoffy);
    fb.insert ("Q", Slice (UINT, qbase, 4, 5 * 4, 2, 2));

    int64_t hoffx = floorSampleIndex (DW.min.x, 2) * 2;
    int64_t hoffy = floorSampleIndex (DW.min.y, 2) * int64_t (5 * 2);
    char* hbase   = reinterpret_cast<char*> (
        reinterpret_cast<intptr_t> (h.data ()) - hoffx - hoffy);
    fb.insert ("H", Slice (HALF, hbase, 2, 5 * 2, 2, 2));

    remove (path);
    OutputFile out (path, hdr);
    out.setFrameBuffer (fb);
    out.writePixels (DW_H);
}

// A sentinel-filled destination with guard bands.
struct GuardedBuffer
{
    static constexpr int GUARD = 128;
    int                  width;
    int                  height;
    size_t               pixelBytes;
    size_t               rowStride;
    std::vector<uint8_t> raw;
    uint8_t*             pixels;

    GuardedBuffer (int w, int h, size_t pb, size_t padBytes)
        : width (w)
        , height (h)
        , pixelBytes (pb)
        , rowStride (size_t (w) * pb + padBytes)
    {
        size_t total = size_t (GUARD) * 2 + rowStride * size_t (h);
        raw.assign (total, 0xA5);
        pixels = raw.data () + GUARD;
    }

    bool guardsIntact () const
    {
        for (int i = 0; i < GUARD; ++i)
            if (raw[size_t (i)] != 0xA5 ||
                raw[raw.size () - 1 - size_t (i)] != 0xA5)
                return false;
        return true;
    }

    bool paddingIntact () const
    {
        size_t used = size_t (width) * pixelBytes;
        for (int r = 0; r < height; ++r)
        {
            const uint8_t* row = pixels + rowStride * size_t (r);
            for (size_t p = used; p < rowStride; ++p)
                if (row[p] != 0xA5) return false;
        }
        return true;
    }
};

static void
testGridMath ()
{
    std::printf ("-- grid math\n");
    CHECK (floorSampleIndex (-4, 2) == -2, "floor -4/2");
    CHECK (floorSampleIndex (-3, 2) == -2, "floor -3/2 must be -2");
    CHECK (floorSampleIndex (-2, 2) == -1, "floor -2/2");
    CHECK (floorSampleIndex (-1, 2) == -1, "floor -1/2 must be -1");
    CHECK (floorSampleIndex (0, 2) == 0, "floor 0/2");
    CHECK (floorSampleIndex (1, 2) == 0, "floor 1/2");
    CHECK (ceilSampleIndex (-3, 2) == -1, "ceil -3/2 must be -1");
    CHECK (ceilSampleIndex (-4, 2) == -2, "ceil -4/2");
    CHECK (ceilSampleIndex (1, 2) == 1, "ceil 1/2 must be 1");
    CHECK (ceilSampleIndex (0, 2) == 0, "ceil 0/2");
}

// Conventional full-window readback proves the fixture is correct.
static void
testFullReadSanity (const char* path)
{
    std::printf ("-- full conventional read (%s)\n", path);
    InputFile in (path);

    std::vector<float>    z (DW_W * DW_H, -1.0f);
    std::vector<uint32_t> q (5 * 5, 0xDEADBEEFu);
    std::vector<half>     h (5 * 5);

    FrameBuffer fb;
    fb.insert (
        "Z",
        Slice::Make (FLOAT, z.data (), DW, 4, size_t (DW_W) * 4));
    int64_t qx = floorSampleIndex (DW.min.x, 2) * 4;
    int64_t qy = floorSampleIndex (DW.min.y, 2) * int64_t (5 * 4);
    fb.insert (
        "Q",
        Slice (
            UINT,
            reinterpret_cast<char*> (
                reinterpret_cast<intptr_t> (q.data ()) - qx - qy),
            4, 20, 2, 2));
    int64_t hx = floorSampleIndex (DW.min.x, 2) * 2;
    int64_t hy = floorSampleIndex (DW.min.y, 2) * int64_t (5 * 2);
    fb.insert (
        "H",
        Slice (
            HALF,
            reinterpret_cast<char*> (
                reinterpret_cast<intptr_t> (h.data ()) - hx - hy),
            2, 10, 2, 2));

    in.setFrameBuffer (fb);
    in.readPixels (DW.min.y, DW.max.y);

    for (int y = DW.min.y; y <= DW.max.y; ++y)
        for (int x = DW.min.x; x <= DW.max.x; ++x)
        {
            int idx = (y + 4) * DW_W + (x + 4);
            CHECK (z[size_t (idx)] == valZ (x, y),
                   "Z at %d,%d got %f want %f",
                   x, y, double (z[size_t (idx)]), double (valZ (x, y)));
        }
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 5; ++i)
        {
            int x = -4 + 2 * i, y = -4 + 2 * j;
            CHECK (q[size_t (j * 5 + i)] == valQ (x, y),
                   "Q at %d,%d got %u want %u",
                   x, y, q[size_t (j * 5 + i)], valQ (x, y));
            CHECK (h[size_t (j * 5 + i)].bits () == valH (x, y).bits (),
                   "H at %d,%d", x, y);
        }
}

// Core trap #1: negative odd region origin, 2x subsampled UINT channel.
// Samples must land on odd destination columns/rows (global grid phase).
static void
testNegativeOddSubsampledConvenience (const char* path)
{
    std::printf ("-- negative odd origin, subsampled channel\n");
    InputFile in (path);

    Box2i         R (V2i (-3, -3), V2i (4, 4)); // 8x8
    GuardedBuffer buf (8, 8, 4, 6); // deliberately unaligned row stride

    Box2i got = in.readRegionChannel (
        "Q", R, buf.pixels, UINT, 4, buf.rowStride);

    CHECK (got == R, "returned box should equal region inside dw");
    CHECK (buf.guardsIntact (), "guard bands overwritten");
    CHECK (buf.paddingIntact (), "row padding overwritten");

    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
        {
            int x = R.min.x + c;
            int y = R.min.y + r;
            const uint8_t* p =
                buf.pixels + size_t (r) * buf.rowStride + size_t (c) * 4;
            uint32_t gotv = peekU32 (p);
            if (isSample (x, y, 2, 2))
            {
                CHECK (c % 2 == 1 && r % 2 == 1,
                       "sample phase wrong at c=%d r=%d", c, r);
                CHECK (gotv == valQ (x, y),
                       "Q[%d,%d] global(%d,%d) got %u want %u",
                       c, r, x, y, gotv, valQ (x, y));
            }
            else
            {
                CHECK (gotv == 0xA5A5A5A5u,
                       "non-sample slot (%d,%d) global(%d,%d) overwritten: %08x",
                       c, r, x, y, gotv);
            }
        }
}

// Core trap #2: region extends past several data-window edges; intersection
// left/top is offset inside destination; padding + sentinels must survive.
static void
testIntersectionOffsetPadding (const char* path)
{
    std::printf ("-- intersection offset + padded rows + sentinels\n");
    InputFile in (path);

    Box2i         R (V2i (-8, -6), V2i (8, 3)); // 17 x 10
    GuardedBuffer buf (17, 10, 4, 7);           // odd extra padding

    FrameBuffer fb;
    fb.insert (
        "Z",
        Slice::MakeRegion (
            FLOAT, buf.pixels, R, 4, buf.rowStride, 1, 1));

    Box2i got = in.readRegion (R, fb);

    Box2i expectInter (V2i (-4, -4), V2i (5, 3));
    CHECK (got == expectInter, "intersection mismatch");
    CHECK (buf.guardsIntact (), "guard bands overwritten");
    CHECK (buf.paddingIntact (), "row padding overwritten");

    for (int r = 0; r < 10; ++r)
        for (int c = 0; c < 17; ++c)
        {
            int x = R.min.x + c;
            int y = R.min.y + r;
            const uint8_t* p =
                buf.pixels + size_t (r) * buf.rowStride + size_t (c) * 4;
            uint32_t bits = peekU32 (p);
            if (DW.intersects (V2i (x, y)))
            {
                float g;
                std::memcpy (&g, &bits, 4);
                CHECK (g == valZ (x, y),
                       "Z dest(%d,%d) global(%d,%d) got %f want %f",
                       c, r, x, y, double (g), double (valZ (x, y)));
            }
            else
            {
                CHECK (bits == 0xA5A5A5A5u,
                       "outside-intersection dest(%d,%d) global(%d,%d) "
                       "overwritten: %08x",
                       c, r, x, y, bits);
            }
        }
}

// Region larger than the data window on every side.
static void
testLargerRegionAllSides (const char* path)
{
    std::printf ("-- region larger than data window on all sides\n");
    InputFile in (path);

    Box2i         R (V2i (-20, -20), V2i (20, 20)); // 41x41
    GuardedBuffer buf (41, 41, 4, 13);

    Box2i got = in.readRegionChannel (
        "Z", R, buf.pixels, FLOAT, 4, buf.rowStride);

    CHECK (got == DW, "intersection must be the whole data window");
    CHECK (buf.guardsIntact () && buf.paddingIntact (),
           "larger: guard/padding overwritten");

    for (int r = 0; r < 41; ++r)
        for (int c = 0; c < 41; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;
            uint32_t bits = peekU32 (
                buf.pixels + size_t (r) * buf.rowStride + size_t (c) * 4);
            if (DW.intersects (V2i (x, y)))
            {
                float g;
                std::memcpy (&g, &bits, 4);
                CHECK (g == valZ (x, y), "larger Z %d,%d", x, y);
            }
            else
                CHECK (bits == 0xA5A5A5A5u,
                       "larger sentinel at dest %d,%d", c, r);
        }
}

// Interleaved compositor canvas: pixel stride is a struct size and each
// channel starts at its field offset.
struct CanvasPixel
{
    float    z;
    uint32_t id;
};

static void
testInterleavedCanvas (const char* path)
{
    std::printf ("-- interleaved canvas with struct pixel stride\n");
    InputFile in (path);

    Box2i R (V2i (-3, -4), V2i (2, 1)); // 6 x 6
    size_t rowStride = size_t (6) * sizeof (CanvasPixel) + 16;

    std::vector<uint8_t> raw (128 * 2 + rowStride * 6, 0xCC);
    uint8_t*             base = raw.data () + 128;

    FrameBuffer fb;
    fb.insert (
        "Z",
        Slice::MakeRegion (
            FLOAT,
            base + offsetof (CanvasPixel, z),
            R,
            sizeof (CanvasPixel),
            rowStride,
            1,
            1));
    fb.insert (
        "U",
        Slice::MakeRegion (
            UINT,
            base + offsetof (CanvasPixel, id),
            R,
            sizeof (CanvasPixel),
            rowStride,
            1,
            1));

    in.readRegion (R, fb);

    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 6; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;
            uint8_t* px = base + size_t (r) * rowStride +
                          size_t (c) * sizeof (CanvasPixel);
            float z;
            std::memcpy (&z, px + offsetof (CanvasPixel, z), 4);
            uint32_t id = peekU32 (px + offsetof (CanvasPixel, id));
            if (DW.intersects (V2i (x, y)))
            {
                CHECK (z == valZ (x, y), "canvas z at %d,%d", x, y);
                CHECK (id == valU (x, y), "canvas id at %d,%d", x, y);
            }
            else
            {
                uint32_t zb;
                std::memcpy (&zb, px + offsetof (CanvasPixel, z), 4);
                CHECK (zb == 0xCCCCCCCCu, "canvas z sentinel %d,%d", c, r);
                CHECK (id == 0xCCCCCCCCu, "canvas id sentinel %d,%d", c, r);
            }
        }

    for (int i = 0; i < 128; ++i)
    {
        CHECK (raw[size_t (i)] == 0xCC, "front guard");
        CHECK (raw[raw.size () - 1 - size_t (i)] == 0xCC, "back guard");
    }
}

static void
testMixedChannels (const char* path)
{
    std::printf ("-- mixed channels in one readRegion call\n");
    InputFile in (path);

    Box2i         R (V2i (-5, -5), V2i (2, 2)); // 8x8
    GuardedBuffer zbuf (8, 8, 4, 16);
    GuardedBuffer qbuf (8, 8, 4, 9);

    FrameBuffer fb;
    fb.insert (
        "Z",
        Slice::MakeRegion (
            FLOAT, zbuf.pixels, R, 4, zbuf.rowStride, 1, 1));
    fb.insert (
        "Q",
        Slice::MakeRegion (
            UINT, qbuf.pixels, R, 4, qbuf.rowStride, 2, 2));

    in.readRegion (R, fb);

    CHECK (zbuf.guardsIntact () && qbuf.guardsIntact (),
           "mixed: guards overwritten");
    CHECK (zbuf.paddingIntact () && qbuf.paddingIntact (),
           "mixed: padding overwritten");

    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;

            const uint8_t* zp =
                zbuf.pixels + size_t (r) * zbuf.rowStride + c * 4;
            uint32_t zb = peekU32 (zp);
            if (DW.intersects (V2i (x, y)))
            {
                float g;
                std::memcpy (&g, &zb, 4);
                CHECK (g == valZ (x, y), "mixed Z at %d,%d", x, y);
            }
            else
                CHECK (zb == 0xA5A5A5A5u, "mixed Z sentinel at %d,%d", x, y);

            const uint8_t* qp =
                qbuf.pixels + size_t (r) * qbuf.rowStride + c * 4;
            uint32_t qv = peekU32 (qp);
            if (DW.intersects (V2i (x, y)) && isSample (x, y, 2, 2))
                CHECK (qv == valQ (x, y), "mixed Q at %d,%d", x, y);
            else
                CHECK (qv == 0xA5A5A5A5u,
                       "mixed Q sentinel at %d,%d got %08x", x, y, qv);
        }
}

static void
testEmptyIntersection (const char* path)
{
    std::printf ("-- empty intersection changes nothing\n");
    InputFile in (path);

    Box2i         R (V2i (100, 100), V2i (110, 110));
    GuardedBuffer buf (11, 11, 4, 3);

    Box2i got = in.readRegionChannel (
        "Z", R, buf.pixels, FLOAT, 4, buf.rowStride);

    CHECK (got.isEmpty (), "returned box must be empty");
    CHECK (buf.guardsIntact () && buf.paddingIntact (),
           "empty: guard/padding changed");
    bool allSentinel = true;
    for (uint8_t b : buf.raw)
        if (b != 0xA5) allSentinel = false;
    CHECK (allSentinel, "empty: destination buffer modified");
}

static void
testHalfSubsampled (const char* path)
{
    std::printf ("-- HALF subsampled channel phase + sentinels\n");
    InputFile in (path);

    Box2i         R (V2i (-3, -1), V2i (2, 2)); // 6 x 4
    GuardedBuffer buf (6, 4, 2, 5);

    in.readRegionChannel ("H", R, buf.pixels, HALF, 2, buf.rowStride);

    CHECK (buf.guardsIntact () && buf.paddingIntact (),
           "half: guard/padding overwritten");

    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 6; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;
            const uint8_t* p =
                buf.pixels + size_t (r) * buf.rowStride + c * 2;
            uint16_t bits = peekU16 (p);
            if (DW.intersects (V2i (x, y)) && isSample (x, y, 2, 2))
            {
                CHECK (bits == valH (x, y).bits (),
                       "H sample global(%d,%d) bits %04x want %04x",
                       x, y, bits, valH (x, y).bits ());
            }
            else
                CHECK (bits == 0xA5A5u,
                       "H non-sample (%d,%d) global(%d,%d) overwritten %04x",
                       c, r, x, y, bits);
        }
}

static void
testTypeConversion (const char* path)
{
    std::printf ("-- FLOAT file channel read as HALF region\n");
    InputFile in (path);

    Box2i         R (V2i (-2, -2), V2i (1, 1)); // 4x4
    GuardedBuffer buf (4, 4, 2, 4);

    in.readRegionChannel ("F", R, buf.pixels, HALF, 2, buf.rowStride);

    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;
            const uint8_t* p =
                buf.pixels + size_t (r) * buf.rowStride + c * 2;
            uint16_t bits = peekU16 (p);
            if (DW.intersects (V2i (x, y)))
                CHECK (bits == half (valF (x, y)).bits (),
                       "convert F->H at %d,%d got %04x want %04x",
                       x, y, bits, half (valF (x, y)).bits ());
            else
                CHECK (bits == 0xA5A5u, "conversion sentinel %d,%d", c, r);
        }
}

static void
testPositiveOrigin (const char* path)
{
    std::printf ("-- positive origin region\n");
    InputFile in (path);

    Box2i         R (V2i (0, 0), V2i (3, 3)); // 4x4
    GuardedBuffer buf (4, 4, 4, 0);

    in.readRegionChannel ("Q", R, buf.pixels, UINT, 4, buf.rowStride);

    // Q samples at global 0,2 -> destination cols 0,2 and rows 0,2.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            int      x = c, y = r;
            uint32_t v = peekU32 (
                buf.pixels + size_t (r) * buf.rowStride + c * 4);
            if (isSample (x, y, 2, 2))
                CHECK (v == valQ (x, y), "pos Q %d,%d", c, r);
            else
                CHECK (v == 0xA5A5A5A5u,
                       "pos Q sentinel %d,%d got %u", c, r, v);
        }
}

// A requested slice without a matching file channel must be filled
// with its fill value only inside the intersection.
static void
testMissingChannelFill (const char* path)
{
    std::printf ("-- missing channel fill\n");
    InputFile in (path);

    Box2i         R (V2i (-8, -6), V2i (8, 3)); // 17 x 10
    GuardedBuffer buf (17, 10, 4, 11);

    in.readRegionChannel (
        "NotInFile", R, buf.pixels, FLOAT, 4, buf.rowStride, 42.5);

    CHECK (buf.guardsIntact () && buf.paddingIntact (),
           "fill: guard/padding overwritten");

    for (int r = 0; r < 10; ++r)
        for (int c = 0; c < 17; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;
            uint32_t bits = peekU32 (
                buf.pixels + size_t (r) * buf.rowStride + c * 4);
            if (DW.intersects (V2i (x, y)))
            {
                float g;
                std::memcpy (&g, &bits, 4);
                CHECK (g == 42.5f, "fill value at %d,%d got %f", c, r, g);
            }
            else
                CHECK (bits == 0xA5A5A5A5u,
                       "fill sentinel at %d,%d", c, r);
        }
}

// Positive odd region origin: rounding the read band down to the even
// grid row must keep global phase (samples at odd destination slots).
static void
testPositiveOddStart (const char* path)
{
    std::printf ("-- positive odd origin phase\n");
    InputFile in (path);

    Box2i         R (V2i (1, 1), V2i (4, 4)); // 4x4
    GuardedBuffer buf (4, 4, 4, 5);

    in.readRegionChannel ("Q", R, buf.pixels, UINT, 4, buf.rowStride);

    CHECK (buf.guardsIntact () && buf.paddingIntact (),
           "pos-odd: guard/padding");
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            int      x = R.min.x + c, y = R.min.y + r;
            uint32_t v = peekU32 (
                buf.pixels + size_t (r) * buf.rowStride + c * 4);
            if (isSample (x, y, 2, 2))
            {
                CHECK (c % 2 == 1 && r % 2 == 1,
                       "pos-odd phase c=%d r=%d", c, r);
                CHECK (v == valQ (x, y), "pos-odd Q %d,%d", x, y);
            }
            else
                CHECK (v == 0xA5A5A5A5u, "pos-odd sentinel %d,%d", c, r);
        }
}

// Zero strides must resolve to tight packing (element size and
// region-width rows).
static void
testDefaultTightStrides (const char* path)
{
    std::printf ("-- default tight strides\n");
    InputFile in (path);

    Box2i            R (V2i (-3, -3), V2i (0, 0)); // 4x4
    std::vector<float> tight (4 * 4, -777.0f);

    Box2i got = in.readRegionChannel (
        "Z", R, tight.data (), FLOAT, 0, 0);

    CHECK (got == R, "tight: region fully inside dw");
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            int x = R.min.x + c, y = R.min.y + r;
            CHECK (tight[size_t (r * 4 + c)] == valZ (x, y),
                   "tight Z at %d,%d", c, r);
        }

    // Subsampled channel with default logical strides: tight logical
    // grid means slice stride becomes 2*sizeof(uint32_t), so samples
    // occupy the odd logical columns/rows and gaps stay untouched.
    std::vector<uint32_t> logical (4 * 4, 0xA5A5A5A5u);
    in.readRegionChannel ("Q", R, logical.data (), UINT, 0, 0);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            int      x = R.min.x + c, y = R.min.y + r;
            uint32_t v = logical[size_t (r * 4 + c)];
            if (isSample (x, y, 2, 2))
                CHECK (v == valQ (x, y), "tight Q at %d,%d", c, r);
            else
                CHECK (v == 0xA5A5A5A5u, "tight Q gap %d,%d", c, r);
        }
}

int
main (int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::string rawPath = dir + "/region_fixture_raw.exr";
    std::string zipPath = dir + "/region_fixture_zip.exr";

    try
    {
        // Exercise the multi-threaded scanline task path as well.
        setGlobalThreadCount (4);

        makeFile (rawPath.c_str (), NO_COMPRESSION);
        makeFile (zipPath.c_str (), ZIP_COMPRESSION);

        testGridMath ();
        testFullReadSanity (rawPath.c_str ());
        testNegativeOddSubsampledConvenience (rawPath.c_str ());
        testIntersectionOffsetPadding (rawPath.c_str ());
        testLargerRegionAllSides (rawPath.c_str ());
        testInterleavedCanvas (rawPath.c_str ());
        testMixedChannels (rawPath.c_str ());
        testEmptyIntersection (rawPath.c_str ());
        testMissingChannelFill (rawPath.c_str ());
        testHalfSubsampled (rawPath.c_str ());
        testTypeConversion (rawPath.c_str ());
        testPositiveOrigin (rawPath.c_str ());
        testPositiveOddStart (rawPath.c_str ());
        testDefaultTightStrides (rawPath.c_str ());

        // ZIP groups several scanlines per chunk; make sure the
        // negative-coordinate pointer setup survives chunk grouping.
        testNegativeOddSubsampledConvenience (zipPath.c_str ());
        testIntersectionOffsetPadding (zipPath.c_str ());
    }
    catch (std::exception& e)
    {
        std::printf ("UNCAUGHT EXCEPTION: %s\n", e.what ());
        return 2;
    }

    std::printf ("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
