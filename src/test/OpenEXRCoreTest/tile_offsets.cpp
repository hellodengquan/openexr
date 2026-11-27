// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenEXR Project.

//
// Integration tests for tiled image chunk location handling.
//
// The OpenEXR on-disk chunk offset table holds the file location of each
// chunk as a 64-bit value. In a multi-part file the pixel data of the
// later parts is located far from the file header, and a tile with a
// large logical index may therefore be the only thing a reader looks at
// deep inside the file. These tests build small in-memory tiled images
// through the public Core C API, mutate the in-memory byte stream, and
// read them back over custom stream callbacks to verify that:
//
//   1. a valid chunk recorded with a large 64-bit file location delivers
//      the pixel values actually written for that part / tile, and
//   2. locations the storage cannot satisfy fail before any output pixel
//      is delivered, distinguishing a physically truncated read position
//      from a chunk that the file simply does not contain.
//
// Everything lives in memory: the backing storage allocated is exactly
// the bytes the writer produced (the in-memory equivalent of a file),
// and each per-tile output buffer is sized for the tile's real pixels.
//

#include "tile_offsets.h"

#include "test_value.h"

#include "openexr.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

namespace
{

//
// In-memory backing store for an EXR file, driven through the custom
// stream callbacks.
//
// `bytes` only ever holds what the writer actually wrote, so the
// storage used is proportional to the real image data rather than to
// any of the (64-bit) offsets referenced inside the file.
//
// `read_limit` truncates the readable range for the read callbacks:
// bytes at offsets >= read_limit behave as if the file ended there. A
// value of zero means "no truncation". `reported_size` lets the size
// query advertise a logical size different from the bytes actually
// present, which is needed to simulate a chunk table pointing past the
// end of the backing storage.
//
struct MemoryFile
{
    std::vector<uint8_t> bytes;
    uint64_t             read_limit    = 0;
    int64_t              reported_size = -1;
    int                  read_errors   = 0;
};

static int64_t
mem_write (
    exr_const_context_t,
    void* userdata,
    const void* buffer,
    uint64_t sz,
    uint64_t offset,
    exr_stream_error_func_ptr_t)
{
    MemoryFile* mf = static_cast<MemoryFile*> (userdata);
    if (offset + sz > mf->bytes.size ())
        mf->bytes.resize (static_cast<size_t> (offset + sz), uint8_t (0));
    memcpy (mf->bytes.data () + offset, buffer, static_cast<size_t> (sz));
    return static_cast<int64_t> (sz);
}

static int64_t
mem_read (
    exr_const_context_t,
    void* userdata,
    void* buffer,
    uint64_t sz,
    uint64_t offset,
    exr_stream_error_func_ptr_t)
{
    MemoryFile* mf = static_cast<MemoryFile*> (userdata);

    uint64_t end = static_cast<uint64_t> (mf->bytes.size ());
    if (mf->read_limit != 0 && mf->read_limit < end)
        end = mf->read_limit;

    // A request starting at or past the readable range is a hard EOF,
    // matching pread() semantics (0 bytes read).
    if (offset >= end)
    {
        ++mf->read_errors;
        return 0;
    }

    uint64_t avail = end - offset;
    if (sz > avail)
    {
        // Short read: the caller asked for more bytes than the storage
        // provides from this position. This is the truncation signal.
        ++mf->read_errors;
        sz = avail;
    }
    memcpy (buffer, mf->bytes.data () + offset, static_cast<size_t> (sz));
    return static_cast<int64_t> (sz);
}

static int64_t
mem_size (exr_const_context_t, void* userdata)
{
    MemoryFile* mf = static_cast<MemoryFile*> (userdata);
    if (mf->reported_size >= 0) return mf->reported_size;
    return static_cast<int64_t> (mf->bytes.size ());
}

static void
mem_destroy (exr_const_context_t, void* userdata, int)
{
    // The MemoryFile is owned by the test, nothing to do.
    (void) userdata;
}

static exr_context_initializer_t
stream_init (MemoryFile& mf, bool want_write)
{
    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.user_data                 = &mf;
    cinit.read_fn                   = &mem_read;
    cinit.size_fn                   = &mem_size;
    cinit.destroy_fn                = &mem_destroy;
    if (want_write) cinit.write_fn = &mem_write;
    return cinit;
}

//
// Geometry of the two tiled parts. Both have one FLOAT channel named
// "V". The builder defaults to uncompressed storage (so the on-disk
// pixel block is native planar data checkable bit-for-bit) and can
// instead be built with compression for the truncation cases.
//
// Part 0 is the background layer (part index 0), part 1 the foreground
// layer (part index 1). Part 1's chunks are written after all of part
// 0's data, so every one of its chunk locations is far from the header.
//
constexpr int kTileSize     = 8;
constexpr int kBgWidth      = 32;
constexpr int kBgHeight     = 24;
constexpr int kBgTilesX     = kBgWidth / kTileSize;   // 4
constexpr int kBgTilesY     = kBgHeight / kTileSize;  // 3
constexpr int kFgWidth      = 16;
constexpr int kFgHeight     = 16;
constexpr int kFgTilesX     = kFgWidth / kTileSize;   // 2
constexpr int kFgTilesY     = kFgHeight / kTileSize;  // 2
constexpr int kChannelBytes = 4;                      // FLOAT

// Deterministic, exactly representable value for a pixel. Keeping the
// integers small makes every float comparison exact.
static float
pixel_value (int part, int tilex, int tiley, int px, int py)
{
    return static_cast<float> (
        (part + 1) * 10000 + tiley * 1000 + tilex * 100 + py * 10 + px);
}

static void
configure_tiled_part (
    exr_context_t    f,
    int              partidx,
    int              width,
    int              height,
    exr_compression_t compression)
{
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        f, partidx, width, height, compression));
    EXRCORE_TEST_RVAL (exr_add_channel (
        f,
        partidx,
        "V",
        EXR_PIXEL_FLOAT,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_set_tile_descriptor (
        f,
        partidx,
        kTileSize,
        kTileSize,
        EXR_TILE_ONE_LEVEL,
        EXR_TILE_ROUND_DOWN));
    // Tiles are written in an order independent of their logical chunk
    // index, which random line order explicitly permits.
    EXRCORE_TEST_RVAL (
        exr_set_lineorder (f, partidx, EXR_LINEORDER_RANDOM_Y));
    // Multi-part files share one display window across all parts; the
    // per-part data window (set from width/height above) stays distinct.
    const exr_attr_box2i_t shared_display = {
        {0, 0}, {kBgWidth - 1, kBgHeight - 1}};
    EXRCORE_TEST_RVAL (
        exr_set_display_window (f, partidx, &shared_display));
}

// Fill the planar float buffer for one tile and run it through the
// encode pipeline. The buffer holds exactly tile_w * tile_h pixels.
static void
write_one_tile (
    exr_context_t f,
    int           partidx,
    int           tilex,
    int           tiley,
    int           tilew,
    int           tileh)
{
    exr_chunk_info_t      cinfo;
    exr_encode_pipeline_t encoder = EXR_ENCODE_PIPELINE_INITIALIZER;
    std::vector<float>    pixels (
        static_cast<size_t> (tilew) * static_cast<size_t> (tileh));

    for (int py = 0; py < tileh; ++py)
        for (int px = 0; px < tilew; ++px)
            pixels[py * tilew + px] =
                pixel_value (partidx, tilex, tiley, px, py);

    EXRCORE_TEST_RVAL (exr_write_tile_chunk_info (
        f, partidx, tilex, tiley, 0, 0, &cinfo));
    EXRCORE_TEST (cinfo.width == tilew);
    EXRCORE_TEST (cinfo.height == tileh);
    EXRCORE_TEST_RVAL (
        exr_encoding_initialize (f, partidx, &cinfo, &encoder));
    EXRCORE_TEST (encoder.channel_count == 1);
    EXRCORE_TEST (!strcmp (encoder.channels[0].channel_name, "V"));
    encoder.channels[0].encode_from_ptr   = (const uint8_t*) pixels.data ();
    encoder.channels[0].user_pixel_stride = kChannelBytes;
    encoder.channels[0].user_line_stride =
        kChannelBytes * tilew;
    EXRCORE_TEST_RVAL (
        exr_encoding_choose_default_routines (f, partidx, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_run (f, partidx, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
}

// Build the complete two-part tiled image into `mf`.
static void
build_multipart_tiled_file (
    MemoryFile& mf, exr_compression_t compression = EXR_COMPRESSION_NONE)
{
    exr_context_t f = nullptr;
    int           bgpart, fgpart;

    exr_context_initializer_t cinit = stream_init (mf, true);

    EXRCORE_TEST_RVAL (
        exr_start_write (&f, "mem:tiled-parts.exr", EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (f, "background", EXR_STORAGE_TILED, &bgpart));
    EXRCORE_TEST (bgpart == 0);
    EXRCORE_TEST_RVAL (
        exr_add_part (f, "foreground", EXR_STORAGE_TILED, &fgpart));
    EXRCORE_TEST (fgpart == 1);

    configure_tiled_part (
        f, bgpart, kBgWidth, kBgHeight, compression);
    configure_tiled_part (
        f, fgpart, kFgWidth, kFgHeight, compression);

    EXRCORE_TEST_RVAL (exr_write_header (f));

    int32_t bgchunks, fgchunks;
    EXRCORE_TEST_RVAL (exr_get_chunk_count (f, bgpart, &bgchunks));
    EXRCORE_TEST (bgchunks == kBgTilesX * kBgTilesY);
    EXRCORE_TEST_RVAL (exr_get_chunk_count (f, fgpart, &fgchunks));
    EXRCORE_TEST (fgchunks == kFgTilesX * kFgTilesY);

    // Part 0's full image first, in natural tile order.
    for (int ty = 0; ty < kBgTilesY; ++ty)
        for (int tx = 0; tx < kBgTilesX; ++tx)
            write_one_tile (f, bgpart, tx, ty, kTileSize, kTileSize);

    // Part 1 follows; its locations are therefore far from the header.
    for (int ty = 0; ty < kFgTilesY; ++ty)
        for (int tx = 0; tx < kFgTilesX; ++tx)
            write_one_tile (f, fgpart, tx, ty, kTileSize, kTileSize);

    EXRCORE_TEST_RVAL (exr_finish (&f));
    EXRCORE_TEST (f == nullptr);
    EXRCORE_TEST (!mf.bytes.empty ());
}

// Decode one tile of an open read context and return the float pixels.
// The returned vector is sized for the tile's actual pixel count.
static std::vector<float>
read_tile_pixels (
    exr_const_context_t f,
    int                 partidx,
    int                 tilex,
    int                 tiley,
    int*                tilew_out = nullptr,
    int*                tileh_out = nullptr)
{
    exr_chunk_info_t      cinfo;
    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;

    EXRCORE_TEST_RVAL (exr_read_tile_chunk_info (
        f, partidx, tilex, tiley, 0, 0, &cinfo));
    EXRCORE_TEST (cinfo.type == EXR_STORAGE_TILED);
    EXRCORE_TEST (cinfo.start_x == tilex);
    EXRCORE_TEST (cinfo.start_y == tiley);

    EXRCORE_TEST_RVAL (
        exr_decoding_initialize (f, partidx, &cinfo, &decoder));
    EXRCORE_TEST (decoder.channel_count == 1);

    const int tilew = decoder.channels[0].width;
    const int tileh = decoder.channels[0].height;
    if (tilew_out) *tilew_out = tilew;
    if (tileh_out) *tileh_out = tileh;

    std::vector<float> pixels (
        static_cast<size_t> (tilew) * static_cast<size_t> (tileh));
    // Poison the destination so a test can prove untouched output on a
    // failed delivery.
    std::fill (pixels.begin (), pixels.end (), -12345.0f);

    decoder.channels[0].decode_to_ptr     = (uint8_t*) pixels.data ();
    decoder.channels[0].user_pixel_stride = kChannelBytes;
    decoder.channels[0].user_line_stride  = kChannelBytes * tilew;

    EXRCORE_TEST_RVAL (
        exr_decoding_choose_default_routines (f, partidx, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_run (f, partidx, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_destroy (f, &decoder));

    return pixels;
}

static void
check_tile_pixels (
    const std::vector<float>& pixels,
    int                       partidx,
    int                       tilex,
    int                       tiley,
    int                       tilew,
    int                       tileh)
{
    EXRCORE_TEST (
        pixels.size () ==
        static_cast<size_t> (tilew) * static_cast<size_t> (tileh));
    for (int py = 0; py < tileh; ++py)
        for (int px = 0; px < tilew; ++px)
        {
            float expected = pixel_value (partidx, tilex, tiley, px, py);
            float got      = pixels[py * tilew + px];
            EXRCORE_TEST_LOCATION (
                got == expected, tilex, tiley);
        }
}

// Native little-endian helpers for editing raw chunk table bytes.
static void
store_le64 (uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t> ((v >> (8 * i)) & 0xffu);
}

static uint64_t
load_le64 (const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t> (p[i]) << (8 * i);
    return v;
}

} // namespace

//
// 1. Valid chunks with large logical indices and 64-bit file locations
//    far from the file header deliver that part's own pixels.
//
void
testReadTilesFarOffsets (const std::string&)
{
    MemoryFile mf;
    build_multipart_tiled_file (mf);
    const uint64_t file_size = mf.bytes.size ();

    exr_context_initializer_t cinit = stream_init (mf, false);
    exr_context_t             f     = nullptr;
    EXRCORE_TEST_RVAL (exr_start_read (&f, "mem:tiled-parts.exr", &cinit));

    int32_t partcount = -1;
    EXRCORE_TEST_RVAL (exr_get_count (f, &partcount));
    EXRCORE_TEST (partcount == 2);

    uint64_t bg_table_off = 0, fg_table_off = 0;
    int32_t  bgchunks = 0, fgchunks = 0;
    EXRCORE_TEST_RVAL (exr_get_chunk_table_offset (f, 0, &bg_table_off));
    EXRCORE_TEST_RVAL (exr_get_chunk_table_offset (f, 1, &fg_table_off));
    EXRCORE_TEST_RVAL (exr_get_chunk_count (f, 0, &bgchunks));
    EXRCORE_TEST_RVAL (exr_get_chunk_count (f, 1, &fgchunks));
    EXRCORE_TEST (bgchunks == kBgTilesX * kBgTilesY);
    EXRCORE_TEST (fgchunks == kFgTilesX * kFgTilesY);

    // The writer lays all parts' chunk tables down contiguously right
    // after the headers; the foreground table therefore follows the
    // background table with no pixel data in between.
    EXRCORE_TEST (fg_table_off > bg_table_off);
    const uint64_t all_tables_end =
        fg_table_off +
        static_cast<uint64_t> (fgchunks) * sizeof (uint64_t);
    EXRCORE_TEST (
        bg_table_off +
            static_cast<uint64_t> (bgchunks) * sizeof (uint64_t) ==
        fg_table_off);

    // Probe the raw 64-bit table entries: the location of the last
    // foreground tile (largest logical index) must be a real offset well
    // inside the file, past every chunk table and far from the header.
    const int last_fg_chunk = fgchunks - 1;
    const uint64_t last_fg_entry_pos =
        fg_table_off + static_cast<uint64_t> (last_fg_chunk) * sizeof (uint64_t);
    EXRCORE_TEST (last_fg_entry_pos + sizeof (uint64_t) <= file_size);
    const uint64_t last_fg_offset =
        load_le64 (mf.bytes.data () + last_fg_entry_pos);
    EXRCORE_TEST (last_fg_offset >= all_tables_end);
    EXRCORE_TEST (last_fg_offset < file_size);

    // Every foreground chunk location is beyond every background chunk
    // location: part 1's pixel data is the deepest data in the file.
    for (int ci = 0; ci < fgchunks; ++ci)
    {
        const uint64_t fg_off = load_le64 (
            mf.bytes.data () + fg_table_off +
            static_cast<uint64_t> (ci) * sizeof (uint64_t));
        for (int bi = 0; bi < bgchunks; ++bi)
        {
            const uint64_t bg_off = load_le64 (
                mf.bytes.data () + bg_table_off +
                static_cast<uint64_t> (bi) * sizeof (uint64_t));
            EXRCORE_TEST (fg_off > bg_off);
        }
    }

    // The same value must be what the library's table exposes.
    uint64_t* table_ptr = nullptr;
    int32_t   table_count = 0;
    EXRCORE_TEST_RVAL (exr_get_chunk_table (f, 1, &table_ptr, &table_count));
    EXRCORE_TEST (table_count == fgchunks);
    EXRCORE_TEST (table_ptr[last_fg_chunk] == last_fg_offset);
    EXRCORE_TEST_RVAL (exr_validate_chunk_table (f, 1));
    EXRCORE_TEST_RVAL (exr_validate_chunk_table (f, 0));

    // Read every background tile and check its pixels.
    for (int ty = 0; ty < kBgTilesY; ++ty)
        for (int tx = 0; tx < kBgTilesX; ++tx)
        {
            int tw = 0, th = 0;
            auto px = read_tile_pixels (f, 0, tx, ty, &tw, &th);
            EXRCORE_TEST (tw == kTileSize && th == kTileSize);
            check_tile_pixels (px, 0, tx, ty, tw, th);
        }

    // Read every foreground tile. Their locations are far from the
    // header yet must still resolve to the foreground layer's pixels.
    for (int ty = 0; ty < kFgTilesY; ++ty)
        for (int tx = 0; tx < kFgTilesX; ++tx)
        {
            int tw = 0, th = 0;
            auto px = read_tile_pixels (f, 1, tx, ty, &tw, &th);
            EXRCORE_TEST (tw == kTileSize && th == kTileSize);
            check_tile_pixels (px, 1, tx, ty, tw, th);
        }

    // Spot-check that the far-offset tile really is distinct data from
    // the background layer at the same tile coordinates.
    {
        auto fgpx = read_tile_pixels (f, 1, kFgTilesX - 1, kFgTilesY - 1);
        auto bgpx = read_tile_pixels (f, 0, kFgTilesX - 1, kFgTilesY - 1);
        EXRCORE_TEST (fgpx[0] != bgpx[0]);
        EXRCORE_TEST (
            fgpx[0] ==
            pixel_value (1, kFgTilesX - 1, kFgTilesY - 1, 0, 0));
    }

    EXRCORE_TEST_RVAL (exr_finish (&f));
}

namespace {

// Open a read context with reconstruction disabled so failure results
// come straight from the recorded chunk locations rather than from a
// best-effort scan of the surrounding bytes.
static exr_context_t
open_strict_reader (MemoryFile& mf)
{
    exr_context_initializer_t cinit = stream_init (mf, false);
    cinit.flags |= EXR_CONTEXT_FLAG_DISABLE_CHUNK_RECONSTRUCTION;
    exr_context_t f = nullptr;
    EXRCORE_TEST_RVAL (exr_start_read (&f, "mem:tiled-parts.exr", &cinit));
    return f;
}

// Attempt to decode the last foreground tile and require that it fails
// with the given code, and that no output pixel was delivered.
static void
require_tile_delivery_failure (MemoryFile& mf, exr_result_t expected_rv)
{
    exr_context_t f = open_strict_reader (mf);

    const int tilex = kFgTilesX - 1;
    const int tiley = kFgTilesY - 1;

    exr_chunk_info_t      cinfo;
    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;

    exr_result_t info_rv =
        exr_read_tile_chunk_info (f, 1, tilex, tiley, 0, 0, &cinfo);

    if (info_rv != EXR_ERR_SUCCESS)
    {
        // Failure at the chunk location / leader stage: no pixel buffer
        // has been touched.
        EXRCORE_TEST_LOCATION (info_rv == expected_rv, 0, 0);
        EXRCORE_TEST_RVAL (exr_finish (&f));
        return;
    }

    // The leader was readable; failure must then occur while pulling the
    // pixel payload, before the unpack stage writes any user pixels.
    EXRCORE_TEST_RVAL (exr_decoding_initialize (f, 1, &cinfo, &decoder));

    const int tilew = decoder.channels[0].width;
    const int tileh = decoder.channels[0].height;
    std::vector<float> pixels (
        static_cast<size_t> (tilew) * static_cast<size_t> (tileh));
    const float poison = -777.0f;
    std::fill (pixels.begin (), pixels.end (), poison);

    decoder.channels[0].decode_to_ptr     = (uint8_t*) pixels.data ();
    decoder.channels[0].user_pixel_stride = kChannelBytes;
    decoder.channels[0].user_line_stride  = kChannelBytes * tilew;
    EXRCORE_TEST_RVAL (
        exr_decoding_choose_default_routines (f, 1, &decoder));

    exr_result_t run_rv = exr_decoding_run (f, 1, &decoder);
    EXRCORE_TEST_LOCATION (run_rv == expected_rv, tilex, tiley);

    // No pixel may have been delivered: the whole output stays poisoned.
    for (float v: pixels)
        EXRCORE_TEST_LOCATION (v == poison, tilex, tiley);

    EXRCORE_TEST_RVAL (exr_decoding_destroy (f, &decoder));
    EXRCORE_TEST_RVAL (exr_finish (&f));
}

} // namespace

//
// 2. Chunk locations beyond what the storage can provide fail before
//    any pixel is delivered. These cases are genuine physical
//    truncation / out-of-range reads (the bytes at the recorded
//    location do not exist), as opposed to a table entry that says the
//    chunk is absent.
//
void
testReadTilesTruncated (const std::string&)
{
    // All truncation cases use ZIP compression. A compressed chunk's
    // packed bytes are pulled with a mandatory full read into an internal
    // buffer and only decompressed into the user output afterwards, so a
    // physically unreadable location fails with EXR_ERR_READ_IO before
    // any output pixel is delivered. (Uncompressed planar data is instead
    // read straight into the output pointer and is explicitly allowed to
    // short-read, so it does not model the failure-before-delivery case.)

    // ---- Leader truncation: the chunk table points at a location whose
    // fixed-size tile leader cannot be fully read. --------------------
    {
        MemoryFile mf;
        build_multipart_tiled_file (mf, EXR_COMPRESSION_ZIP);

        exr_context_t             probe = open_strict_reader (mf);
        exr_chunk_info_t          good_info;
        EXRCORE_TEST_RVAL (exr_read_tile_chunk_info (
            probe, 1, kFgTilesX - 1, kFgTilesY - 1, 0, 0, &good_info));
        const uint64_t target_offset = good_info.data_offset;
        EXRCORE_TEST_RVAL (exr_finish (&probe));

        // Cut the stream partway through the 20-byte tile leader so the
        // leader read itself comes back short.
        EXRCORE_TEST (target_offset > 8);
        mf.read_limit = target_offset + 8;

        const int errors_before = mf.read_errors;
        require_tile_delivery_failure (mf, EXR_ERR_READ_IO);
        EXRCORE_TEST (mf.read_errors > errors_before);
    }

    // ---- Payload truncation: the leader is intact but the compressed
    // pixel data runs past the end of the backing bytes.
    //
    // The image uses ZIP compression deliberately: a compressed payload
    // is read with a mandatory full read, so a short read is reported as
    // EXR_ERR_READ_IO before decompression and before any output pixel is
    // touched. (An uncompressed chunk instead tolerates short reads and
    // zero-fills the missing tail, which is a different, defined
    // behavior and not a delivery failure.)
    {
        MemoryFile mf;
        build_multipart_tiled_file (mf, EXR_COMPRESSION_ZIP);

        exr_context_t    probe = open_strict_reader (mf);
        exr_chunk_info_t good_info;
        EXRCORE_TEST_RVAL (exr_read_tile_chunk_info (
            probe, 1, kFgTilesX - 1, kFgTilesY - 1, 0, 0, &good_info));
        EXRCORE_TEST (good_info.compression == EXR_COMPRESSION_ZIP);
        const uint64_t payload_start = good_info.data_offset;
        const uint64_t payload_end =
            good_info.data_offset + good_info.packed_size;
        EXRCORE_TEST_RVAL (exr_finish (&probe));
        EXRCORE_TEST (payload_end <= mf.bytes.size ());
        // The compressed block must carry more than a handful of bytes.
        EXRCORE_TEST (good_info.packed_size > 16);

        // Keep the full leader readable but drop the tail of the packed
        // payload, so the mandatory read comes back short.
        mf.read_limit = payload_end - 8;
        EXRCORE_TEST (mf.read_limit > payload_start);

        const int errors_before = mf.read_errors;
        require_tile_delivery_failure (mf, EXR_ERR_READ_IO);
        EXRCORE_TEST (mf.read_errors > errors_before);
    }

    // ---- Recorded location past the end of the storage: the 64-bit
    // table entry references bytes the backing store does not have,
    // while the size query honestly reports that larger logical size.
    {
        MemoryFile mf;
        build_multipart_tiled_file (mf, EXR_COMPRESSION_ZIP);

        uint64_t fg_table_off = 0;
        {
            exr_context_t probe = open_strict_reader (mf);
            EXRCORE_TEST_RVAL (
                exr_get_chunk_table_offset (probe, 1, &fg_table_off));
            EXRCORE_TEST_RVAL (exr_finish (&probe));
        }

        const uint64_t original_size = mf.bytes.size ();
        // A location that needs the full 64-bit field: 4 GiB past the
        // real end, so any 32-bit truncation of the offset would point
        // somewhere entirely different instead of failing cleanly.
        const uint64_t bogus_offset =
            (uint64_t (1) << 32) + original_size + 4096;
        store_le64 (
            mf.bytes.data () +
                fg_table_off +
                    static_cast<uint64_t> (kFgTilesX * kFgTilesY - 1) *
                        sizeof (uint64_t),
            bogus_offset);
        mf.reported_size = static_cast<int64_t> (bogus_offset + 4096);

        const int errors_before = mf.read_errors;
        require_tile_delivery_failure (mf, EXR_ERR_READ_IO);
        EXRCORE_TEST (mf.read_errors > errors_before);
    }
}

//
// 3. A chunk the file genuinely does not contain: the table entry is
//    zero (the value an interrupted write leaves behind), the storage
//    itself is whole and every other chunk readable. This must be
//    reported as an incomplete chunk table, distinct from the physical
//    truncation failures above.
//
void
testReadTilesMissingChunk (const std::string&)
{
    MemoryFile mf;
    build_multipart_tiled_file (mf);

    uint64_t fg_table_off = 0;
    int32_t  fgchunks     = 0;
    {
        exr_context_t probe = open_strict_reader (mf);
        EXRCORE_TEST_RVAL (
            exr_get_chunk_table_offset (probe, 1, &fg_table_off));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (probe, 1, &fgchunks));
        EXRCORE_TEST_RVAL (exr_finish (&probe));
    }

    const int missing_chunk = kFgTilesX * kFgTilesY - 1;
    const uint64_t missing_entry_pos =
        fg_table_off +
            static_cast<uint64_t> (missing_chunk) * sizeof (uint64_t);

    // The entry currently holds a valid far offset; zero it to mark the
    // chunk as absent without touching any pixel bytes.
    EXRCORE_TEST (load_le64 (mf.bytes.data () + missing_entry_pos) != 0);
    store_le64 (mf.bytes.data () + missing_entry_pos, 0);
    EXRCORE_TEST (mf.read_limit == 0);

    // ---- With reconstruction disabled, the missing entry is reported
    // directly as an incomplete chunk table. --------------------------
    {
        exr_context_t f = open_strict_reader (mf);

        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE,
            exr_validate_chunk_table (f, 1));

        exr_chunk_info_t cinfo;
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE,
            exr_read_tile_chunk_info (
                f,
                1,
                kFgTilesX - 1,
                kFgTilesY - 1,
                0,
                0,
                &cinfo));
        // The storage was never physically unable to supply bytes: a
        // missing entry is not a truncated read, so no read error was
        // raised (contrast with testReadTilesTruncated).
        EXRCORE_TEST (mf.read_errors == 0);

        // A different, still-present foreground tile reads normally and
        // delivers its own pixels, proving the failure is local to the
        // missing chunk.
        int tw = 0, th = 0;
        auto px = read_tile_pixels (f, 1, 0, 0, &tw, &th);
        check_tile_pixels (px, 1, 0, 0, tw, th);

        // The whole background part is unaffected too.
        auto bgpx = read_tile_pixels (f, 0, 0, 0, &tw, &th);
        check_tile_pixels (bgpx, 0, 0, 0, tw, th);

        EXRCORE_TEST_RVAL (exr_finish (&f));
    }

    // ---- With reconstruction enabled, a whole-storage scan walks the
    // chunk leaders and is able to rediscover the chunk whose table entry
    // was zeroed, so the read succeeds and delivers the original pixels.
    // This is the library's recovery path for an interrupted write; the
    // point of this case is that it never reports a physical read error.
    {
        exr_context_initializer_t cinit = stream_init (mf, false);
        exr_context_t             f     = nullptr;
        EXRCORE_TEST_RVAL (
            exr_start_read (&f, "mem:tiled-parts.exr", &cinit));

        exr_chunk_info_t cinfo;
        exr_result_t rv = exr_read_tile_chunk_info (
            f, 1, kFgTilesX - 1, kFgTilesY - 1, 0, 0, &cinfo);
        // Reconstruction either locates the chunk (success) or, on a
        // storage where it cannot, flags an incomplete table / bad leader.
        // A physical I/O error is never the answer: the storage is whole.
        EXRCORE_TEST (
            rv == EXR_ERR_SUCCESS ||
            rv == EXR_ERR_INCOMPLETE_CHUNK_TABLE ||
            rv == EXR_ERR_BAD_CHUNK_LEADER);
        EXRCORE_TEST (rv != EXR_ERR_READ_IO);
        // Reconstruction only reads bytes that exist, so it never trips
        // the stream's short-read / out-of-range path.
        EXRCORE_TEST (mf.read_errors == 0);

        if (rv == EXR_ERR_SUCCESS)
        {
            // The rediscovered chunk must hand back this layer's own
            // pixels, not data from another tile.
            int tw = 0, th = 0;
            auto px = read_tile_pixels (
                f, 1, kFgTilesX - 1, kFgTilesY - 1, &tw, &th);
            check_tile_pixels (
                px, 1, kFgTilesX - 1, kFgTilesY - 1, tw, th);
        }

        // Present chunks are still delivered correctly.
        int tw = 0, th = 0;
        auto px = read_tile_pixels (f, 1, 0, 0, &tw, &th);
        check_tile_pixels (px, 1, 0, 0, tw, th);

        EXRCORE_TEST_RVAL (exr_finish (&f));
    }
}
