// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenEXR Project.

//
// Regression tests for reconstructing an incomplete / corrupt chunk
// offset table.
//
// The scenario is a multi-part EXR where the same source material is
// stored as separate parts (e.g. an art plate and its matte) which can
// share identical data windows and coordinates. When a writer crashes
// before flushing the chunk offset tables, the reader must rebuild the
// table by scanning the chunk data and must:
//
//   * attribute every chunk to the part named in its own chunk leader,
//     so parts covering the same coordinates stay independent;
//   * derive each chunk's slot from the owning part's channels, data
//     window and storage / compression layout;
//   * keep a chunk that genuinely cannot be located distinct (offset
//     stays zero, reported as an incomplete table) from a valid, present
//     chunk whose pixels simply decode to all black.
//

#include "read.h"

#include "test_value.h"

#include "openexr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#    include <unistd.h>
#endif

#include <iostream>
#include <memory>
#include <string>
#include <vector>

static void
reconstruct_err_cb (
    exr_const_context_t f, int code, const char* msg)
{
    (void) f;
    // expected on the deliberately damaged files; keep noise down
    std::cerr << "(expected) err_cb " << code << ": " << msg << std::endl;
}

// Number of scanline chunks written for a part: each chunk holds the
// number of scanlines dictated by the compression.
static int32_t
scanline_chunk_count (int height, exr_compression_t ctype)
{
    int lines_per_chunk = exr_compression_lines_per_chunk (ctype);
    EXRCORE_TEST (lines_per_chunk > 0);
    return (height + lines_per_chunk - 1) / lines_per_chunk;
}

// Write a single FLOAT channel named "V" holding a constant value into
// every scanline chunk of a part.
static void
write_constant_scanline_part (
    exr_context_t outf,
    int           part_index,
    int           width,
    int           height,
    exr_compression_t ctype,
    float         value)
{
    int lines_per_chunk = exr_compression_lines_per_chunk (ctype);
    EXRCORE_TEST (lines_per_chunk > 0);

    std::vector<float> row ((size_t) width, value);

    for (int32_t ci = 0; ci < scanline_chunk_count (height, ctype); ++ci)
    {
        int y = ci * lines_per_chunk;

        exr_chunk_info_t cinfo;
        EXRCORE_TEST_RVAL (
            exr_write_scanline_chunk_info (outf, part_index, y, &cinfo));

        exr_encode_pipeline_t encoder =
            EXR_ENCODE_PIPELINE_INITIALIZER;
        EXRCORE_TEST_RVAL (
            exr_encoding_initialize (outf, part_index, &cinfo, &encoder));
        EXRCORE_TEST (encoder.channel_count == 1);

        encoder.channels[0].encode_from_ptr   = (const uint8_t*) row.data ();
        encoder.channels[0].user_pixel_stride = (int) sizeof (float);
        encoder.channels[0].user_line_stride  = (int) (width * sizeof (float));

        EXRCORE_TEST_RVAL (
            exr_encoding_choose_default_routines (outf, part_index, &encoder));
        EXRCORE_TEST_RVAL (
            exr_encoding_run (outf, part_index, &encoder));
        EXRCORE_TEST_RVAL (exr_encoding_destroy (outf, &encoder));
    }
}

// Write a single FLOAT channel holding a constant value into every
// tile (one level) of a tiled part.
static void
write_constant_tiled_part (
    exr_context_t outf,
    int           part_index,
    int           width,
    int           height,
    int           tile_w,
    int           tile_h,
    float         value)
{
    const int ntx = (width + tile_w - 1) / tile_w;
    const int nty = (height + tile_h - 1) / tile_h;

    std::vector<float> tile_pixels (
        (size_t) tile_w * (size_t) tile_h, value);

    for (int ty = 0; ty < nty; ++ty)
    {
        for (int tx = 0; tx < ntx; ++tx)
        {
            exr_chunk_info_t cinfo;
            EXRCORE_TEST_RVAL (exr_write_tile_chunk_info (
                outf, part_index, tx, ty, 0, 0, &cinfo));

            exr_encode_pipeline_t encoder =
                EXR_ENCODE_PIPELINE_INITIALIZER;
            EXRCORE_TEST_RVAL (
                exr_encoding_initialize (outf, part_index, &cinfo, &encoder));
            EXRCORE_TEST (encoder.channel_count == 1);

            encoder.channels[0].encode_from_ptr   =
                (const uint8_t*) tile_pixels.data ();
            encoder.channels[0].user_pixel_stride = (int) sizeof (float);
            encoder.channels[0].user_line_stride  =
                (int) (tile_w * sizeof (float));

            EXRCORE_TEST_RVAL (
                exr_encoding_choose_default_routines (
                    outf, part_index, &encoder));
            EXRCORE_TEST_RVAL (
                exr_encoding_run (outf, part_index, &encoder));
            EXRCORE_TEST_RVAL (exr_encoding_destroy (outf, &encoder));
        }
    }
}

// Build a two-part scanline file. Both parts cover the same data window
// (identical coordinates) but use different channels and compression so
// their on-disk chunk layout differs, mirroring a plate plus its matte.
// The matte part is filled entirely with black (0.0).
static void
write_two_part_file (
    const std::string& path, int width, int height)
{
    exr_context_t outf;
    int           pidx;

    EXRCORE_TEST_RVAL (
        exr_start_write (
            &outf, path.c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));

    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "plate", EXR_STORAGE_SCANLINE, &pidx));
    EXRCORE_TEST (pidx == 0);
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        outf, 0, width, height, EXR_COMPRESSION_ZIP));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf, 0, "V", EXR_PIXEL_FLOAT,
        EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));

    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "matte", EXR_STORAGE_SCANLINE, &pidx));
    EXRCORE_TEST (pidx == 1);
    // ZIPS keeps one scanline per chunk, a deliberately different layout
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        outf, 1, width, height, EXR_COMPRESSION_ZIPS));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf, 1, "A", EXR_PIXEL_FLOAT,
        EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));

    EXRCORE_TEST_RVAL (exr_write_header (outf));

    // distinct pixel content per part
    write_constant_scanline_part (
        outf, 0, width, height, EXR_COMPRESSION_ZIP, 1.0f);
    write_constant_scanline_part (
        outf, 1, width, height, EXR_COMPRESSION_ZIPS, 0.0f);

    EXRCORE_TEST_RVAL (exr_finish (&outf));
}

// Zero every entry of a part's chunk offset table, simulating a writer
// that crashed after streaming the chunk data but before flushing the
// table. Returns the table's file offset and size.
static void
zero_chunk_table (
    const std::string& path,
    uint64_t           table_offset,
    int32_t            chunk_count)
{
    FILE* fp = fopen (path.c_str (), "r+b");
    EXRCORE_TEST (fp != NULL);
    EXRCORE_TEST (fseek (fp, (long) table_offset, SEEK_SET) == 0);

    std::vector<uint8_t> zeros ((size_t) chunk_count * sizeof (uint64_t), 0);
    EXRCORE_TEST (
        fwrite (zeros.data (), 1, zeros.size (), fp) == zeros.size ());
    EXRCORE_TEST (fclose (fp) == 0);
}

// Decode one scanline chunk of a part and return the first pixel value.
static float
read_first_pixel (
    exr_const_context_t inf,
    int                 part_index,
    int                 y)
{
    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL (
        exr_read_scanline_chunk_info (inf, part_index, y, &cinfo));

    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
    EXRCORE_TEST_RVAL (
        exr_decoding_initialize (inf, part_index, &cinfo, &decoder));
    EXRCORE_TEST (decoder.channel_count == 1);

    // the decoder fills the whole chunk, so size the buffer accordingly
    const size_t npixels =
        (size_t) cinfo.width * (size_t) cinfo.height;
    std::vector<float> buf (npixels, -999.0f);

    decoder.channels[0].decode_to_ptr          = (uint8_t*) buf.data ();
    decoder.channels[0].user_pixel_stride      = (int) sizeof (float);
    decoder.channels[0].user_line_stride       =
        (int) (cinfo.width * sizeof (float));
    decoder.channels[0].user_bytes_per_element = (int) sizeof (float);
    decoder.channels[0].user_data_type         = EXR_PIXEL_FLOAT;

    EXRCORE_TEST_RVAL (
        exr_decoding_choose_default_routines (inf, part_index, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_run (inf, part_index, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_destroy (inf, &decoder));
    return buf[0];
}

// Decode one tile of a tiled part and return the first pixel value.
static float
read_first_tile_pixel (
    exr_const_context_t inf,
    int                 part_index,
    int                 tilex,
    int                 tiley)
{
    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL (exr_read_tile_chunk_info (
        inf, part_index, tilex, tiley, 0, 0, &cinfo));

    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
    EXRCORE_TEST_RVAL (
        exr_decoding_initialize (inf, part_index, &cinfo, &decoder));
    EXRCORE_TEST (decoder.channel_count == 1);

    const size_t npixels =
        (size_t) cinfo.width * (size_t) cinfo.height;
    std::vector<float> buf (npixels, -999.0f);

    decoder.channels[0].decode_to_ptr          = (uint8_t*) buf.data ();
    decoder.channels[0].user_pixel_stride      = (int) sizeof (float);
    decoder.channels[0].user_line_stride       =
        (int) (cinfo.width * sizeof (float));
    decoder.channels[0].user_bytes_per_element = (int) sizeof (float);
    decoder.channels[0].user_data_type         = EXR_PIXEL_FLOAT;

    EXRCORE_TEST_RVAL (
        exr_decoding_choose_default_routines (inf, part_index, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_run (inf, part_index, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_destroy (inf, &decoder));
    return buf[0];
}

void
testReadMultiPart (const std::string& tempdir)
{
    const int width  = 12;
    const int height = 8;

    const std::string fn = tempdir + "reconstruct_multipart.exr";
    write_two_part_file (fn, width, height);

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &reconstruct_err_cb;

    // ---- sanity: the intact file reads back with the right content ----
    {
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));

        int32_t cnt0, cnt1;
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 0, &cnt0));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 1, &cnt1));
        EXRCORE_TEST (cnt0 == scanline_chunk_count (height, EXR_COMPRESSION_ZIP));
        EXRCORE_TEST (cnt1 == scanline_chunk_count (height, EXR_COMPRESSION_ZIPS));

        EXRCORE_TEST (read_first_pixel (inf, 0, 0) == 1.0f);
        EXRCORE_TEST (read_first_pixel (inf, 1, 0) == 0.0f);

        EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 0));
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 1));
        exr_finish (&inf);
    }

    // capture each part's table location before damaging the file
    uint64_t off0 = 0, off1 = 0;
    int32_t  cnt0 = 0, cnt1 = 0;
    {
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));
        EXRCORE_TEST_RVAL (exr_get_chunk_table_offset (inf, 0, &off0));
        EXRCORE_TEST_RVAL (exr_get_chunk_table_offset (inf, 1, &off1));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 0, &cnt0));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 1, &cnt1));
        exr_finish (&inf);
    }

    // ---- damage both tables: every chunk must be reconstructed and
    //      attributed to its own part from its leader ----
    zero_chunk_table (fn, off0, cnt0);
    zero_chunk_table (fn, off1, cnt1);

    {
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));

        // all chunks were present on disk, so the tables validate again
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 0));
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 1));

        // each part keeps its own pixels despite sharing coordinates
        EXRCORE_TEST (read_first_pixel (inf, 0, 0) == 1.0f);
        EXRCORE_TEST (read_first_pixel (inf, 1, 0) == 0.0f);

        // read every chunk of both parts to exercise every rebuilt slot
        for (int ci = 0; ci < cnt0; ++ci)
        {
            int y = ci * 16; // ZIP: 16 scanlines per chunk
            EXRCORE_TEST (read_first_pixel (inf, 0, y) == 1.0f);
        }
        for (int ci = 0; ci < cnt1; ++ci)
        {
            int y = ci; // ZIPS: 1 scanline per chunk
            EXRCORE_TEST (read_first_pixel (inf, 1, y) == 0.0f);
        }
        exr_finish (&inf);
    }

    remove (fn.c_str ());
}

// Truncate a file so its last @p remove_bytes bytes are gone, simulating
// a writer that crashed while streaming the final chunk(s).
static void
truncate_file_tail (const std::string& path, uint64_t remove_bytes)
{
    FILE* fp = fopen (path.c_str (), "r+b");
    EXRCORE_TEST (fp != NULL);
    EXRCORE_TEST (fseek (fp, 0, SEEK_END) == 0);
    long end = ftell (fp);
    EXRCORE_TEST (end > 0 && (uint64_t) end > remove_bytes);
    EXRCORE_TEST (ftruncate (fileno (fp), end - (long) remove_bytes) == 0);
    EXRCORE_TEST (fclose (fp) == 0);
}

// A single-part file whose last scanline chunk was never written: the
// rebuilt table must mark only that chunk missing while recovering all
// earlier ones, and the missing entry must stay distinct from a present
// all-black chunk.
void
testReadIncompleteReconstruction (const std::string& tempdir)
{
    const int      width  = 7;
    const int      height = 5;
    const exr_compression_t ctype = EXR_COMPRESSION_ZIPS; // 1 line/chunk
    const std::string fn = tempdir + "reconstruct_incomplete.exr";

    // build a complete, valid, all-black file first
    {
        exr_context_t outf;
        EXRCORE_TEST_RVAL (
            exr_start_write (
                &outf, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));
        int pidx = 0;
        EXRCORE_TEST_RVAL (
            exr_add_part (outf, "black", EXR_STORAGE_SCANLINE, &pidx));
        EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
            outf, 0, width, height, ctype));
        EXRCORE_TEST_RVAL (exr_add_channel (
            outf, 0, "A", EXR_PIXEL_FLOAT,
            EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
        EXRCORE_TEST_RVAL (exr_write_header (outf));
        write_constant_scanline_part (outf, 0, width, height, ctype, 0.0f);
        EXRCORE_TEST_RVAL (exr_finish (&outf));
    }

    // locate the offset of the last chunk so it can be removed cleanly
    uint64_t last_chunk_offset = 0;
    {
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), NULL));
        int32_t cnt = 0;
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 0, &cnt));
        EXRCORE_TEST (cnt == height);
        uint64_t* table = NULL;
        int32_t   tcnt  = 0;
        EXRCORE_TEST_RVAL (exr_get_chunk_table (inf, 0, &table, &tcnt));
        last_chunk_offset = table[cnt - 1];
        exr_finish (&inf);
    }

    // zero the offset table (force reconstruction) and drop the final
    // chunk's bytes from the end of the file
    {
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), NULL));
        uint64_t table_off = 0;
        int32_t  cnt       = 0;
        EXRCORE_TEST_RVAL (
            exr_get_chunk_table_offset (inf, 0, &table_off));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 0, &cnt));
        exr_finish (&inf);
        zero_chunk_table (fn, table_off, cnt);
    }

    long file_end = 0;
    {
        FILE* fp = fopen (fn.c_str (), "rb");
        EXRCORE_TEST (fp != NULL);
        EXRCORE_TEST (fseek (fp, 0, SEEK_END) == 0);
        file_end = ftell (fp);
        EXRCORE_TEST (fclose (fp) == 0);
    }
    truncate_file_tail (
        fn, (uint64_t) file_end - last_chunk_offset);

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &reconstruct_err_cb;

    exr_context_t inf;
    EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));

    int32_t cnt = 0;
    EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 0, &cnt));
    EXRCORE_TEST (cnt == height);

    // table cannot validate: the last chunk is genuinely absent
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INCOMPLETE_CHUNK_TABLE,
        exr_validate_chunk_table (inf, 0));

    // earlier chunks are recovered and read as valid (all-black) data
    for (int y = 0; y < height - 1; ++y)
        EXRCORE_TEST (read_first_pixel (inf, 0, y) == 0.0f);

    // the missing chunk is reported as incomplete, never as black data
    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INCOMPLETE_CHUNK_TABLE,
        exr_read_scanline_chunk_info (inf, 0, height - 1, &cinfo));

    exr_finish (&inf);
    remove (fn.c_str ());
}

// Mixed storage: a scanline plate part and a tiled matte part covering
// the same coordinates. After both offset tables are zeroed, the rebuild
// must keep each part's chunks in the slots derived from its own storage
// mode and layout, never cross-attributing them.
void
testReadMixedStorageReconstruction (const std::string& tempdir)
{
    const int width   = 10;
    const int height  = 6;
    const int tile_w  = 4;
    const int tile_h  = 3;
    const std::string fn = tempdir + "reconstruct_mixed.exr";

    {
        exr_context_t outf;
        EXRCORE_TEST_RVAL (
            exr_start_write (
                &outf, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));

        int pidx = 0;
        EXRCORE_TEST_RVAL (
            exr_add_part (outf, "plate", EXR_STORAGE_SCANLINE, &pidx));
        EXRCORE_TEST (pidx == 0);
        EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
            outf, 0, width, height, EXR_COMPRESSION_ZIPS));
        EXRCORE_TEST_RVAL (exr_add_channel (
            outf, 0, "V", EXR_PIXEL_FLOAT,
            EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));

        EXRCORE_TEST_RVAL (
            exr_add_part (outf, "matte", EXR_STORAGE_TILED, &pidx));
        EXRCORE_TEST (pidx == 1);
        EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
            outf, 1, width, height, EXR_COMPRESSION_ZIP));
        EXRCORE_TEST_RVAL (exr_add_channel (
            outf, 1, "A", EXR_PIXEL_FLOAT,
            EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
        EXRCORE_TEST_RVAL (exr_set_tile_descriptor (
            outf, 1, tile_w, tile_h,
            EXR_TILE_ONE_LEVEL, EXR_TILE_ROUND_DOWN));

        EXRCORE_TEST_RVAL (exr_write_header (outf));

        write_constant_scanline_part (
            outf, 0, width, height, EXR_COMPRESSION_ZIPS, 1.0f);
        write_constant_tiled_part (
            outf, 1, width, height, tile_w, tile_h, 0.0f);

        EXRCORE_TEST_RVAL (exr_finish (&outf));
    }

    // zero both offset tables to force reconstruction
    {
        uint64_t table_off[2] = {0, 0};
        int32_t  cnt[2]       = {0, 0};

        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &reconstruct_err_cb;
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));
        for (int p = 0; p < 2; ++p)
        {
            EXRCORE_TEST_RVAL (
                exr_get_chunk_table_offset (inf, p, &table_off[p]));
            EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, p, &cnt[p]));
        }
        exr_finish (&inf);

        for (int p = 0; p < 2; ++p)
            zero_chunk_table (fn, table_off[p], cnt[p]);
    }

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &reconstruct_err_cb;
    exr_context_t inf;
    EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));

    // every rebuilt table is complete because all chunks exist on disk
    EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 0));
    EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 1));

    // scanline plate keeps its own (non-black) pixels
    for (int y = 0; y < height; ++y)
        EXRCORE_TEST (read_first_pixel (inf, 0, y) == 1.0f);

    // tiled matte keeps its own (all-black) pixels, indexed by tile
    const int ntx = (width + tile_w - 1) / tile_w;
    const int nty = (height + tile_h - 1) / tile_h;
    for (int ty = 0; ty < nty; ++ty)
        for (int tx = 0; tx < ntx; ++tx)
            EXRCORE_TEST (read_first_tile_pixel (inf, 1, tx, ty) == 0.0f);

    exr_finish (&inf);
    remove (fn.c_str ());
}

// Cross-part independence: two parts share identical coordinates. After
// zeroing both offset tables and removing the matte part's final chunk
// (the realistic tail-truncation of a crashed writer), the intact plate
// part must still reconstruct and validate completely, while only the
// matte part reports the missing region. A failure in one part's table
// must never block recovery of the other part's pixels.
void
testReadCrossPartIndependence (const std::string& tempdir)
{
    const int width  = 12;
    const int height = 8;
    const std::string fn = tempdir + "reconstruct_crosspart.exr";
    write_two_part_file (fn, width, height);

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &reconstruct_err_cb;

    uint64_t off0 = 0, off1 = 0;
    int32_t  cnt0 = 0, cnt1 = 0;
    uint64_t matte_last_offset = 0;
    {
        exr_context_t inf;
        EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));
        EXRCORE_TEST_RVAL (exr_get_chunk_table_offset (inf, 0, &off0));
        EXRCORE_TEST_RVAL (exr_get_chunk_table_offset (inf, 1, &off1));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 0, &cnt0));
        EXRCORE_TEST_RVAL (exr_get_chunk_count (inf, 1, &cnt1));
        uint64_t* table = NULL;
        int32_t   tcnt  = 0;
        EXRCORE_TEST_RVAL (exr_get_chunk_table (inf, 1, &table, &tcnt));
        matte_last_offset = table[cnt1 - 1];
        exr_finish (&inf);
    }

    // force reconstruction, then drop the matte's last chunk bytes
    zero_chunk_table (fn, off0, cnt0);
    zero_chunk_table (fn, off1, cnt1);

    long file_end = 0;
    {
        FILE* fp = fopen (fn.c_str (), "rb");
        EXRCORE_TEST (fp != NULL);
        EXRCORE_TEST (fseek (fp, 0, SEEK_END) == 0);
        file_end = ftell (fp);
        EXRCORE_TEST (fclose (fp) == 0);
    }
    truncate_file_tail (
        fn, (uint64_t) file_end - matte_last_offset);

    exr_context_t inf;
    EXRCORE_TEST_RVAL (exr_start_read (&inf, fn.c_str (), &cinit));

    // the plate part is fully present and independent of the matte failure
    EXRCORE_TEST_RVAL (exr_validate_chunk_table (inf, 0));
    for (int ci = 0; ci < cnt0; ++ci)
        EXRCORE_TEST (read_first_pixel (inf, 0, ci * 16) == 1.0f);

    // the matte part only loses its final chunk
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INCOMPLETE_CHUNK_TABLE,
        exr_validate_chunk_table (inf, 1));
    for (int y = 0; y < height - 1; ++y)
        EXRCORE_TEST (read_first_pixel (inf, 1, y) == 0.0f);
    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INCOMPLETE_CHUNK_TABLE,
        exr_read_scanline_chunk_info (inf, 1, height - 1, &cinfo));

    exr_finish (&inf);
    remove (fn.c_str ());
}
