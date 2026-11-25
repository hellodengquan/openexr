// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenEXR Project.

//
// Integration tests for the PIZ (lossless wavelet + Huffman) compressor.
//
// The asset library seals multi-channel EXR masters with PIZ, so these
// tests exercise the full encode -> file -> decode path through the
// public C core API rather than any internal helper:
//
//  * testPIZBitPatterns() feeds every stored channel raw sample bit
//    patterns (including corner bit patterns and NaN payloads) and
//    requires the decoded samples to be bitwise identical, in the same
//    channel order, landing at the same actual sampling positions.
//    HALF, UINT and FLOAT pixel types with mixed x/y subsampling are
//    covered, both as scanline chunks and as tiles.
//
//  * testPIZPartialChunks() writes a multi-chunk image, then makes only
//    some of the compression blocks available (file truncation and a
//    corrupt chunk leader). The chunks that remain intact must still be
//    independently reconstructable to the exact original bits, while the
//    unavailable pixels must be reported as errors and must never take
//    away the availability of the good regions.
//

#include "pizIntegration.h"
#include "test_value.h"

#include "openexr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#    include <io.h>
#else
#    include <sys/types.h>
#    include <unistd.h>
#endif

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

//
// The default error handler prints each reported error to stderr. Keep
// that behaviour (it documents the expected-failure paths) while marking
// the messages as expected so a test log stays readable.
//
static void
expected_error_handler (exr_const_context_t, int code, const char* msg)
{
    std::cerr << "  [expected library error " << code << "]: " << msg
              << std::endl;
}

static exr_context_initializer_t
quiet_initializer ()
{
    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &expected_error_handler;
    return cinit;
}

//
// Deterministic 32-bit mix so the expected patterns can be regenerated
// independently from the buffers that travel through the compressor.
//
static uint32_t
mix32 (uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static uint16_t
expected_half (uint32_t seed, int x, int y)
{
    uint32_t v = mix32 (seed + (uint32_t) x * 0x9e3779b1U +
                        (uint32_t) y * 0x85ebca77U);
    // Force the distinctive half bit patterns through at fixed sites so
    // the wavelet / range-coder stages cannot hide a dropped high bit.
    uint32_t site = (uint32_t) (x * 31 + y * 17);
    switch (site % 9)
    {
        case 0: return 0x0000;       // +0
        case 1: return 0x8000;       // -0
        case 2: return 0x3c00;       // 1.0
        case 3: return 0xbc00;       // -1.0
        case 4: return 0x7bff;       // largest finite half
        case 5: return 0xfbff;       // most negative finite half
        case 6: return 0x7c00;       // +inf
        case 7: return 0xfc00;       // -inf
        case 8: return 0x7e51;       // quiet NaN with a non-trivial payload
    }
    return (uint16_t) (v >> 16);
}

static uint32_t
expected_uint (uint32_t seed, int x, int y)
{
    uint32_t v = mix32 (seed ^ (uint32_t) x * 0xc2b2ae35U ^
                        (uint32_t) y * 0x27d4eb2fU);
    uint32_t site = (uint32_t) (x * 13 + y * 7);
    switch (site % 6)
    {
        case 0: return 0x00000000U;
        case 1: return 0xffffffffU;
        case 2: return 0x80000000U;
        case 3: return 0x7fffffffU;
        case 4: return 0x00000001U;
        case 5: return 0xfffffffeU;
    }
    return v;
}

static float
expected_float (uint32_t seed, int x, int y)
{
    uint32_t bits = expected_uint (seed, x, y);
    float    f;
    memcpy (&f, &bits, sizeof (f));
    return f;
}

struct ChanSpec
{
    const char*       name;
    exr_pixel_type_t  type;
    int               xs;
    int               ys;
    uint32_t          seed;
};

//
// One planar, full-image buffer per channel. Only the sampled positions
// are filled with the expected pattern; every other slot is set to a
// fixed canary so the test can prove non-sampled slots were never
// written (i.e. the actual sampling positions are respected).
//
struct Channel
{
    ChanSpec            spec;
    int                 width;
    int                 height;
    int                 bytes;
    std::vector<uint8_t> original;
    std::vector<uint8_t> decoded;

    static const uint32_t CANARY = 0xBEEF1234U;

    Channel (const ChanSpec& s, int w, int h)
        : spec (s), width (w), height (h),
          bytes (s.type == EXR_PIXEL_HALF ? 2 : 4)
    {
        original.assign ((size_t) width * height * bytes, 0);
        decoded.assign ((size_t) width * height * bytes, CANARY & 0xff);
        // Spread the canary over all bytes of every slot.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                write_canary (x, y);
    }

    size_t
    offset (int x, int y) const
    {
        return ((size_t) y * width + x) * bytes;
    }

    void
    write_canary (int x, int y)
    {
        if (bytes == 2)
        {
            uint16_t v = (uint16_t) 0xBEEFU;
            memcpy (decoded.data () + offset (x, y), &v, 2);
        }
        else
        {
            uint32_t v = CANARY;
            memcpy (decoded.data () + offset (x, y), &v, 4);
        }
    }

    void
    fill_original ()
    {
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if ((x % spec.xs) != 0 || (y % spec.ys) != 0)
                {
                    // Non-sampled slot in the source: store a canary too,
                    // it is never read by the encoder.
                    if (bytes == 2)
                    {
                        uint16_t v = (uint16_t) 0xBEEFU;
                        memcpy (original.data () + offset (x, y), &v, 2);
                    }
                    else
                    {
                        uint32_t v = CANARY;
                        memcpy (original.data () + offset (x, y), &v, 4);
                    }
                    continue;
                }
                if (spec.type == EXR_PIXEL_HALF)
                {
                    uint16_t v = expected_half (spec.seed, x, y);
                    memcpy (original.data () + offset (x, y), &v, 2);
                }
                else if (spec.type == EXR_PIXEL_UINT)
                {
                    uint32_t v = expected_uint (spec.seed, x, y);
                    memcpy (original.data () + offset (x, y), &v, 4);
                }
                else
                {
                    float v = expected_float (spec.seed, x, y);
                    memcpy (original.data () + offset (x, y), &v, 4);
                }
            }
        }
    }
};

static const Channel*
find_channel (const std::vector<Channel>& chans, const char* name)
{
    for (const auto& c : chans)
        if (strcmp (c.spec.name, name) == 0) return &c;
    return nullptr;
}

//
// Bind a pipeline channel to a full-image planar grid. Both the pack
// and unpack stages accept the same convention here:
//   base        = first grid slot of the chunk (row start_y, col 0),
//   pixel stride = xs * bytes  -> samples land on cols 0, xs, 2xs ...,
//   line stride  = ys * W * bytes -> sampled row k lands on grid row
//                  start_y + k * ys.
// Non-sampled grid slots are never touched and keep their canaries,
// proving the samples occupy their actual sampling positions.
//
static void
bind_scan_channel (
    exr_coding_channel_info_t& curc,
    const Channel&             ch,
    int                        chunk_start_y,
    bool                       decoding)
{
    const uint8_t* buffer_start =
        (decoding ? ch.decoded.data () : ch.original.data ());
    const uint8_t* base =
        buffer_start + (size_t) chunk_start_y * ch.width * ch.bytes;

    curc.user_pixel_stride = ch.spec.xs * ch.bytes;
    curc.user_line_stride  = ch.spec.ys * ch.width * ch.bytes;
    if (decoding)
        curc.decode_to_ptr = const_cast<uint8_t*> (base);
    else
        curc.encode_from_ptr = base;
}

static void
write_piz_scanline_file (
    const std::string&       filename,
    int                      width,
    int                      height,
    const std::vector<ChanSpec>& specs,
    const std::vector<Channel>&  chans)
{
    exr_context_t f;
    int           partidx;
    auto          cinit = quiet_initializer ();

    EXRCORE_TEST_RVAL (
        exr_start_write (&f, filename.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (f, "piz-scan", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        f, partidx, width, height, EXR_COMPRESSION_PIZ));
    for (const auto& s : specs)
        EXRCORE_TEST_RVAL (exr_add_channel (
            f, partidx, s.name, s.type, EXR_PERCEPTUALLY_LOGARITHMIC, s.xs, s.ys));
    EXRCORE_TEST_RVAL (exr_write_header (f));

    int32_t lines_per_chunk = 0;
    EXRCORE_TEST_RVAL (
        exr_get_scanlines_per_chunk (f, partidx, &lines_per_chunk));
    EXRCORE_TEST (lines_per_chunk == 32);

    exr_encode_pipeline_t encoder = EXR_ENCODE_PIPELINE_INITIALIZER;
    bool                  first   = true;
    for (int y = 0; y < height; y += lines_per_chunk)
    {
        exr_chunk_info_t cinfo;
        EXRCORE_TEST_RVAL (
            exr_write_scanline_chunk_info (f, partidx, y, &cinfo));
        if (first)
        {
            EXRCORE_TEST_RVAL (
                exr_encoding_initialize (f, partidx, &cinfo, &encoder));
        }
        else
        {
            EXRCORE_TEST_RVAL (
                exr_encoding_update (f, partidx, &cinfo, &encoder));
        }

        for (int c = 0; c < encoder.channel_count; ++c)
        {
            exr_coding_channel_info_t& curc = encoder.channels[c];
            if (curc.height == 0)
            {
                curc.encode_from_ptr   = NULL;
                curc.user_pixel_stride = 0;
                curc.user_line_stride  = 0;
                continue;
            }
            const Channel* ch = find_channel (chans, curc.channel_name);
            EXRCORE_TEST (ch != nullptr);
            bind_scan_channel (curc, *ch, y, false);
        }
        if (first)
        {
            EXRCORE_TEST_RVAL (
                exr_encoding_choose_default_routines (f, partidx, &encoder));
        }
        EXRCORE_TEST_RVAL (exr_encoding_run (f, partidx, &encoder));
        first = false;
    }
    EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
    EXRCORE_TEST_RVAL (exr_finish (&f));
}

static void
write_piz_tiled_file (
    const std::string&          filename,
    int                         width,
    int                         height,
    const std::vector<ChanSpec>& specs,
    const std::vector<Channel>&  chans)
{
    exr_context_t f;
    int           partidx;
    auto          cinit = quiet_initializer ();

    EXRCORE_TEST_RVAL (
        exr_start_write (&f, filename.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (f, "piz-tile", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        f, partidx, width, height, EXR_COMPRESSION_PIZ));
    for (const auto& s : specs)
        EXRCORE_TEST_RVAL (exr_add_channel (
            f, partidx, s.name, s.type, EXR_PERCEPTUALLY_LOGARITHMIC, s.xs, s.ys));
    EXRCORE_TEST_RVAL (exr_set_tile_descriptor (
        f, partidx, 32, 32, EXR_TILE_ONE_LEVEL, EXR_TILE_ROUND_DOWN));
    EXRCORE_TEST_RVAL (exr_write_header (f));

    int32_t tilew = 0, tileh = 0;
    EXRCORE_TEST_RVAL (exr_get_tile_sizes (f, partidx, 0, 0, &tilew, &tileh));
    EXRCORE_TEST (tilew == 32 && tileh == 32);

    int tiles_x = (width + tilew - 1) / tilew;
    int tiles_y = (height + tileh - 1) / tileh;

    exr_encode_pipeline_t encoder = EXR_ENCODE_PIPELINE_INITIALIZER;
    bool                  first   = true;
    for (int ty = 0; ty < tiles_y; ++ty)
    {
        for (int tx = 0; tx < tiles_x; ++tx)
        {
            exr_chunk_info_t cinfo;
            EXRCORE_TEST_RVAL (exr_write_tile_chunk_info (
                f, partidx, tx, ty, 0, 0, &cinfo));
            if (first)
            {
                EXRCORE_TEST_RVAL (
                    exr_encoding_initialize (f, partidx, &cinfo, &encoder));
            }
            else
            {
                EXRCORE_TEST_RVAL (
                    exr_encoding_update (f, partidx, &cinfo, &encoder));
            }

            int origin_x = tx * tilew;
            int origin_y = ty * tileh;
            for (int c = 0; c < encoder.channel_count; ++c)
            {
                exr_coding_channel_info_t& curc = encoder.channels[c];
                if (curc.height == 0)
                {
                    curc.encode_from_ptr   = NULL;
                    curc.user_pixel_stride = 0;
                    curc.user_line_stride  = 0;
                    continue;
                }
                const Channel* ch = find_channel (chans, curc.channel_name);
                EXRCORE_TEST (ch != nullptr);

                // Same full-grid convention as the scanline path:
                // base at tile origin, strides scatter the samples.
                const uint8_t* ebase =
                    ch->original.data () +
                    ((size_t) origin_y * ch->width + origin_x) * ch->bytes;
                curc.encode_from_ptr   = ebase;
                curc.user_pixel_stride = ch->spec.xs * ch->bytes;
                curc.user_line_stride  = ch->spec.ys * ch->width * ch->bytes;
            }
            if (first)
                EXRCORE_TEST_RVAL (
                    exr_encoding_choose_default_routines (f, partidx, &encoder));
            EXRCORE_TEST_RVAL (exr_encoding_run (f, partidx, &encoder));
            first = false;
        }
    }
    EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
    EXRCORE_TEST_RVAL (exr_finish (&f));
}

//
// Compare every decoded slot against the independently regenerated
// expectation. Non-sampled slots must still hold their canaries.
//
static void
verify_channel_bits (const Channel& ch)
{
    for (int y = 0; y < ch.height; ++y)
    {
        for (int x = 0; x < ch.width; ++x)
        {
            size_t off = ch.offset (x, y);
            if ((x % ch.spec.xs) != 0 || (y % ch.spec.ys) != 0)
            {
                if (ch.bytes == 2)
                {
                    uint16_t got;
                    memcpy (&got, ch.decoded.data () + off, 2);
                    EXRCORE_TEST_LOCATION (got == 0xBEEFU, x, y);
                }
                else
                {
                    uint32_t got;
                    memcpy (&got, ch.decoded.data () + off, 4);
                    EXRCORE_TEST_LOCATION (got == Channel::CANARY, x, y);
                }
                continue;
            }
            if (ch.spec.type == EXR_PIXEL_HALF)
            {
                uint16_t got, want;
                memcpy (&got, ch.decoded.data () + off, 2);
                want = expected_half (ch.spec.seed, x, y);
                if (got != want)
                {
                    std::cerr << "channel " << ch.spec.name << " half at " << x
                              << "," << y << " got 0x" << std::hex << got
                              << " want 0x" << want << std::dec << std::endl;
                    EXRCORE_TEST_FAIL (piz half bit mismatch);
                }
            }
            else if (ch.spec.type == EXR_PIXEL_UINT)
            {
                uint32_t got, want;
                memcpy (&got, ch.decoded.data () + off, 4);
                want = expected_uint (ch.spec.seed, x, y);
                if (got != want)
                {
                    std::cerr << "channel " << ch.spec.name << " uint at " << x
                              << "," << y << " got 0x" << std::hex << got
                              << " want 0x" << want << std::dec << std::endl;
                    EXRCORE_TEST_FAIL (piz uint bit mismatch);
                }
            }
            else
            {
                uint32_t got, want;
                float    wantf = expected_float (ch.spec.seed, x, y);
                memcpy (&got, ch.decoded.data () + off, 4);
                memcpy (&want, &wantf, 4);
                if (got != want)
                {
                    std::cerr << "channel " << ch.spec.name << " float at "
                              << x << "," << y << " got 0x" << std::hex << got
                              << " want 0x" << want << std::dec << std::endl;
                    EXRCORE_TEST_FAIL (piz float bit mismatch);
                }
            }
        }
    }
}

static void
decode_all_scanline_chunks (
    const std::string&         filename,
    int                        width,
    int                        height,
    std::vector<Channel>&      chans)
{
    exr_context_t f;
    auto          cinit = quiet_initializer ();
    EXRCORE_TEST_RVAL (exr_start_read (&f, filename.c_str (), &cinit));

    int32_t lines_per_chunk = 0;
    EXRCORE_TEST_RVAL (exr_get_scanlines_per_chunk (f, 0, &lines_per_chunk));

    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
    bool                  first   = true;
    for (int y = 0; y < height; y += lines_per_chunk)
    {
        exr_chunk_info_t cinfo;
        EXRCORE_TEST_RVAL (exr_read_scanline_chunk_info (f, 0, y, &cinfo));
        if (first)
        {
            EXRCORE_TEST_RVAL (
                exr_decoding_initialize (f, 0, &cinfo, &decoder));
        }
        else
        {
            EXRCORE_TEST_RVAL (
                exr_decoding_update (f, 0, &cinfo, &decoder));
        }

        for (int c = 0; c < decoder.channel_count; ++c)
        {
            exr_coding_channel_info_t& curc = decoder.channels[c];
            if (curc.height == 0)
            {
                curc.decode_to_ptr     = NULL;
                curc.user_pixel_stride = 0;
                curc.user_line_stride  = 0;
                continue;
            }
            const Channel* ch = find_channel (chans, curc.channel_name);
            EXRCORE_TEST (ch != nullptr);
            bind_scan_channel (curc, *ch, y, true);
        }
        if (first)
        {
            EXRCORE_TEST_RVAL (
                exr_decoding_choose_default_routines (f, 0, &decoder));
        }
        EXRCORE_TEST_RVAL (exr_decoding_run (f, 0, &decoder));
        first = false;
    }
    EXRCORE_TEST_RVAL (exr_decoding_destroy (f, &decoder));
    EXRCORE_TEST_RVAL (exr_finish (&f));
}

static void
decode_all_tiles (
    const std::string&    filename,
    int                   width,
    int                   height,
    std::vector<Channel>& chans)
{
    exr_context_t f;
    auto          cinit = quiet_initializer ();
    EXRCORE_TEST_RVAL (exr_start_read (&f, filename.c_str (), &cinit));

    int32_t tilew = 0, tileh = 0;
    EXRCORE_TEST_RVAL (exr_get_tile_sizes (f, 0, 0, 0, &tilew, &tileh));
    int tiles_x = (width + tilew - 1) / tilew;
    int tiles_y = (height + tileh - 1) / tileh;

    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
    bool                  first   = true;
    for (int ty = 0; ty < tiles_y; ++ty)
    {
        for (int tx = 0; tx < tiles_x; ++tx)
        {
            exr_chunk_info_t cinfo;
            EXRCORE_TEST_RVAL (
                exr_read_tile_chunk_info (f, 0, tx, ty, 0, 0, &cinfo));
            if (first)
            {
                EXRCORE_TEST_RVAL (
                    exr_decoding_initialize (f, 0, &cinfo, &decoder));
            }
            else
            {
                EXRCORE_TEST_RVAL (
                    exr_decoding_update (f, 0, &cinfo, &decoder));
            }

            int origin_x = tx * tilew;
            int origin_y = ty * tileh;
            for (int c = 0; c < decoder.channel_count; ++c)
            {
                exr_coding_channel_info_t& curc = decoder.channels[c];
                if (curc.height == 0)
                {
                    curc.decode_to_ptr     = NULL;
                    curc.user_pixel_stride = 0;
                    curc.user_line_stride  = 0;
                    continue;
                }
                Channel* ch =
                    const_cast<Channel*> (find_channel (chans, curc.channel_name));
                EXRCORE_TEST (ch != nullptr);

                uint8_t* base =
                    ch->decoded.data () +
                    ((size_t) origin_y * ch->width + origin_x) * ch->bytes;
                curc.decode_to_ptr     = base;
                curc.user_pixel_stride = ch->spec.xs * ch->bytes;
                curc.user_line_stride  = ch->spec.ys * ch->width * ch->bytes;
            }
            if (first)
            {
                EXRCORE_TEST_RVAL (
                    exr_decoding_choose_default_routines (f, 0, &decoder));
            }
            EXRCORE_TEST_RVAL (exr_decoding_run (f, 0, &decoder));
            first = false;
        }
    }
    EXRCORE_TEST_RVAL (exr_decoding_destroy (f, &decoder));
    EXRCORE_TEST_RVAL (exr_finish (&f));
}

static void
reset_decoded (std::vector<Channel>& chans)
{
    for (auto& ch : chans)
    {
        for (int y = 0; y < ch.height; ++y)
            for (int x = 0; x < ch.width; ++x)
                ch.write_canary (x, y);
    }
}

//
// Requirement 1: each stored channel type keeps its raw bit pattern, the
// channels stay in their declared order and every sample lands at its
// actual (sub)sampling position after a PIZ round trip.
//
} // namespace

void
testPIZBitPatterns (const std::string& tempdir)
{
    // 62 x 66: each dimension is a multiple of every subsampling factor
    // used below, but misaligned with the 32-sample PIZ block size so
    // partial edge blocks (a narrower final tile column, the final 2
    // lines) are exercised.
    const int W = 62;
    const int H = 66;

    const std::vector<ChanSpec> specs = {
        {"A", EXR_PIXEL_HALF,  1, 1, 0x1001u},
        {"B", EXR_PIXEL_FLOAT, 1, 1, 0x2002u},
        {"G", EXR_PIXEL_HALF,  2, 2, 0x3003u},
        {"I", EXR_PIXEL_UINT,  1, 1, 0x4004u},
        {"R", EXR_PIXEL_HALF,  2, 1, 0x5005u},
        {"Y", EXR_PIXEL_HALF,  1, 2, 0x6006u},
    };

    std::vector<Channel> chans;
    for (const auto& s : specs)
    {
        chans.emplace_back (s, W, H);
        chans.back ().fill_original ();
    }

    // The channel list is stored alphabetically; the pipeline order must
    // match it so each buffer is bound to the right channel name.
    for (size_t i = 1; i < specs.size (); ++i)
        EXRCORE_TEST (strcmp (specs[i - 1].name, specs[i].name) < 0);

    {
        std::string fn = tempdir + "piz_bitpatterns_scan.exr";
        std::cout << "  PIZ bit patterns, scanline, mixed sampling"
                  << std::endl;
        write_piz_scanline_file (fn, W, H, specs, chans);
        reset_decoded (chans);
        decode_all_scanline_chunks (fn, W, H, chans);
        for (const auto& ch : chans) verify_channel_bits (ch);
        remove (fn.c_str ());
    }

    {
        // Tiled images only support uniform (1, 1) sampling, so mixed
        // subsampling is covered exclusively by the scanline case above.
        // The tiled case still exercises every pixel type, the tile
        // block layout and partial edge tiles (62 x 66 against 32 x 32).
        std::vector<ChanSpec> tile_specs;
        for (const auto& s : specs)
            if (s.xs == 1 && s.ys == 1) tile_specs.push_back (s);

        std::string fn = tempdir + "piz_bitpatterns_tile.exr";
        std::cout << "  PIZ bit patterns, tiled, uniform sampling and "
                     "partial edge tiles"
                  << std::endl;
        write_piz_tiled_file (fn, W, H, tile_specs, chans);
        reset_decoded (chans);
        decode_all_tiles (fn, W, H, chans);
        for (const auto& s : tile_specs)
            verify_channel_bits (*find_channel (chans, s.name));
        remove (fn.c_str ());
    }
}

//
// Simpler uniform-sampling image used for the partial-chunk tests: one
// HALF, one UINT and one FLOAT channel, enough chunks that individual
// blocks can be removed while others stay intact.
//
struct PartialImage
{
    static const int     W = 40;
    static const int     H = 96; // 3 PIZ scanline chunks of 32 lines
    std::vector<Channel> chans;

    PartialImage ()
    {
        const std::vector<ChanSpec> specs = {
            {"F", EXR_PIXEL_FLOAT, 1, 1, 0xA11CEu},
            {"H", EXR_PIXEL_HALF,  1, 1, 0xB0Bu},
            {"I", EXR_PIXEL_UINT,  1, 1, 0xC0DEu},
        };
        for (const auto& s : specs)
        {
            chans.emplace_back (s, W, H);
            chans.back ().fill_original ();
        }
    }
};

static bool
slot_is_intact (const Channel& ch, int y_start, int y_end)
{
    for (int y = y_start; y < y_end; ++y)
        for (int x = 0; x < ch.width; ++x)
        {
            size_t off = ch.offset (x, y);
            if (ch.spec.type == EXR_PIXEL_HALF)
            {
                uint16_t got, want = expected_half (ch.spec.seed, x, y);
                memcpy (&got, ch.decoded.data () + off, 2);
                if (got != want) return false;
            }
            else if (ch.spec.type == EXR_PIXEL_UINT)
            {
                uint32_t got, want = expected_uint (ch.spec.seed, x, y);
                memcpy (&got, ch.decoded.data () + off, 4);
                if (got != want) return false;
            }
            else
            {
                uint32_t got, want;
                float    wantf = expected_float (ch.spec.seed, x, y);
                memcpy (&got, ch.decoded.data () + off, 4);
                memcpy (&want, &wantf, 4);
                if (got != want) return false;
            }
        }
    return true;
}

static bool
slot_is_canary (const Channel& ch, int y_start, int y_end)
{
    for (int y = y_start; y < y_end; ++y)
        for (int x = 0; x < ch.width; ++x)
        {
            size_t off = ch.offset (x, y);
            uint32_t got;
            if (ch.bytes == 2)
            {
                uint16_t g;
                memcpy (&g, ch.decoded.data () + off, 2);
                if (g != 0xBEEFU) return false;
            }
            else
            {
                memcpy (&got, ch.decoded.data () + off, 4);
                if (got != Channel::CANARY) return false;
            }
        }
    return true;
}

//
// Decode one scanline chunk into the channel buffers. The pipeline is
// created fresh per chunk so a failed decode cannot leak state into the
// next attempt; returns the library status of the chunk info read or
// the decoding run.
//
static exr_result_t
decode_one_scan_chunk (
    exr_context_t            f,
    int                      chunk_y,
    std::vector<Channel>&    chans)
{
    exr_chunk_info_t      cinfo;
    exr_result_t          rv =
        exr_read_scanline_chunk_info (f, 0, chunk_y, &cinfo);
    if (rv != EXR_ERR_SUCCESS) return rv;

    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
    rv = exr_decoding_initialize (f, 0, &cinfo, &decoder);
    if (rv != EXR_ERR_SUCCESS)
    {
        exr_decoding_destroy (f, &decoder);
        return rv;
    }
    for (int c = 0; c < decoder.channel_count; ++c)
    {
        exr_coding_channel_info_t& curc = decoder.channels[c];
        if (curc.height == 0)
        {
            curc.decode_to_ptr     = NULL;
            curc.user_pixel_stride = 0;
            curc.user_line_stride  = 0;
            continue;
        }
        const Channel* ch = find_channel (chans, curc.channel_name);
        EXRCORE_TEST (ch != nullptr);
        bind_scan_channel (curc, *ch, chunk_y, true);
    }
    EXRCORE_TEST_RVAL (
        exr_decoding_choose_default_routines (f, 0, &decoder));
    rv = exr_decoding_run (f, 0, &decoder);
    exr_decoding_destroy (f, &decoder);
    return rv;
}

//
// Read the on-disk start offset of a scanline chunk by opening the
// original file and asking for its chunk info. The leader (y coordinate
// and packed size) occupies 8 bytes before the pixel data offset.
//
static uint64_t
chunk_leader_offset (const std::string& fn, int chunk_y)
{
    exr_context_t      f;
    exr_chunk_info_t   cinfo;
    auto               cinit = quiet_initializer ();
    EXRCORE_TEST_RVAL (exr_start_read (&f, fn.c_str (), &cinit));
    EXRCORE_TEST_RVAL (exr_read_scanline_chunk_info (f, 0, chunk_y, &cinfo));
    // data_offset points just past the 8-byte leader.
    uint64_t leader = cinfo.data_offset - 2 * sizeof (int32_t);
    EXRCORE_TEST_RVAL (exr_finish (&f));
    return leader;
}

static void
truncate_file (const std::string& fn, uint64_t new_size)
{
    // Resize an existing file down to new_size, dropping every byte past
    // the cut point to simulate a master whose later compression blocks
    // were never written / are unavailable.
#ifdef _WIN32
    FILE* fp = nullptr;
    EXRCORE_TEST (fopen_s (&fp, fn.c_str (), "rb+") == 0);
    EXRCORE_TEST (fp != nullptr);
    EXRCORE_TEST (_chsize_s (_fileno (fp), (long long) new_size) == 0);
    fclose (fp);
#else
    EXRCORE_TEST (::truncate (fn.c_str (), (off_t) new_size) == 0);
#endif
}

//
// Requirement 2a: only the first compression blocks of the file are
// present (truncated master). Good blocks remain independently readable;
// missing pixels are signalled and stay undelivered.
//
static void
testPIZTruncatedChunks (
    const std::string& fn, const PartialImage& img_template)
{
    const int lines_per_chunk = 32;
    const int cut_chunk       = 2; // chunks 0 and 1 survive, chunk 2 is gone
    uint64_t  cut_offset =
        chunk_leader_offset (fn, cut_chunk * lines_per_chunk);
    truncate_file (fn, cut_offset);

    PartialImage got;
    reset_decoded (got.chans);

    exr_context_t f;
    auto          cinit = quiet_initializer ();
    EXRCORE_TEST_RVAL (exr_start_read (&f, fn.c_str (), &cinit));

    // The missing region must fail through the normal status path (the
    // exact error code depends on where the truncated bytes are noticed),
    // and the context must stay usable afterwards.
    exr_result_t rv =
        decode_one_scan_chunk (f, cut_chunk * lines_per_chunk, got.chans);
    EXRCORE_TEST (rv != EXR_ERR_SUCCESS);

    // The two intact blocks must still decode on their own and match the
    // original samples bit for bit.
    EXRCORE_TEST_RVAL (decode_one_scan_chunk (f, 0, got.chans));
    EXRCORE_TEST_RVAL (
        decode_one_scan_chunk (f, lines_per_chunk, got.chans));
    EXRCORE_TEST_RVAL (exr_finish (&f));

    for (const auto& want_ch : img_template.chans)
    {
        const Channel* got_ch = find_channel (got.chans, want_ch.spec.name);
        EXRCORE_TEST (got_ch != nullptr);
        EXRCORE_TEST (slot_is_intact (*got_ch, 0, 2 * lines_per_chunk));
        // Pixels belonging to the unavailable block were never written.
        EXRCORE_TEST (
            slot_is_canary (*got_ch, 2 * lines_per_chunk, PartialImage::H));
    }

    // Decode the good region again from a fresh context to prove no
    // state from the failed read is required.
    PartialImage  again;
    reset_decoded (again.chans);
    exr_context_t f2;
    EXRCORE_TEST_RVAL (exr_start_read (&f2, fn.c_str (), &cinit));
    EXRCORE_TEST_RVAL (decode_one_scan_chunk (f2, 0, again.chans));
    EXRCORE_TEST_RVAL (
        decode_one_scan_chunk (f2, lines_per_chunk, again.chans));
    EXRCORE_TEST_RVAL (exr_finish (&f2));
    for (const auto& want_ch : img_template.chans)
    {
        const Channel* got_ch =
            find_channel (again.chans, want_ch.spec.name);
        EXRCORE_TEST (slot_is_intact (*got_ch, 0, 2 * lines_per_chunk));
    }
}

//
// Requirement 2b: all blocks are present but one has a corrupt leader
// (declared packed size larger than anything the chunk can legally hold).
// That single block must be undeliverable while its neighbours are
// unaffected.
//
static void
testPIZCorruptChunkLeader (
    const std::string& fn, const PartialImage& img_template)
{
    const int lines_per_chunk = 32;
    const int bad_chunk       = 1;
    uint64_t  leader =
        chunk_leader_offset (fn, bad_chunk * lines_per_chunk);

    {
        std::fstream out (
            fn, std::ios::in | std::ios::out | std::ios::binary);
        EXRCORE_TEST (out.is_open ());
        // Leader layout: int32 y coordinate, then int32 packed size.
        out.seekp ((std::streamoff) (leader + sizeof (int32_t)));
        int32_t bogus = 0x7fffffff;
        out.write (reinterpret_cast<const char*> (&bogus), sizeof (bogus));
        EXRCORE_TEST (out.good ());
    }

    PartialImage got;
    reset_decoded (got.chans);

    exr_context_t f;
    auto          cinit = quiet_initializer ();
    EXRCORE_TEST_RVAL (exr_start_read (&f, fn.c_str (), &cinit));

    // The corrupt block must be rejected without taking the context down.
    exr_result_t rv =
        decode_one_scan_chunk (f, bad_chunk * lines_per_chunk, got.chans);
    EXRCORE_TEST (rv != EXR_ERR_SUCCESS);

    // Chunks before and after the corrupt one are independently valid.
    EXRCORE_TEST_RVAL (decode_one_scan_chunk (f, 0, got.chans));
    EXRCORE_TEST_RVAL (
        decode_one_scan_chunk (f, 2 * lines_per_chunk, got.chans));
    EXRCORE_TEST_RVAL (exr_finish (&f));

    for (const auto& want_ch : img_template.chans)
    {
        const Channel* got_ch = find_channel (got.chans, want_ch.spec.name);
        EXRCORE_TEST (got_ch != nullptr);
        EXRCORE_TEST (slot_is_intact (*got_ch, 0, lines_per_chunk));
        EXRCORE_TEST (slot_is_canary (
            *got_ch, lines_per_chunk, 2 * lines_per_chunk));
        EXRCORE_TEST (
            slot_is_intact (*got_ch, 2 * lines_per_chunk, PartialImage::H));
    }
}

void
testPIZPartialChunks (const std::string& tempdir)
{
    PartialImage img;

    std::vector<ChanSpec> specs;
    for (const auto& ch : img.chans) specs.push_back (ch.spec);

    {
        std::string fn = tempdir + "piz_partial_truncated.exr";
        std::cout << "  PIZ partial blocks: truncated file" << std::endl;
        write_piz_scanline_file (
            fn, PartialImage::W, PartialImage::H, specs, img.chans);
        testPIZTruncatedChunks (fn, img);
        remove (fn.c_str ());
    }

    {
        std::string fn = tempdir + "piz_partial_corrupt.exr";
        std::cout << "  PIZ partial blocks: corrupt chunk leader"
                  << std::endl;
        write_piz_scanline_file (
            fn, PartialImage::W, PartialImage::H, specs, img.chans);
        testPIZCorruptChunkLeader (fn, img);
        remove (fn.c_str ());
    }
}
