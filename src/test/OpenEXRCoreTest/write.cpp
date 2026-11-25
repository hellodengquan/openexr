// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenEXR Project.

#ifdef _WIN32
// windows is very particular about when windows.h is included
#include <windows.h>
#include <fileapi.h>
#include <inttypes.h>
#include <strsafe.h>
#else
#include <unistd.h>
#include <glob.h>
#endif

#include "write.h"

#include "test_value.h"

#include "openexr.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

static void
err_cb (exr_const_context_t f, exr_result_t code, const char* msg)
{
    std::cerr << "err_cb " << exr_get_error_code_as_string (code) << " ("
              << code << "): " << msg << std::endl;
}

void
testWriteBadArgs (const std::string& tempdir)
{
    exr_context_t             f;
    std::string               fn    = tempdir + "invalid.exr";
    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_start_write (NULL, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_start_write (&f, NULL, EXR_WRITE_FILE_DIRECTLY, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_start_write (&f, NULL, EXR_WRITE_FILE_DIRECTLY, &cinit));

    //    EXRCORE_TEST_RVAL_FAIL (EXR_ERR_FILE_ACCESS, exr_start_write (&f, fn.c_str (), &cinit));
}

void
testWriteBadFiles (const std::string& tempdir)
{}

void
testStartWriteScan (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testscan.exr";
    int           partidx;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_count (NULL, &partidx));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_count (outf, NULL));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_add_part (NULL, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_add_part (outf, "beauty", EXR_STORAGE_LAST_TYPE, &partidx));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST (partidx == 0);
    {
        uint64_t chunk_table_offset = 0;
        int      count              = 0;
        EXRCORE_TEST_RVAL (
            exr_get_chunk_table_offset (outf, 0, &chunk_table_offset));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INVALID_ARGUMENT,
            exr_get_chunk_table_offset (outf, 0, NULL));
        EXRCORE_TEST_RVAL (exr_get_count (outf, &count));
        EXRCORE_TEST (count == 1);
    }
    /* dup name check */
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_add_part (outf, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    /* this add should work, making 2 parts */
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "other", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST (partidx == 1);
    /* test repeated add failure doesn't leak */
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_add_part (outf, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 2);
    partidx = 0;

    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_NOT_OPEN_READ,
        exr_read_scanline_chunk_info (outf, partidx, 42, &cinfo));

    const char* partname;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_name (NULL, partidx, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_name (outf, partidx - 1, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_name (outf, partidx + 2, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_name (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_name (outf, partidx, &partname));
    EXRCORE_TEST (0 == strcmp (partname, "beauty"));

    exr_storage_t storage;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_storage (NULL, partidx, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_storage (outf, partidx - 1, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_storage (outf, partidx + 2, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_storage (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_storage (outf, partidx, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_SCANLINE);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_TILE_SCAN_MIXEDAPI,
        exr_set_tile_descriptor (
            outf, partidx, 32, 32, EXR_TILE_ONE_LEVEL, EXR_TILE_ROUND_DOWN));

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());
}

////////////////////////////////////////

void
testStartWriteDeepScan (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testdeepscan.exr";
    int           partidx;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_DEEP_SCANLINE, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 1);
    partidx = 0;

    exr_storage_t storage;
    EXRCORE_TEST_RVAL (exr_get_storage (outf, partidx, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_DEEP_SCANLINE);

    uint32_t verflags;
    EXRCORE_TEST_RVAL (
        exr_get_file_version_and_flags (outf, &verflags));
    EXRCORE_TEST (verflags == (2 | 0x00000800));

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());
}

////////////////////////////////////////

void
testStartWriteTile (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testattr.exr";
    int           partidx;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_count (NULL, &partidx));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_count (outf, NULL));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_add_part (NULL, "beauty", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 1);
    partidx = 0;

    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_NOT_OPEN_READ,
        exr_read_scanline_chunk_info (outf, partidx, 42, &cinfo));

    const char* partname;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_name (NULL, partidx, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_name (outf, partidx - 1, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_name (outf, partidx + 1, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_name (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_name (outf, partidx, &partname));
    EXRCORE_TEST (0 == strcmp (partname, "beauty"));

    exr_storage_t storage;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_storage (NULL, partidx, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_storage (outf, partidx - 1, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_storage (outf, partidx + 1, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_storage (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_storage (outf, partidx, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_TILED);

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());
}

////////////////////////////////////////

void
testStartWriteDeepTile (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testattr.exr";
    int           partidx;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_DEEP_TILED, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 1);
    partidx = 0;

    exr_storage_t storage;
    EXRCORE_TEST_RVAL (exr_get_storage (outf, partidx, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_DEEP_TILED);

    uint32_t verflags;
    EXRCORE_TEST_RVAL (
        exr_get_file_version_and_flags (outf, &verflags));
    EXRCORE_TEST (verflags == (2 | 0x00000800));

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());
}

void
testWriteBaseHeader (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testattr.exr";
    int           partidx;
    uint32_t      verflags;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_count (NULL, &partidx));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_count (outf, NULL));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_add_part (NULL, "beauty", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 1);
    partidx = 0;

    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_NOT_OPEN_READ,
        exr_read_scanline_chunk_info (outf, partidx, 42, &cinfo));

    const char* partname;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_name (NULL, partidx, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_name (outf, partidx - 1, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_name (outf, partidx + 1, &partname));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_name (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_name (outf, partidx, &partname));
    EXRCORE_TEST (0 == strcmp (partname, "beauty"));

    exr_storage_t storage;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_storage (NULL, partidx, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_storage (outf, partidx - 1, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_storage (outf, partidx + 1, &storage));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_storage (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_storage (outf, partidx, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_TILED);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_set_longname_support (NULL, 0));
    EXRCORE_TEST_RVAL (exr_set_longname_support (outf, 1));
    EXRCORE_TEST_RVAL (exr_attr_set_int (outf, partidx, "shortname", 42));
    EXRCORE_TEST_RVAL (exr_set_longname_support (outf, 0));
    EXRCORE_TEST_RVAL (exr_set_longname_support (outf, 1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "reallongreallongreallonglongchannelname",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_NAME_TOO_LONG, exr_set_longname_support (outf, 0));
    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_TILED, &partidx));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_NAME_TOO_LONG,
        exr_attr_set_int (
            outf, partidx, "areallongreallongreallonglongname", 42));

    EXRCORE_TEST_RVAL (exr_set_longname_support (outf, 1));
    EXRCORE_TEST_RVAL (exr_attr_set_int (
        outf, partidx, "areallongreallongreallonglongname", 42));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_NAME_TOO_LONG, exr_set_longname_support (outf, 0));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_zip_compression_level (NULL, 1, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_zip_compression_level (outf, -1, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_zip_compression_level (outf, 5, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_zip_compression_level (outf, 0, NULL));
    int zlev = -2;
    EXRCORE_TEST_RVAL (exr_get_zip_compression_level (outf, 0, &zlev));
    EXRCORE_TEST (zlev == -1);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_set_zip_compression_level (NULL, 0, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_zip_compression_level (outf, -1, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_zip_compression_level (outf, 5, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_set_zip_compression_level (outf, 0, -2));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_set_zip_compression_level (outf, 0, 42));
    EXRCORE_TEST_RVAL (exr_set_zip_compression_level (outf, 0, 4));
    EXRCORE_TEST_RVAL (exr_get_zip_compression_level (outf, 0, &zlev));
    EXRCORE_TEST (zlev == 4);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_dwa_compression_level (NULL, 0, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_dwa_compression_level (outf, -1, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_dwa_compression_level (outf, 5, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_dwa_compression_level (outf, 0, NULL));
    float dlev = -3.f;
    EXRCORE_TEST_RVAL (exr_get_dwa_compression_level (outf, 0, &dlev));
    EXRCORE_TEST (dlev == 45.f);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_set_dwa_compression_level (NULL, 0, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_dwa_compression_level (outf, -1, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_dwa_compression_level (outf, 5, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_dwa_compression_level (outf, 0, -2.f));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_dwa_compression_level (outf, 0, INFINITY));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_dwa_compression_level (outf, 0, NAN));
    EXRCORE_TEST_RVAL (exr_set_dwa_compression_level (outf, 0, 42.f));
    EXRCORE_TEST_RVAL (exr_get_dwa_compression_level (outf, 0, &dlev));
    EXRCORE_TEST (dlev == 42.f);
    EXRCORE_TEST_RVAL (exr_set_dwa_compression_level (outf, 0, 420.f));
    EXRCORE_TEST_RVAL (exr_get_dwa_compression_level (outf, 0, &dlev));
    EXRCORE_TEST (dlev == 420.f);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_lossy_htj2k_quality (NULL, 0, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_lossy_htj2k_quality (outf, -1, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_lossy_htj2k_quality (outf, 5, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_lossy_htj2k_quality (outf, 0, NULL));
    float hlev = -3.f;
    EXRCORE_TEST_RVAL (exr_get_lossy_htj2k_quality (outf, 0, &hlev));
    EXRCORE_TEST (hlev == 110.f);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_set_lossy_htj2k_quality (NULL, 0, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_lossy_htj2k_quality (outf, -1, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_lossy_htj2k_quality (outf, 5, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_lossy_htj2k_quality (outf, 0, 0.f));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_lossy_htj2k_quality (outf, 0, 151.f));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_lossy_htj2k_quality (outf, 0, INFINITY));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_lossy_htj2k_quality (outf, 0, NAN));
    EXRCORE_TEST_RVAL (exr_set_lossy_htj2k_quality (outf, 0, 42.f));
    EXRCORE_TEST_RVAL (exr_get_lossy_htj2k_quality (outf, 0, &hlev));
    EXRCORE_TEST (hlev == 42.f);
    EXRCORE_TEST_RVAL (exr_set_lossy_htj2k_quality (outf, 0, 150.f));
    EXRCORE_TEST_RVAL (exr_get_lossy_htj2k_quality (outf, 0, &hlev));
    EXRCORE_TEST (hlev == 150.f);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_zstd_compression_level (NULL, 0, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_zstd_compression_level (outf, -1, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_zstd_compression_level (outf, 5, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_zstd_compression_level (outf, 0, NULL));
    int slev = -1;
    EXRCORE_TEST_RVAL (exr_get_zstd_compression_level (outf, 0, &slev));
    EXRCORE_TEST (slev == 5);

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_set_zstd_compression_level (NULL, 0, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_zstd_compression_level (outf, -1, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_set_zstd_compression_level (outf, 5, 5));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_zstd_compression_level (outf, 0, -1));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_zstd_compression_level (outf, 0, 0));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_set_zstd_compression_level (outf, 0, 23));
    EXRCORE_TEST_RVAL (exr_set_zstd_compression_level (outf, 0, 10));
    EXRCORE_TEST_RVAL (exr_get_zstd_compression_level (outf, 0, &slev));
    EXRCORE_TEST (slev == 10);
    EXRCORE_TEST_RVAL (exr_set_zstd_compression_level (outf, 0, 22));
    EXRCORE_TEST_RVAL (exr_get_zstd_compression_level (outf, 0, &slev));
    EXRCORE_TEST (slev == 22);
    EXRCORE_TEST_RVAL (exr_set_zstd_compression_level (outf, 0, 1));
    EXRCORE_TEST_RVAL (exr_get_zstd_compression_level (outf, 0, &slev));
    EXRCORE_TEST (slev == 1);

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    exr_attr_box2i_t dataw = {{-2, -3}, {514, 515}};
    exr_attr_box2i_t dispw = {{0, 0}, {512, 512}};
    exr_attr_v2f_t   swc   = {0.5f, 0.5f};
    EXRCORE_TEST_RVAL (exr_initialize_required_attr (
        outf,
        partidx,
        &dataw,
        &dispw,
        1.f,
        &swc,
        1.f,
        EXR_LINEORDER_INCREASING_Y,
        EXR_COMPRESSION_ZIPS));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "R",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "G",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "B",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));

    EXRCORE_TEST_RVAL (exr_write_header (outf));
    EXRCORE_TEST_RVAL (
        exr_get_file_version_and_flags (outf, &verflags));
    EXRCORE_TEST (verflags == 2);

    uint64_t cto;
    EXRCORE_TEST_RVAL (
        exr_get_chunk_table_offset (outf, 0, &cto));
    EXRCORE_TEST (cto == 364);

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());

    ////

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_TILED, &partidx));
    dataw = {{0, 0}, {512, 512}};
    EXRCORE_TEST_RVAL (exr_initialize_required_attr (
        outf,
        partidx,
        &dataw,
        &dispw,
        1.f,
        &swc,
        1.f,
        EXR_LINEORDER_INCREASING_Y,
        EXR_COMPRESSION_ZIPS));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "R",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "G",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "B",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));

    EXRCORE_TEST_RVAL_FAIL (EXR_ERR_MISSING_REQ_ATTR, exr_write_header (outf));
    EXRCORE_TEST_RVAL (exr_set_tile_descriptor (
        outf, partidx, 32, 32, EXR_TILE_ONE_LEVEL, EXR_TILE_ROUND_DOWN));

    uint32_t              txsize, tysize;
    exr_tile_level_mode_t lmode;
    exr_tile_round_mode_t rmode;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_tile_descriptor (
            NULL, partidx, &txsize, &tysize, &lmode, &rmode));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_tile_descriptor (outf, -1, &txsize, &tysize, &lmode, &rmode));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_tile_descriptor (
            outf, partidx + 10, &txsize, &tysize, &lmode, &rmode));
    EXRCORE_TEST_RVAL (exr_get_tile_descriptor (
        outf, partidx, &txsize, &tysize, &lmode, &rmode));
    EXRCORE_TEST (txsize == 32 && tysize == 32);

    EXRCORE_TEST_RVAL (exr_write_header (outf));
    EXRCORE_TEST_RVAL (
        exr_get_file_version_and_flags (outf, &verflags));
    EXRCORE_TEST (verflags == (2 | 0x00000200));

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());

    ////

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL_FAIL (EXR_ERR_FILE_BAD_HEADER, exr_write_header (outf));
    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());
}

// Copying a header whose channel name is longer than the destination
// context allows must fail without leaving a dangling cached channel list.
static void
testCopyAttrsLongChannelName (const std::string& tempdir)
{
    exr_context_t outf, srcf, dstf;
    std::string   srcfn = tempdir + "testcopylong.exr";
    int           partidx;
    const char*   longname =
        "ChannelNameThatIsLongerThanThirtyOneBytes_0123456789";

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, srcfn.c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));
    EXRCORE_TEST_RVAL (exr_set_longname_support (outf, 1));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "src", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        outf, partidx, 1, 1, EXR_COMPRESSION_NONE));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf, partidx, longname, EXR_PIXEL_HALF, EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
    EXRCORE_TEST_RVAL (exr_write_header (outf));

    exr_chunk_info_t      cinfo;
    exr_encode_pipeline_t encoder;
    const uint16_t        hval = 0;
    EXRCORE_TEST_RVAL (exr_write_scanline_chunk_info (outf, 0, 0, &cinfo));
    EXRCORE_TEST_RVAL (exr_encoding_initialize (outf, 0, &cinfo, &encoder));
    encoder.channels[0].encode_from_ptr   = (const uint8_t*) &hval;
    encoder.channels[0].user_pixel_stride = 2;
    encoder.channels[0].user_line_stride  = 2;
    EXRCORE_TEST_RVAL (
        exr_encoding_choose_default_routines (outf, 0, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_run (outf, 0, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_destroy (outf, &encoder));
    EXRCORE_TEST_RVAL (exr_finish (&outf));

    EXRCORE_TEST_RVAL (exr_start_read (&srcf, srcfn.c_str (), NULL));
    EXRCORE_TEST_RVAL (exr_start_write (
        &dstf, (tempdir + "testcopylong_out.exr").c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));
    EXRCORE_TEST_RVAL (
        exr_add_part (dstf, "dst", EXR_STORAGE_SCANLINE, &partidx));

    EXRCORE_TEST (
        exr_copy_unset_attributes (dstf, partidx, srcf, 0) !=
        EXR_ERR_SUCCESS);

    // none of these may touch freed memory; the channel list is simply
    // missing, so it can be repaired by adding a channel
    EXRCORE_TEST (exr_write_header (dstf) != EXR_ERR_SUCCESS);
    EXRCORE_TEST_RVAL (exr_add_channel (
        dstf, partidx, "R", EXR_PIXEL_HALF, EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));

    EXRCORE_TEST_RVAL (exr_finish (&dstf));
    EXRCORE_TEST_RVAL (exr_finish (&srcf));
    remove (srcfn.c_str ());
    remove ((tempdir + "testcopylong_out.exr").c_str ());
}

void
testWriteAttrs (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testattr.exr";
    int           partidx;
    const char*   partname;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "tester", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        outf, partidx, 1, 1, EXR_COMPRESSION_ZIPS));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "R",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "G",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));
    EXRCORE_TEST_RVAL (exr_add_channel (
        outf,
        partidx,
        "B",
        EXR_PIXEL_HALF,
        EXR_PERCEPTUALLY_LOGARITHMIC,
        1,
        1));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_set_name (NULL, partidx, "a"));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_set_name (outf, -1, "a"));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_set_name (outf, 1, "a"));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_set_name (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_set_name (outf, partidx, ""));
    EXRCORE_TEST_RVAL (exr_set_name (outf, partidx, "beauty"));
    EXRCORE_TEST_RVAL (exr_get_name (outf, partidx, &partname));
    EXRCORE_TEST (0 == strcmp (partname, "beauty"));

    const exr_attr_chlist_t* chans;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_get_channels (NULL, partidx, &chans));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_get_channels (outf, -1, &chans));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_get_channels (outf, 1, &chans));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_get_channels (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_channels (outf, partidx, &chans));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG, exr_set_channels (NULL, partidx, chans));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_set_channels (outf, -1, chans));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_set_channels (outf, 1, chans));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT, exr_set_channels (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_set_channels (outf, partidx, chans));
    EXRCORE_TEST_RVAL (exr_get_channels (outf, partidx, &chans));

#define TEST_CORNER_CASE_GET(fn, arg)                                          \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG, fn (NULL, partidx, arg));                 \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, fn (outf, -1, arg));                    \
    EXRCORE_TEST_RVAL_FAIL (EXR_ERR_ARGUMENT_OUT_OF_RANGE, fn (outf, 1, arg)); \
    EXRCORE_TEST_RVAL (fn (outf, partidx, arg))
#define TEST_CORNER_CASE_SET(fn, arg)                                          \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG, fn (NULL, partidx, arg));                 \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE, fn (outf, -1, arg));                    \
    EXRCORE_TEST_RVAL_FAIL (EXR_ERR_ARGUMENT_OUT_OF_RANGE, fn (outf, 1, arg)); \
    EXRCORE_TEST_RVAL (fn (outf, partidx, arg))

#define TEST_CORNER_CASE_NAME(fnt, arg, alt)                                   \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_set_##fnt (NULL, partidx, #arg, &arg));                       \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, -1, #arg, &arg));                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, 1, #arg, &arg));                             \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_set_##fnt (outf, partidx, #arg, NULL));                       \
    EXRCORE_TEST_RVAL (exr_attr_set_##fnt (outf, partidx, #arg, &arg));        \
    alt altv = 0;                                                              \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ATTR_TYPE_MISMATCH,                                            \
        exr_attr_set_##alt (outf, partidx, #arg, altv));                       \
    memset (&arg, 0, sizeof (arg));                                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_get_##fnt (NULL, partidx, #arg, &arg));                       \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, -1, #arg, &arg));                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, 1, #arg, &arg));                             \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, NULL, &arg));                       \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, "", &arg));                         \
    EXRCORE_TEST_RVAL (exr_attr_get_##fnt (outf, partidx, #arg, &arg))

#define TEST_CORNER_CASE_NAME_C(fnt, arg, cnt, oarg, alt)                      \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_set_##fnt (NULL, partidx, #arg, cnt, arg));                   \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, -1, #arg, cnt, arg));                        \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, 1, #arg, cnt, arg));                         \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_set_##fnt (outf, partidx, #arg, -1, arg));                    \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_set_##fnt (outf, partidx, #arg, cnt, NULL));                  \
    EXRCORE_TEST_RVAL (exr_attr_set_##fnt (outf, partidx, #arg, cnt, arg));    \
    alt altv = 0;                                                              \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ATTR_TYPE_MISMATCH,                                            \
        exr_attr_set_##alt (outf, partidx, #arg, altv));                       \
    memset (&arg, 0, sizeof (arg));                                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_get_##fnt (NULL, partidx, #arg, &cnt, oarg));                 \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, -1, #arg, &cnt, oarg));                      \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, 1, #arg, &cnt, oarg));                       \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, NULL, &cnt, oarg));                 \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, "", &cnt, oarg));                   \
    EXRCORE_TEST_RVAL (exr_attr_get_##fnt (outf, partidx, #arg, &cnt, oarg))

#define TEST_CORNER_CASE_NAME_V(fnt, arg, alt)                                 \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_set_##fnt (NULL, partidx, #arg, arg));                        \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, -1, #arg, arg));                             \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, 1, #arg, arg));                              \
    EXRCORE_TEST_RVAL (exr_attr_set_##fnt (outf, partidx, #arg, arg));         \
    alt altv = 0;                                                              \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ATTR_TYPE_MISMATCH,                                            \
        exr_attr_set_##alt (outf, partidx, #arg, altv));                       \
    memset (&arg, 0, sizeof (arg));                                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_get_##fnt (NULL, partidx, #arg, &arg));                       \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, -1, #arg, &arg));                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, 1, #arg, &arg));                             \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, NULL, &arg));                       \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, "", &arg));                         \
    EXRCORE_TEST_RVAL (exr_attr_get_##fnt (outf, partidx, #arg, &arg))

#define TEST_CORNER_CASE_NAME_NOALT(fnt, arg, oarg)                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_set_##fnt (NULL, partidx, #arg, arg));                        \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, -1, #arg, arg));                             \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_set_##fnt (outf, 1, #arg, arg));                              \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_set_##fnt (outf, partidx, #arg, NULL));                       \
    EXRCORE_TEST_RVAL (exr_attr_set_##fnt (outf, partidx, #arg, arg));         \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_MISSING_CONTEXT_ARG,                                           \
        exr_attr_get_##fnt (NULL, partidx, #arg, &oarg));                      \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, -1, #arg, &oarg));                           \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,                                         \
        exr_attr_get_##fnt (outf, 1, #arg, &oarg));                            \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, NULL, &oarg));                      \
    EXRCORE_TEST_RVAL_FAIL (                                                   \
        EXR_ERR_INVALID_ARGUMENT,                                              \
        exr_attr_get_##fnt (outf, partidx, "", &oarg));                        \
    EXRCORE_TEST_RVAL (exr_attr_get_##fnt (outf, partidx, #arg, &oarg))

    {
        int32_t version = -1;
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_set_version (outf, partidx, 2));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ARGUMENT_OUT_OF_RANGE, exr_set_version (outf, partidx, 0));
        TEST_CORNER_CASE_SET (exr_set_version, 1);
        TEST_CORNER_CASE_GET (exr_get_version, &version);
        EXRCORE_TEST (version == 1);
    }

    {
        exr_compression_t ct;
        TEST_CORNER_CASE_GET (exr_get_compression, &ct);
        EXRCORE_TEST (ct == EXR_COMPRESSION_ZIPS);
        TEST_CORNER_CASE_SET (exr_set_compression, EXR_COMPRESSION_ZIP);
        EXRCORE_TEST_RVAL (exr_get_compression (outf, partidx, &ct));
        EXRCORE_TEST (ct == EXR_COMPRESSION_ZIP);
    }

    {
        TEST_CORNER_CASE_SET (exr_set_chunk_count, 1);
    }

    {
        exr_attr_box2i_t dw;
        TEST_CORNER_CASE_GET (exr_get_data_window, &dw);
        TEST_CORNER_CASE_SET (exr_set_data_window, &dw);
    }

    {
        exr_attr_box2i_t dpw;
        TEST_CORNER_CASE_GET (exr_get_display_window, &dpw);
        TEST_CORNER_CASE_SET (exr_set_display_window, &dpw);
    }

    {
        uint8_t bytes_data[] = {0x76, 0x2f, 0x31, 0x01};
        exr_attr_bytes_t mybytes = {4, bytes_data};
        TEST_CORNER_CASE_NAME (bytes, mybytes, int);
    }

    {
        exr_lineorder_t lo;
        TEST_CORNER_CASE_GET (exr_get_lineorder, &lo);
        EXRCORE_TEST (lo == EXR_LINEORDER_INCREASING_Y);
        TEST_CORNER_CASE_SET (exr_set_lineorder, EXR_LINEORDER_DECREASING_Y);
        EXRCORE_TEST_RVAL (exr_get_lineorder (outf, partidx, &lo));
        EXRCORE_TEST (lo == EXR_LINEORDER_DECREASING_Y);
        EXRCORE_TEST_RVAL (
            exr_set_lineorder (outf, partidx, EXR_LINEORDER_INCREASING_Y));
    }

    {
        float par;
        TEST_CORNER_CASE_GET (exr_get_pixel_aspect_ratio, &par);
        TEST_CORNER_CASE_SET (exr_set_pixel_aspect_ratio, 1.f);
    }

    {
        exr_attr_v2f_t swc;
        TEST_CORNER_CASE_GET (exr_get_screen_window_center, &swc);
        TEST_CORNER_CASE_SET (exr_set_screen_window_center, &swc);
    }

    {
        float sww;
        TEST_CORNER_CASE_GET (exr_get_screen_window_width, &sww);
        TEST_CORNER_CASE_SET (exr_set_screen_window_width, 1.f);
    }

    {
        int intv = 42;
        TEST_CORNER_CASE_NAME_V (int, intv, float);
        EXRCORE_TEST (intv == 42);
    }

    {
        float floatv = 42.f;
        TEST_CORNER_CASE_NAME_V (float, floatv, int);
        EXRCORE_TEST (floatv == 42.f);
    }

    {
        double doublev = 42.0;
        TEST_CORNER_CASE_NAME_V (double, doublev, int);
        EXRCORE_TEST (doublev == 42.0);
    }

    {
        const exr_attr_chlist_t* addlchans = chans;
        TEST_CORNER_CASE_NAME_NOALT (channels, addlchans, addlchans);
    }

    {
        exr_attr_chromaticities_t mychroma = {
            1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
        TEST_CORNER_CASE_NAME (chromaticities, mychroma, int);
        EXRCORE_TEST (mychroma.red_y == 2.f);
        EXRCORE_TEST (mychroma.white_x == 7.f);
    }

    {
        exr_compression_t compt = EXR_COMPRESSION_ZIPS;
        TEST_CORNER_CASE_NAME_V (compression, compt, int);
        EXRCORE_TEST (compt == EXR_COMPRESSION_ZIPS);
    }

    {
        exr_envmap_t envmapt = EXR_ENVMAP_LATLONG;
        TEST_CORNER_CASE_NAME_V (envmap, envmapt, int);
        EXRCORE_TEST (envmapt == EXR_ENVMAP_LATLONG);
    }

    {
        float        myfvec[] = {1.f, 2.f};
        const float* outfvec  = NULL;
        int          fvsz     = 2;
        TEST_CORNER_CASE_NAME_C (float_vector, myfvec, fvsz, &outfvec, int);
        EXRCORE_TEST (fvsz == 2);
        EXRCORE_TEST (outfvec[1] == 2.f);
    }

    {
        exr_attr_keycode_t mykeycodet = {1, 2, 3, 4, 5, 6, 7};
        TEST_CORNER_CASE_NAME (keycode, mykeycodet, int);
        EXRCORE_TEST (mykeycodet.count == 4);
    }

    {
        exr_lineorder_t mylineordt = EXR_LINEORDER_DECREASING_Y;
        TEST_CORNER_CASE_NAME_V (lineorder, mylineordt, int);
        EXRCORE_TEST (mylineordt == EXR_LINEORDER_DECREASING_Y);
    }

    {
        exr_attr_box2i_t tb2i = {{1, 2}, {3, 4}};
        TEST_CORNER_CASE_NAME (box2i, tb2i, int);
        EXRCORE_TEST (tb2i.min.x == 1);
        EXRCORE_TEST (tb2i.min.y == 2);
        EXRCORE_TEST (tb2i.max.x == 3);
        EXRCORE_TEST (tb2i.max.y == 4);
    }

    {
        exr_attr_box2f_t tb2f = {{1.f, 2.f}, {3.f, 4.f}};
        TEST_CORNER_CASE_NAME (box2f, tb2f, int);
        EXRCORE_TEST (tb2f.min.x == 1.f);
        EXRCORE_TEST (tb2f.min.y == 2.f);
        EXRCORE_TEST (tb2f.max.x == 3.f);
        EXRCORE_TEST (tb2f.max.y == 4.f);
    }

    {
        exr_attr_v2i_t tv2i = {1, 2};
        TEST_CORNER_CASE_NAME (v2i, tv2i, int);
        EXRCORE_TEST (tv2i.x == 1);
        EXRCORE_TEST (tv2i.y == 2);
    }

    {
        exr_attr_m33f_t mym33f = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        TEST_CORNER_CASE_NAME (m33f, mym33f, int);
        EXRCORE_TEST (mym33f.m[3] == 4);
    }

    {
        exr_attr_m33d_t mym33d = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        TEST_CORNER_CASE_NAME (m33d, mym33d, int);
        EXRCORE_TEST (mym33d.m[3] == 4);
    }

    {
        exr_attr_m44f_t mym44f = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        TEST_CORNER_CASE_NAME (m44f, mym44f, int);
        EXRCORE_TEST (mym44f.m[3] == 4);
    }

    {
        exr_attr_m44d_t mym44d = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        TEST_CORNER_CASE_NAME (m44d, mym44d, int);
        EXRCORE_TEST (mym44d.m[3] == 4);
    }

    {
        uint8_t dummy[] = {
            0xDE,
            0xAD,
            0xBE,
            0xEF,
            0xDE,
            0xAD,
            0xBE,
            0xEF,
            0xDE,
            0xAD,
            0xBE,
            0xEF,
            0xDE,
            0xAD,
            0xBE,
            0xEF};
        exr_attr_preview_t mypreview = {2, 2, 0, dummy};
        TEST_CORNER_CASE_NAME (preview, mypreview, int);
    }

    {
        exr_attr_rational_t myrational = {24000, 1001};
        TEST_CORNER_CASE_NAME (rational, myrational, int);
        EXRCORE_TEST (myrational.num == 24000);
        EXRCORE_TEST (myrational.denom == 1001);
    }

    {
        const char* sname = "teststrattr";
        const char* scal  = "testvalue";
        const char* outsv;
        int32_t     outssz;
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_MISSING_CONTEXT_ARG,
            exr_attr_set_string (NULL, partidx, sname, scal));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ARGUMENT_OUT_OF_RANGE,
            exr_attr_set_string (outf, -1, sname, scal));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ARGUMENT_OUT_OF_RANGE,
            exr_attr_set_string (outf, 1, sname, scal));
        EXRCORE_TEST_RVAL (exr_attr_set_string (outf, partidx, sname, NULL));
        EXRCORE_TEST_RVAL (exr_attr_set_string (outf, partidx, sname, ""));
        EXRCORE_TEST_RVAL (exr_attr_set_string (outf, partidx, sname, NULL));
        EXRCORE_TEST_RVAL (exr_attr_set_string (outf, partidx, sname, scal));
        int altv = 0;
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ATTR_TYPE_MISMATCH,
            exr_attr_set_int (outf, partidx, sname, altv));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_MISSING_CONTEXT_ARG,
            exr_attr_get_string (NULL, partidx, sname, &outssz, &outsv));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ARGUMENT_OUT_OF_RANGE,
            exr_attr_get_string (outf, -1, sname, &outssz, &outsv));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_ARGUMENT_OUT_OF_RANGE,
            exr_attr_get_string (outf, 1, sname, &outssz, &outsv));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INVALID_ARGUMENT,
            exr_attr_get_string (outf, partidx, NULL, &outssz, &outsv));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INVALID_ARGUMENT,
            exr_attr_get_string (outf, partidx, "", &outssz, &outsv));
        EXRCORE_TEST_RVAL (
            exr_attr_get_string (outf, partidx, sname, &outssz, &outsv));
        EXRCORE_TEST (outssz == 9);
        EXRCORE_TEST (0 == strcmp (outsv, scal));
    }

    {
        const char* mysvec[]   = {"foo", "bar"};
        const char* outsvec[3] = {NULL, NULL, NULL};
        int         svsz       = 2;
        TEST_CORNER_CASE_NAME_C (string_vector, mysvec, svsz, outsvec, int);
        EXRCORE_TEST (svsz == 2);
    }

    {
        exr_attr_tiledesc_t mytiled = {
            32,
            32,
            EXR_PACK_TILE_LEVEL_ROUND (
                EXR_TILE_MIPMAP_LEVELS, EXR_TILE_ROUND_DOWN)};
        TEST_CORNER_CASE_NAME (tiledesc, mytiled, int);
    }

    {
        exr_attr_timecode_t mytimecode = {42, 84};
        TEST_CORNER_CASE_NAME (timecode, mytimecode, int);
        EXRCORE_TEST (mytimecode.time_and_flags == 42);
        EXRCORE_TEST (mytimecode.user_data == 84);
    }

    {
        exr_attr_v2f_t tv2f = {1.f, 2.f};
        TEST_CORNER_CASE_NAME (v2f, tv2f, int);
        EXRCORE_TEST (tv2f.x == 1.f);
        EXRCORE_TEST (tv2f.y == 2.f);
    }

    {
        exr_attr_v2d_t tv2d = {1.0, 2.0};
        TEST_CORNER_CASE_NAME (v2d, tv2d, int);
        EXRCORE_TEST (tv2d.x == 1.0);
        EXRCORE_TEST (tv2d.y == 2.0);
    }

    {
        exr_attr_v3i_t tv3i = {1, 2, 3};
        TEST_CORNER_CASE_NAME (v3i, tv3i, int);
        EXRCORE_TEST (tv3i.x == 1);
        EXRCORE_TEST (tv3i.y == 2);
        EXRCORE_TEST (tv3i.z == 3);
    }

    {
        exr_attr_v3f_t tv3f = {1.f, 2.f, 3.f};
        TEST_CORNER_CASE_NAME (v3f, tv3f, int);
        EXRCORE_TEST (tv3f.x == 1.f);
        EXRCORE_TEST (tv3f.y == 2.f);
        EXRCORE_TEST (tv3f.z == 3.f);
    }

    {
        exr_attr_v3d_t tv3d = {1.0, 2.0, 3.0};
        TEST_CORNER_CASE_NAME (v3d, tv3d, int);
        EXRCORE_TEST (tv3d.x == 1.0);
        EXRCORE_TEST (tv3d.y == 2.0);
        EXRCORE_TEST (tv3d.z == 3.0);
    }

    {
        EXRCORE_TEST_RVAL (
            exr_attr_set_user (outf, partidx, "tuser", "mytype", 4, "foo"));
        int32_t     sz;
        const void* ptr;
        const char* type;
        EXRCORE_TEST_RVAL (
            exr_attr_get_user (outf, partidx, "tuser", &type, &sz, &ptr));
        EXRCORE_TEST (0 == strcmp (type, "mytype"));
        EXRCORE_TEST (sz == 4);
        EXRCORE_TEST (0 == strcmp ((const char*) ptr, "foo"));
    }

    int32_t attrcnt = -1;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_attribute_count (NULL, partidx, &attrcnt));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_count (outf, -1, &attrcnt));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_count (outf, 1, &attrcnt));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_count (outf, partidx, NULL));
    EXRCORE_TEST_RVAL (exr_get_attribute_count (outf, partidx, &attrcnt));
    std::cout << "Testing writing of " << attrcnt << " attributes" << std::endl;

    const exr_attribute_t* attrget;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_attribute_by_index (
            NULL, partidx, EXR_ATTR_LIST_FILE_ORDER, 15, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_by_index (
            outf, -1, EXR_ATTR_LIST_FILE_ORDER, 15, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_by_index (
            outf, 1, EXR_ATTR_LIST_FILE_ORDER, 15, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_by_index (
            outf, partidx, EXR_ATTR_LIST_FILE_ORDER, 15, NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_by_index (
            outf, partidx, (exr_attr_list_access_mode_t) 42, 15, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_by_index (
            outf, partidx, EXR_ATTR_LIST_FILE_ORDER, -1, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_by_index (
            outf, partidx, EXR_ATTR_LIST_FILE_ORDER, 1234, &attrget));
    EXRCORE_TEST_RVAL (exr_get_attribute_by_index (
        outf, partidx, EXR_ATTR_LIST_FILE_ORDER, 15, &attrget));
    EXRCORE_TEST_RVAL (exr_get_attribute_by_index (
        outf, partidx, EXR_ATTR_LIST_SORTED_ORDER, 15, &attrget));

    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_attribute_by_name (NULL, partidx, "tv3f", &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_by_name (outf, -1, "tv3f", &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_by_name (outf, 1, "tv3f", &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_by_name (outf, partidx, "tv3f", NULL));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_by_name (outf, partidx, NULL, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_by_name (outf, partidx, "", &attrget));
    EXRCORE_TEST_RVAL (
        exr_get_attribute_by_name (outf, partidx, "tv3f", &attrget));

    const exr_attribute_t** attrlistget;
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_MISSING_CONTEXT_ARG,
        exr_get_attribute_list (
            NULL, partidx, EXR_ATTR_LIST_FILE_ORDER, &attrcnt, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_list (
            outf, -1, EXR_ATTR_LIST_FILE_ORDER, &attrcnt, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_ARGUMENT_OUT_OF_RANGE,
        exr_get_attribute_list (
            outf, 1, EXR_ATTR_LIST_FILE_ORDER, &attrcnt, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_list (
            outf, partidx, EXR_ATTR_LIST_FILE_ORDER, NULL, &attrget));
    EXRCORE_TEST_RVAL_FAIL (
        EXR_ERR_INVALID_ARGUMENT,
        exr_get_attribute_list (
            outf,
            partidx,
            (exr_attr_list_access_mode_t) 42,
            &attrcnt,
            &attrget));
    attrcnt = 1;
    EXRCORE_TEST_RVAL (exr_get_attribute_list (
        outf, partidx, EXR_ATTR_LIST_FILE_ORDER, &attrcnt, &attrget));
    attrlistget =
        (const exr_attribute_t**) malloc (attrcnt * sizeof (exr_attribute_t*));
    if (attrlistget)
    {
        EXRCORE_TEST_RVAL (exr_get_attribute_list (
            outf, partidx, EXR_ATTR_LIST_FILE_ORDER, &attrcnt, attrlistget));
        EXRCORE_TEST_RVAL (exr_get_attribute_list (
            outf, partidx, EXR_ATTR_LIST_SORTED_ORDER, &attrcnt, attrlistget));
        free (attrlistget);
    }

    EXRCORE_TEST_RVAL (exr_write_header (outf));
    exr_chunk_info_t cinfo;
    EXRCORE_TEST_RVAL (exr_write_scanline_chunk_info (outf, 0, 0, &cinfo));
    exr_encode_pipeline_t encoder;
    EXRCORE_TEST_RVAL (exr_encoding_initialize (outf, 0, &cinfo, &encoder));
    const uint8_t rgb[]                   = {0, 0, 0, 0, 0, 0};
    encoder.channels[0].encode_from_ptr   = rgb + 4;
    encoder.channels[0].user_pixel_stride = 6;
    encoder.channels[0].user_line_stride  = 6;
    encoder.channels[1].encode_from_ptr   = rgb + 2;
    encoder.channels[1].user_pixel_stride = 6;
    encoder.channels[1].user_line_stride  = 6;
    encoder.channels[2].encode_from_ptr   = rgb;
    encoder.channels[2].user_pixel_stride = 6;
    encoder.channels[2].user_line_stride  = 6;

    EXRCORE_TEST_RVAL (
        exr_encoding_choose_default_routines (outf, 0, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_run (outf, 0, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_destroy (outf, &encoder));
    EXRCORE_TEST_RVAL (exr_finish (&outf));

    EXRCORE_TEST_RVAL (exr_start_read (&outf, outfn.c_str (), &cinit));
    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());

    testCopyAttrsLongChannelName (tempdir);
}

void
testUpdateMeta (const std::string& tempdir)
{}

void
testWriteScans (const std::string& tempdir)
{}

// Edge tiles must be clipped to the requested level, including levels
// smaller than the nominal tile size. Tile coordinates are independent
// of the data window origin.
static void
testWriteTileChunkInfo (
    const std::string&    tempdir,
    exr_tile_level_mode_t mode,
    exr_tile_round_mode_t rounding,
    int                   offset)
{
    exr_context_t f;
    std::string   fn = tempdir + "tile_chunk_info.exr";
    int           partidx;
    const int     sizes[] = {6, 3, rounding == EXR_TILE_ROUND_DOWN ? 1 : 2, 1};
    const int     levels  = mode == EXR_TILE_ONE_LEVEL
                                ? 1
                                : (rounding == EXR_TILE_ROUND_DOWN ? 3 : 4);
    exr_attr_box2i_t dw   = {{-offset, offset}, {5 - offset, 5 + offset}};

    EXRCORE_TEST_RVAL (
        exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, NULL));
    EXRCORE_TEST_RVAL (exr_add_part (f, "tiles", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        f, partidx, 6, 6, EXR_COMPRESSION_ZIP));
    EXRCORE_TEST_RVAL (exr_set_data_window (f, partidx, &dw));
    EXRCORE_TEST_RVAL (exr_add_channel (
        f, partidx, "U", EXR_PIXEL_UINT, EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
    EXRCORE_TEST_RVAL (
        exr_set_tile_descriptor (f, partidx, 2, 2, mode, rounding));
    EXRCORE_TEST_RVAL (exr_write_header (f));

    for (int ly = 0; ly < levels; ++ly)
    {
        for (int lx = 0; lx < levels; ++lx)
        {
            if (mode == EXR_TILE_MIPMAP_LEVELS && lx != ly) continue;
            const int width = sizes[lx], height = sizes[ly];
            for (int ty = 0; ty < (height + 1) / 2; ++ty)
            {
                for (int tx = 0; tx < (width + 1) / 2; ++tx)
                {
                    exr_chunk_info_t cinfo;
                    const int        w = width - tx * 2 > 1 ? 2 : 1;
                    const int        h = height - ty * 2 > 1 ? 2 : 1;
                    EXRCORE_TEST_RVAL (exr_write_tile_chunk_info (
                        f, partidx, tx, ty, lx, ly, &cinfo));
                    EXRCORE_TEST (cinfo.width == w);
                    EXRCORE_TEST (cinfo.height == h);
                    EXRCORE_TEST (
                        cinfo.unpacked_size == (uint64_t) (w * h * 4));
                }
            }
        }
    }
    EXRCORE_TEST_RVAL (exr_finish (&f));
    remove (fn.c_str ());
}

void
testWriteTiles (const std::string& tempdir)
{
    const exr_tile_level_mode_t modes[] = {
        EXR_TILE_ONE_LEVEL, EXR_TILE_MIPMAP_LEVELS, EXR_TILE_RIPMAP_LEVELS};
    const exr_tile_round_mode_t rounding[] = {
        EXR_TILE_ROUND_DOWN, EXR_TILE_ROUND_UP};
    const int offsets[] = {0, 10};
    for (auto mode: modes)
        for (auto round: rounding)
            for (int offset: offsets)
                testWriteTileChunkInfo (tempdir, mode, round, offset);

    exr_context_t             f, outf, testf;
    std::string               outfn = tempdir;
    std::string               fn    = ILM_IMF_TEST_IMAGEDIR;
    int                       partidx;
    int32_t                   partcnt, outpartcnt, levelsx, levelsy;
    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    fn += "v1.7.test.tiled.exr";
    EXRCORE_TEST_RVAL (exr_start_read (&f, fn.c_str (), &cinit));
    outfn += "v1.7.test.tiled.exr";
    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "test", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST (partidx == 0);

    EXRCORE_TEST_RVAL (exr_copy_unset_attributes (outf, 0, f, 0));

    exr_storage_t ps;
    EXRCORE_TEST_RVAL (exr_get_storage (outf, 0, &ps));
    EXRCORE_TEST (EXR_STORAGE_TILED == ps);

    levelsx = levelsy = -1;
    EXRCORE_TEST_RVAL (exr_get_tile_levels (outf, 0, &levelsx, &levelsy));
    EXRCORE_TEST (levelsx == 1);
    EXRCORE_TEST (levelsy == 1);

    EXRCORE_TEST_RVAL (exr_get_tile_sizes (outf, 0, 0, 0, &levelsx, &levelsy));
    EXRCORE_TEST (levelsx == 12);
    EXRCORE_TEST (levelsy == 24);

    EXRCORE_TEST_RVAL_FAIL (EXR_ERR_NOT_OPEN_WRITE, exr_write_header (f));
    EXRCORE_TEST_RVAL (exr_write_header (outf));

    EXRCORE_TEST_RVAL (exr_get_chunk_count (f, 0, &partcnt));
    EXRCORE_TEST_RVAL (exr_get_chunk_count (outf, 0, &outpartcnt));
    EXRCORE_TEST (partcnt == outpartcnt);

    exr_attr_box2i_t dw;
    int              ty, tx;
    void*            cmem     = NULL;
    size_t           cmemsize = 0;

    EXRCORE_TEST_RVAL (exr_get_data_window (outf, 0, &dw));
    ty = 0;
    for (int32_t y = dw.min.y; y <= dw.max.y; y += levelsy)
    {
        tx = 0;
        for (int32_t x = dw.min.x; x <= dw.max.x; x += levelsx)
        {
            exr_chunk_info_t cinfo;
            EXRCORE_TEST_RVAL (
                exr_read_tile_chunk_info (f, 0, tx, ty, 0, 0, &cinfo));
            if (cmemsize < cinfo.packed_size)
            {
                if (cmem) free (cmem);
                cmem = malloc (cinfo.packed_size);
                if (!cmem) throw std::runtime_error ("out of memory");
                cmemsize = cinfo.packed_size;
            }
            EXRCORE_TEST_RVAL (exr_read_chunk (f, 0, &cinfo, cmem));
            EXRCORE_TEST_RVAL (exr_write_tile_chunk (
                outf, 0, tx, ty, 0, 0, cmem, cinfo.packed_size));
            ++tx;
        }
        ++ty;
    }
    if (cmem) free (cmem);
    EXRCORE_TEST_RVAL (exr_finish (&f));
    EXRCORE_TEST_RVAL (exr_finish (&outf));

    EXRCORE_TEST_RVAL (exr_start_read (&testf, outfn.c_str (), &cinit));
    EXRCORE_TEST_RVAL (exr_get_tile_levels (testf, 0, &levelsx, &levelsy));
    EXRCORE_TEST (levelsx == 1);
    EXRCORE_TEST (levelsy == 1);

    EXRCORE_TEST_RVAL (exr_get_tile_sizes (testf, 0, 0, 0, &levelsx, &levelsy));
    EXRCORE_TEST (levelsx == 12);
    EXRCORE_TEST (levelsy == 24);
    EXRCORE_TEST_RVAL (exr_finish (&testf));

    remove (outfn.c_str ());
}

void
testWriteMultiPart (const std::string& tempdir)
{
    exr_context_t outf;
    std::string   outfn = tempdir + "testdeepscan.exr";
    int           partidx;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "debug", EXR_STORAGE_TILED, &partidx));
    EXRCORE_TEST (partidx == 1);
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 2);

    // the part table grows geometrically, so exercise several reallocations
    for (int p = 2; p < 100; ++p)
    {
        char name[32];
        snprintf (name, sizeof (name), "part%d", p);
        EXRCORE_TEST_RVAL (
            exr_add_part (outf, name, EXR_STORAGE_SCANLINE, &partidx));
        EXRCORE_TEST (partidx == p);
    }
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 100);
    for (int p = 2; p < 100; ++p)
    {
        char        name[32];
        const char* partname = NULL;
        snprintf (name, sizeof (name), "part%d", p);
        EXRCORE_TEST_RVAL (exr_get_name (outf, p, &partname));
        EXRCORE_TEST (partname && strcmp (partname, name) == 0);
    }

    exr_storage_t storage;
    EXRCORE_TEST_RVAL (exr_get_storage (outf, 0, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_SCANLINE);
    EXRCORE_TEST_RVAL (exr_get_storage (outf, 1, &storage));
    EXRCORE_TEST (storage == EXR_STORAGE_TILED);

    uint32_t verflags;
    EXRCORE_TEST_RVAL (
        exr_get_file_version_and_flags (outf, &verflags));
    EXRCORE_TEST (verflags == (2 | 0x00001000));

    EXRCORE_TEST_RVAL (exr_finish (&outf));
    remove (outfn.c_str ());
}

void
testStartWriteUTF8 (const std::string& tempdir)
{
    exr_context_t outf;
    // per google translate, image in Japanese
    std::string   outfn = tempdir + "画像.exr";
    int           partidx;

    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
    cinit.error_handler_fn          = &err_cb;
    cinit.zip_level = 3;
    cinit.flags |= EXR_CONTEXT_FLAG_WRITE_LEGACY_HEADER;

    exr_set_default_zip_compression_level (-1);

    EXRCORE_TEST_RVAL (exr_start_write (
        &outf, outfn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
    EXRCORE_TEST_RVAL (
        exr_add_part (outf, "beauty", EXR_STORAGE_SCANLINE, &partidx));
    EXRCORE_TEST (partidx == 0);
    EXRCORE_TEST_RVAL (exr_get_count (outf, &partidx));
    EXRCORE_TEST (partidx == 1);
    partidx = 0;

    int fw = 1;
    int fh = 1;
    exr_attr_box2i_t dataW = { {0, 0}, {0, 0} };

    EXRCORE_TEST_RVAL (
        exr_initialize_required_attr_simple (outf, partidx, fw, fh, EXR_COMPRESSION_NONE));
    EXRCORE_TEST_RVAL (exr_set_data_window (outf, partidx, &dataW));

    EXRCORE_TEST_RVAL (exr_add_channel (
        outf, partidx, "h", EXR_PIXEL_HALF, EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
    EXRCORE_TEST_RVAL (exr_write_header (outf));

    exr_chunk_info_t      cinfo;
    exr_encode_pipeline_t encoder;

    EXRCORE_TEST_RVAL (exr_write_scanline_chunk_info (outf, 0, 0, &cinfo));
    EXRCORE_TEST_RVAL (
        exr_encoding_initialize (outf, 0, &cinfo, &encoder));

    uint16_t hval[] = { 0x1234, 0 };
    for (int c = 0; c < encoder.channel_count; ++c)
    {
        encoder.channels[c].encode_from_ptr   = (const uint8_t *)hval;
        encoder.channels[c].user_pixel_stride = 2;
        encoder.channels[c].user_line_stride  = 2;
    }
    EXRCORE_TEST_RVAL (
        exr_encoding_choose_default_routines (outf, 0, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_run (outf, 0, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_destroy (outf, &encoder));

    EXRCORE_TEST_RVAL (exr_finish (&outf));
#ifdef _WIN32
    int      wcSize = 0, fnlen = 0;
    wchar_t* wcFn = NULL;

    fnlen  = (int) strlen (outfn.c_str ());
    wcSize = MultiByteToWideChar (CP_UTF8, 0, outfn.c_str (), fnlen, NULL, 0);
    wcFn   = (wchar_t*) malloc (sizeof (wchar_t) * (wcSize + 1));
    if (wcFn)
    {
        MultiByteToWideChar (CP_UTF8, 0, outfn.c_str (), fnlen, wcFn, wcSize);
        wcFn[wcSize] = 0;
    }
    EXRCORE_TEST ( _waccess (wcFn, 0) != -1 );
    _wremove (wcFn);
    free (wcFn);
#else
    EXRCORE_TEST ( access (outfn.c_str (), F_OK) != -1 );
    remove (outfn.c_str ());
#endif
}

//
// Deterministic multi-part output.
//
// Producers hand chunks over in arbitrary order (and across parts);
// with EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE the library must spool them
// and emit a fixed physical order at finish, keep the missing-chunk
// case distinguishable from a completed file, and leave the normal
// delivery-order behavior untouched when the flag is not set.
//

namespace
{

// Build a small scanline part: width x height, one FLOAT channel whose
// pixel value encodes (part, chunk index) so a round trip can tell
// chunks apart.
void
define_scan_part (
    exr_context_t f,
    int           part,
    int           width,
    int           height,
    int           lines_per_chunk)
{
    EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
        f, part, width, height, EXR_COMPRESSION_NONE));
    exr_attr_box2i_t dw = {{0, 0}, {width - 1, height - 1}};
    EXRCORE_TEST_RVAL (exr_set_data_window (f, part, &dw));
    EXRCORE_TEST_RVAL (exr_add_channel (
        f, part, "V", EXR_PIXEL_FLOAT, EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
    (void) lines_per_chunk;
}

// Encode and deliver one scanline chunk to the (possibly deterministic)
// context. Pixel values are (float)(part * 1000 + chunk) so every chunk
// has distinct, checkable contents.
void
deliver_scan_chunk (
    exr_context_t f,
    int           part,
    int           width,
    int           chunk,
    int           lines_per_chunk)
{
    int                y = chunk * lines_per_chunk;
    exr_chunk_info_t   cinfo;
    exr_encode_pipeline_t encoder = EXR_ENCODE_PIPELINE_INITIALIZER;

    EXRCORE_TEST_RVAL (exr_write_scanline_chunk_info (f, part, y, &cinfo));
    EXRCORE_TEST_RVAL (exr_encoding_initialize (f, part, &cinfo, &encoder));
    EXRCORE_TEST (encoder.channel_count == 1);

    std::unique_ptr<float[]> pixels (
        new float[(size_t) width * (size_t) cinfo.height]);
    for (int32_t i = 0; i < width * cinfo.height; ++i)
        pixels[i] = (float) (part * 1000 + chunk);

    encoder.channels[0].encode_from_ptr   = (const uint8_t*) pixels.get ();
    encoder.channels[0].user_pixel_stride = (int) sizeof (float);
    encoder.channels[0].user_line_stride  = width * (int) sizeof (float);

    EXRCORE_TEST_RVAL (
        exr_encoding_choose_default_routines (f, part, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_run (f, part, &encoder));
    EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
}

// Read back one scanline chunk and confirm every pixel matches the
// (part, chunk) marker it was written with, i.e. chunk index still maps
// to the same channel data after the reordered write.
void
check_scan_chunk (
    exr_const_context_t f,
    int                 part,
    int                 width,
    int                 chunk,
    int                 lines_per_chunk)
{
    int                y = chunk * lines_per_chunk;
    exr_chunk_info_t   cinfo;
    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;

    EXRCORE_TEST_RVAL (exr_read_scanline_chunk_info (f, part, y, &cinfo));
    EXRCORE_TEST_RVAL (exr_decoding_initialize (f, part, &cinfo, &decoder));
    EXRCORE_TEST (decoder.channel_count == 1);

    std::unique_ptr<float[]> pixels (
        new float[(size_t) width * (size_t) cinfo.height]);
    decoder.channels[0].decode_to_ptr     = (uint8_t*) pixels.get ();
    decoder.channels[0].user_pixel_stride = (int) sizeof (float);
    decoder.channels[0].user_line_stride  = width * (int) sizeof (float);

    EXRCORE_TEST_RVAL (
        exr_decoding_choose_default_routines (f, part, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_run (f, part, &decoder));
    EXRCORE_TEST_RVAL (exr_decoding_destroy (f, &decoder));

    const float expected = (float) (part * 1000 + chunk);
    for (int32_t i = 0; i < width * cinfo.height; ++i)
        EXRCORE_TEST (pixels[i] == expected);
}

} // namespace

void
testWriteDeterministic (const std::string& tempdir)
{
    const int width           = 4;
    const int height          = 4;
    const int lines_per_chunk = 1;
    const int nchunks         = height / lines_per_chunk;
    const int nparts          = 3;

    /* ---- 1. deterministic write, chunks delivered out of order and
     *        interleaved across parts ---- */
    {
        std::string               fn = tempdir + "deterministic.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;
        cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

        int isdet = -1;
        EXRCORE_TEST_RVAL (exr_is_deterministic_write (f, &isdet));
        EXRCORE_TEST (isdet == 1);

        int pidx = -1;
        for (int p = 0; p < nparts; ++p)
        {
            char name[16];
            snprintf (name, sizeof (name), "layer%d", p);
            EXRCORE_TEST_RVAL (
                exr_add_part (f, name, EXR_STORAGE_SCANLINE, &pidx));
            EXRCORE_TEST (pidx == p);
            define_scan_part (f, p, width, height, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_write_header (f));

        int32_t chunk_count = -1;
        EXRCORE_TEST_RVAL (exr_get_chunk_count (f, 0, &chunk_count));
        EXRCORE_TEST (chunk_count == nchunks);

        /* nothing produced yet */
        for (int p = 0; p < nparts; ++p)
        {
            int32_t written = -1;
            EXRCORE_TEST_RVAL (
                exr_get_written_chunk_count (f, p, &written));
            EXRCORE_TEST (written == 0);
        }

        /* deliberately scrambled, interleaved delivery order */
        const int order[][2] = {
            {2, 3}, {0, 2}, {1, 0}, {2, 0}, {0, 3}, {1, 2},
            {0, 0}, {2, 2}, {1, 3}, {2, 1}, {0, 1}, {1, 1}};
        for (const auto& req: order)
        {
            deliver_scan_chunk (
                f, req[0], width, req[1], lines_per_chunk);
            int32_t written = -1;
            EXRCORE_TEST_RVAL (
                exr_get_written_chunk_count (f, req[0], &written));
            EXRCORE_TEST (written > 0);
        }

        for (int p = 0; p < nparts; ++p)
        {
            int32_t written = -1;
            EXRCORE_TEST_RVAL (
                exr_get_written_chunk_count (f, p, &written));
            EXRCORE_TEST (written == nchunks);
        }

        EXRCORE_TEST_RVAL (exr_finish (&f));

        /* ---- physical layout must be reproducible: parts and chunks
         *      appear in ascending index order on disk ---- */
        exr_context_t             rf;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (
            exr_start_read (&rf, fn.c_str (), &rinit));

        int rparts = -1;
        EXRCORE_TEST_RVAL (exr_get_count (rf, &rparts));
        EXRCORE_TEST (rparts == nparts);

        uint64_t prev_end = 0;
        for (int p = 0; p < nparts; ++p)
        {
            int32_t   cnt = -1;
            uint64_t* table = NULL;
            EXRCORE_TEST_RVAL (
                exr_get_chunk_table (rf, p, &table, &cnt));
            EXRCORE_TEST (cnt == nchunks);

            /* chunk table itself is in ascending chunk-index order */
            for (int c = 0; c < cnt; ++c)
            {
                EXRCORE_TEST (table[c] != 0);
                if (c > 0) EXRCORE_TEST (table[c] > table[c - 1]);
            }
            /* parts are contiguous and ordered: this part starts after
             * the previous one's data */
            if (p > 0) EXRCORE_TEST (table[0] >= prev_end);
            prev_end = table[cnt - 1];

            EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, p));

            /* index still addresses the channel data written for it */
            for (int c = 0; c < cnt; ++c)
                check_scan_chunk (rf, p, width, c, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_finish (&rf));
        remove (fn.c_str ());
    }

    /* ---- 2. same inputs through both modes must agree on layout ---- */
    {
        std::string fn_det = tempdir + "mode_det.exr";
        std::string fn_ref = tempdir + "mode_ref.exr";

        for (int deterministic = 0; deterministic <= 1; ++deterministic)
        {
            const std::string&      fn = deterministic ? fn_det : fn_ref;
            exr_context_t           f;
            exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
            cinit.error_handler_fn          = &err_cb;
            if (deterministic)
                cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

            EXRCORE_TEST_RVAL (exr_start_write (
                &f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));

            int pidx = -1;
            EXRCORE_TEST_RVAL (
                exr_add_part (f, "a", EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, 0, width, height, lines_per_chunk);
            EXRCORE_TEST_RVAL (
                exr_add_part (f, "b", EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, 1, width, height, lines_per_chunk);
            EXRCORE_TEST_RVAL (exr_write_header (f));

            if (deterministic)
            {
                /* reversed delivery, still must come out canonical */
                for (int p = 1; p >= 0; --p)
                    for (int c = nchunks - 1; c >= 0; --c)
                        deliver_scan_chunk (f, p, width, c, lines_per_chunk);
            }
            else
            {
                for (int p = 0; p < 2; ++p)
                    for (int c = 0; c < nchunks; ++c)
                        deliver_scan_chunk (f, p, width, c, lines_per_chunk);
            }
            EXRCORE_TEST_RVAL (exr_finish (&f));
        }

        /* the chunk offset tables of the two files must be identical */
        exr_context_t             f1, f2;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (exr_start_read (&f1, fn_det.c_str (), &rinit));
        EXRCORE_TEST_RVAL (exr_start_read (&f2, fn_ref.c_str (), &rinit));
        for (int p = 0; p < 2; ++p)
        {
            int32_t   c1 = 0, c2 = 0;
            uint64_t *t1 = NULL, *t2 = NULL;
            EXRCORE_TEST_RVAL (exr_get_chunk_table (f1, p, &t1, &c1));
            EXRCORE_TEST_RVAL (exr_get_chunk_table (f2, p, &t2, &c2));
            EXRCORE_TEST (c1 == c2);
            for (int c = 0; c < c1; ++c)
                EXRCORE_TEST (t1[c] == t2[c]);
        }
        EXRCORE_TEST_RVAL (exr_finish (&f1));
        EXRCORE_TEST_RVAL (exr_finish (&f2));
        remove (fn_det.c_str ());
        remove (fn_ref.c_str ());
    }

    /* ---- 3. missing chunks: finish reports partial material but
     *        keeps the file, and the produced chunks are readable ---- */
    {
        std::string               fn = tempdir + "deterministic_partial.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;
        cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
        int pidx = -1;
        EXRCORE_TEST_RVAL (
            exr_add_part (f, "only", EXR_STORAGE_SCANLINE, &pidx));
        define_scan_part (f, 0, width, height, lines_per_chunk);
        EXRCORE_TEST_RVAL (exr_write_header (f));

        /* deliver every chunk except the last one */
        for (int c = 0; c < nchunks - 1; ++c)
            deliver_scan_chunk (f, 0, width, c, lines_per_chunk);

        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE, exr_finish (&f));
#ifndef _WIN32
        /* the partial material is retained rather than deleted */
        EXRCORE_TEST (access (fn.c_str (), F_OK) != -1);
#endif

        exr_context_t             rf;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (exr_start_read (&rf, fn.c_str (), &rinit));

        /* the table as a whole is incomplete ... */
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE,
            exr_validate_chunk_table (rf, 0));

        int32_t   cnt = -1;
        uint64_t* table = NULL;
        EXRCORE_TEST_RVAL (exr_get_chunk_table (rf, 0, &table, &cnt));
        EXRCORE_TEST (cnt == nchunks);
        /* ... the missing chunk has a zero offset, the rest valid ... */
        for (int c = 0; c < cnt; ++c)
            EXRCORE_TEST ((c < cnt - 1) == (table[c] != 0));

        /* ... and the chunks that did arrive keep their indexed data */
        for (int c = 0; c < cnt - 1; ++c)
            check_scan_chunk (rf, 0, width, c, lines_per_chunk);

        EXRCORE_TEST_RVAL (exr_finish (&rf));
        remove (fn.c_str ());
    }

    /* ---- 3b. missing chunk in a middle part of a multi-part file:
     *         later parts must still land in their own chunk space ---- */
    {
        std::string               fn = tempdir + "deterministic_mpartial.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;
        cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
        for (int p = 0; p < nparts; ++p)
        {
            char name[16];
            snprintf (name, sizeof (name), "m%d", p);
            int pidx = -1;
            EXRCORE_TEST_RVAL (
                exr_add_part (f, name, EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, p, width, height, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_write_header (f));

        for (int p = 0; p < nparts; ++p)
            for (int c = 0; c < nchunks; ++c)
                /* part 1 loses its middle chunk */
                if (!(p == 1 && c == 1))
                    deliver_scan_chunk (f, p, width, c, lines_per_chunk);

        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE, exr_finish (&f));

        exr_context_t             rf;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (exr_start_read (&rf, fn.c_str (), &rinit));

        /* complete parts validate, the gappy one does not */
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, 0));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE,
            exr_validate_chunk_table (rf, 1));
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, 2));

        /* each part's chunk table must occupy its own, contiguous range
         * of the file and the surviving entries must be strictly
         * increasing: this fails if the gappy part completed early and
         * absorbed the next part's chunks */
        uint64_t prev_part_end = 0;
        for (int p = 0; p < nparts; ++p)
        {
            int32_t   cnt = -1;
            uint64_t* table = NULL;
            EXRCORE_TEST_RVAL (
                exr_get_chunk_table (rf, p, &table, &cnt));
            EXRCORE_TEST (cnt == nchunks);
            uint64_t last = 0;
            for (int c = 0; c < cnt; ++c)
            {
                if (table[c] != 0)
                {
                    EXRCORE_TEST (table[c] > last);
                    if (p > 0) EXRCORE_TEST (table[c] >= prev_part_end);
                    last = table[c];
                }
            }
            /* every part produced at least one chunk here */
            EXRCORE_TEST (last != 0);
            prev_part_end = last;
        }

        /* all surviving chunks keep their indexed data, including the
         * part after the gappy one */
        for (int p = 0; p < nparts; ++p)
            for (int c = 0; c < nchunks; ++c)
                if (!(p == 1 && c == 1))
                    check_scan_chunk (rf, p, width, c, lines_per_chunk);

        EXRCORE_TEST_RVAL (exr_finish (&rf));
        remove (fn.c_str ());
    }

    /* ---- 3c. an entire middle part missing: its table is all zero and
     *         the surrounding parts still keep their own chunk ranges ---- */
    {
        std::string               fn = tempdir + "deterministic_emptypart.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;
        cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
        for (int p = 0; p < nparts; ++p)
        {
            char name[16];
            snprintf (name, sizeof (name), "e%d", p);
            int pidx = -1;
            EXRCORE_TEST_RVAL (
                exr_add_part (f, name, EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, p, width, height, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_write_header (f));

        /* part 1 produces nothing at all */
        for (int p = 0; p < nparts; ++p)
            if (p != 1)
                for (int c = 0; c < nchunks; ++c)
                    deliver_scan_chunk (f, p, width, c, lines_per_chunk);

        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE, exr_finish (&f));

        exr_context_t             rf;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (exr_start_read (&rf, fn.c_str (), &rinit));

        int32_t   cnt = -1;
        uint64_t* table = NULL;
        EXRCORE_TEST_RVAL (exr_get_chunk_table (rf, 1, &table, &cnt));
        EXRCORE_TEST (cnt == nchunks);
        for (int c = 0; c < cnt; ++c) EXRCORE_TEST (table[c] == 0);

        EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, 0));
        EXRCORE_TEST_RVAL_FAIL (
            EXR_ERR_INCOMPLETE_CHUNK_TABLE,
            exr_validate_chunk_table (rf, 1));
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, 2));

        for (int c = 0; c < nchunks; ++c)
        {
            check_scan_chunk (rf, 0, width, c, lines_per_chunk);
            check_scan_chunk (rf, 2, width, c, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_finish (&rf));
        remove (fn.c_str ());
    }

    /* ---- 4. tiled parts must be canonicalized through the same path ---- */
    {
        std::string               fn = tempdir + "deterministic_tiled.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;
        cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

        const int tw = 2, th = 2;
        const int ntx = (width + tw - 1) / tw;
        const int nty = (height + th - 1) / th;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
        int pidx = -1;
        EXRCORE_TEST_RVAL (
            exr_add_part (f, "tiles", EXR_STORAGE_TILED, &pidx));
        EXRCORE_TEST_RVAL (exr_initialize_required_attr_simple (
            f, 0, width, height, EXR_COMPRESSION_NONE));
        exr_attr_box2i_t dw = {{0, 0}, {width - 1, height - 1}};
        EXRCORE_TEST_RVAL (exr_set_data_window (f, 0, &dw));
        EXRCORE_TEST_RVAL (exr_add_channel (
            f, 0, "V", EXR_PIXEL_FLOAT, EXR_PERCEPTUALLY_LOGARITHMIC, 1, 1));
        EXRCORE_TEST_RVAL (exr_set_tile_descriptor (
            f, 0, tw, th, EXR_TILE_ONE_LEVEL, EXR_TILE_ROUND_DOWN));
        EXRCORE_TEST_RVAL (exr_write_header (f));

        int32_t tchunks = -1;
        EXRCORE_TEST_RVAL (exr_get_chunk_count (f, 0, &tchunks));
        EXRCORE_TEST (tchunks == ntx * nty);

        /* deliver tiles in reverse y/x order */
        for (int ty = nty - 1; ty >= 0; --ty)
        {
            for (int tx = ntx - 1; tx >= 0; --tx)
            {
                exr_chunk_info_t      cinfo;
                exr_encode_pipeline_t encoder =
                    EXR_ENCODE_PIPELINE_INITIALIZER;
                EXRCORE_TEST_RVAL (exr_write_tile_chunk_info (
                    f, 0, tx, ty, 0, 0, &cinfo));
                EXRCORE_TEST_RVAL (
                    exr_encoding_initialize (f, 0, &cinfo, &encoder));

                std::unique_ptr<float[]> pixels (
                    new float[(size_t) cinfo.width * cinfo.height]);
                for (int32_t i = 0; i < cinfo.width * cinfo.height; ++i)
                    pixels[i] = (float) (ty * ntx + tx);

                encoder.channels[0].encode_from_ptr =
                    (const uint8_t*) pixels.get ();
                encoder.channels[0].user_pixel_stride =
                    (int) sizeof (float);
                encoder.channels[0].user_line_stride =
                    cinfo.width * (int) sizeof (float);
                EXRCORE_TEST_RVAL (
                    exr_encoding_choose_default_routines (f, 0, &encoder));
                EXRCORE_TEST_RVAL (exr_encoding_run (f, 0, &encoder));
                EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
            }
        }
        EXRCORE_TEST_RVAL (exr_finish (&f));

        exr_context_t             rf;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (exr_start_read (&rf, fn.c_str (), &rinit));
        EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, 0));

        int32_t   cnt = -1;
        uint64_t* table = NULL;
        EXRCORE_TEST_RVAL (exr_get_chunk_table (rf, 0, &table, &cnt));
        EXRCORE_TEST (cnt == tchunks);
        for (int c = 1; c < cnt; ++c)
            EXRCORE_TEST (table[c] > table[c - 1]);

        /* indexed tile reads still map to the right coordinates/data */
        for (int ty = 0; ty < nty; ++ty)
            for (int tx = 0; tx < ntx; ++tx)
            {
                exr_chunk_info_t      cinfo;
                exr_decode_pipeline_t decoder =
                    EXR_DECODE_PIPELINE_INITIALIZER;
                EXRCORE_TEST_RVAL (exr_read_tile_chunk_info (
                    rf, 0, tx, ty, 0, 0, &cinfo));
                EXRCORE_TEST_RVAL (
                    exr_decoding_initialize (rf, 0, &cinfo, &decoder));
                std::unique_ptr<float[]> pixels (
                    new float[(size_t) cinfo.width * cinfo.height]);
                decoder.channels[0].decode_to_ptr =
                    (uint8_t*) pixels.get ();
                decoder.channels[0].user_pixel_stride =
                    (int) sizeof (float);
                decoder.channels[0].user_line_stride =
                    cinfo.width * (int) sizeof (float);
                EXRCORE_TEST_RVAL (
                    exr_decoding_choose_default_routines (rf, 0, &decoder));
                EXRCORE_TEST_RVAL (exr_decoding_run (rf, 0, &decoder));
                EXRCORE_TEST_RVAL (exr_decoding_destroy (rf, &decoder));
                const float expected = (float) (ty * ntx + tx);
                for (int32_t i = 0; i < cinfo.width * cinfo.height; ++i)
                    EXRCORE_TEST (pixels[i] == expected);
            }
        EXRCORE_TEST_RVAL (exr_finish (&rf));
        remove (fn.c_str ());
    }

    /* ---- 5. concurrent producers deliver arbitrary parts/chunks from
     *        several threads; the spool + context lock must serialize
     *        them and the output stay canonical ---- */
    {
        std::string               fn = tempdir + "deterministic_threads.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;
        cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
        for (int p = 0; p < nparts; ++p)
        {
            char name[16];
            snprintf (name, sizeof (name), "t%d", p);
            int pidx = -1;
            EXRCORE_TEST_RVAL (
                exr_add_part (f, name, EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, p, width, height, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_write_header (f));

        /* every (part, chunk) job is handed to a worker thread */
        std::vector<std::thread> workers;
        for (int p = nparts - 1; p >= 0; --p)
            for (int c = nchunks - 1; c >= 0; --c)
                workers.emplace_back ([f, p, c] () {
                    deliver_scan_chunk (f, p, width, c, lines_per_chunk);
                });
        for (auto& t: workers) t.join ();

        for (int p = 0; p < nparts; ++p)
        {
            int32_t written = -1;
            EXRCORE_TEST_RVAL (
                exr_get_written_chunk_count (f, p, &written));
            EXRCORE_TEST (written == nchunks);
        }
        EXRCORE_TEST_RVAL (exr_finish (&f));

        exr_context_t             rf;
        exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        rinit.error_handler_fn          = &err_cb;
        EXRCORE_TEST_RVAL (exr_start_read (&rf, fn.c_str (), &rinit));
        for (int p = 0; p < nparts; ++p)
        {
            EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, p));
            for (int c = 0; c < nchunks; ++c)
                check_scan_chunk (rf, p, width, c, lines_per_chunk);
        }
        EXRCORE_TEST_RVAL (exr_finish (&rf));
        remove (fn.c_str ());
    }

    /* ---- 6. temporary-file mode: success renames into place, a
     *        missing chunk leaves the partial material behind ---- */
    {
        std::string fn = tempdir + "deterministic_tmp.exr";

        /* complete write: final name must appear, temp name gone */
        {
            exr_context_t             f;
            exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
            cinit.error_handler_fn          = &err_cb;
            cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

            EXRCORE_TEST_RVAL (exr_start_write (
                &f,
                fn.c_str (),
                EXR_INTERMEDIATE_TEMP_FILE,
                &cinit));
            int pidx = -1;
            EXRCORE_TEST_RVAL (
                exr_add_part (f, "only", EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, 0, width, height, lines_per_chunk);
            EXRCORE_TEST_RVAL (exr_write_header (f));
            for (int c = 0; c < nchunks; ++c)
                deliver_scan_chunk (f, 0, width, c, lines_per_chunk);
            EXRCORE_TEST_RVAL (exr_finish (&f));
#ifndef _WIN32
            EXRCORE_TEST (access (fn.c_str (), F_OK) != -1);
#endif
            exr_context_t             rf;
            exr_context_initializer_t rinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
            rinit.error_handler_fn          = &err_cb;
            EXRCORE_TEST_RVAL (exr_start_read (&rf, fn.c_str (), &rinit));
            EXRCORE_TEST_RVAL (exr_validate_chunk_table (rf, 0));
            EXRCORE_TEST_RVAL (exr_finish (&rf));
            remove (fn.c_str ());
        }

        /* incomplete write: finish reports the partial status and the
         * (temporary) material is retained for inspection rather than
         * deleted, while the final name is never produced */
        {
            exr_context_t             f;
            exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
            cinit.error_handler_fn          = &err_cb;
            cinit.flags |= EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE;

            EXRCORE_TEST_RVAL (exr_start_write (
                &f,
                fn.c_str (),
                EXR_INTERMEDIATE_TEMP_FILE,
                &cinit));
            const char* actualname = NULL;
            EXRCORE_TEST_RVAL (exr_get_file_name (f, &actualname));
            int pidx = -1;
            EXRCORE_TEST_RVAL (
                exr_add_part (f, "only", EXR_STORAGE_SCANLINE, &pidx));
            define_scan_part (f, 0, width, height, lines_per_chunk);
            EXRCORE_TEST_RVAL (exr_write_header (f));
            for (int c = 0; c < nchunks - 1; ++c)
                deliver_scan_chunk (f, 0, width, c, lines_per_chunk);
            EXRCORE_TEST_RVAL_FAIL (
                EXR_ERR_INCOMPLETE_CHUNK_TABLE, exr_finish (&f));
#ifndef _WIN32
            /* final destination must not have been renamed into place */
            EXRCORE_TEST (access (fn.c_str (), F_OK) == -1);
            /* but the partial material still exists on disk somewhere */
            glob_t globbed;
            std::string pattern = tempdir + "tmp.*deterministic_tmp.exr";
            int       grv       = glob (pattern.c_str (), 0, NULL, &globbed);
            EXRCORE_TEST (grv == 0);
            EXRCORE_TEST (globbed.gl_pathc >= 1);
            for (size_t i = 0; i < globbed.gl_pathc; ++i)
                remove (globbed.gl_pathv[i]);
            globfree (&globbed);
#else
            (void) actualname;
#endif
        }
    }

    /* ---- 7. normal mode keeps the original strict delivery rules ---- */
    {
        std::string               fn = tempdir + "normal_strict.exr";
        exr_context_t             f;
        exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
        cinit.error_handler_fn          = &err_cb;

        EXRCORE_TEST_RVAL (
            exr_start_write (&f, fn.c_str (), EXR_WRITE_FILE_DIRECTLY, &cinit));
        int isdet = 1;
        EXRCORE_TEST_RVAL (exr_is_deterministic_write (f, &isdet));
        EXRCORE_TEST (isdet == 0);

        int pidx = -1;
        EXRCORE_TEST_RVAL (
            exr_add_part (f, "a", EXR_STORAGE_SCANLINE, &pidx));
        define_scan_part (f, 0, width, height, lines_per_chunk);
        EXRCORE_TEST_RVAL (
            exr_add_part (f, "b", EXR_STORAGE_SCANLINE, &pidx));
        define_scan_part (f, 1, width, height, lines_per_chunk);
        EXRCORE_TEST_RVAL (exr_write_header (f));

        /* writing part 1 before part 0 is finished must still fail,
         * and skipping ahead within a part must still fail: the order
         * enforcement lives in the actual chunk write. */
        deliver_scan_chunk (f, 0, width, 0, lines_per_chunk);

        /* encode a chunk for part 1, but the write must be rejected */
        {
            exr_chunk_info_t     cinfo;
            exr_encode_pipeline_t encoder = EXR_ENCODE_PIPELINE_INITIALIZER;
            std::unique_ptr<float[]> pixels (new float[width]);
            for (int i = 0; i < width; ++i) pixels[i] = 1.0f;

            EXRCORE_TEST_RVAL (
                exr_write_scanline_chunk_info (f, 1, 0, &cinfo));
            EXRCORE_TEST_RVAL (
                exr_encoding_initialize (f, 1, &cinfo, &encoder));
            encoder.channels[0].encode_from_ptr =
                (const uint8_t*) pixels.get ();
            encoder.channels[0].user_pixel_stride = (int) sizeof (float);
            encoder.channels[0].user_line_stride =
                width * (int) sizeof (float);
            EXRCORE_TEST_RVAL (
                exr_encoding_choose_default_routines (f, 1, &encoder));
            EXRCORE_TEST_RVAL_FAIL (
                EXR_ERR_INCORRECT_PART,
                exr_encoding_run (f, 1, &encoder));
            EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
        }

        /* skipping chunk 1 of part 0 must be rejected as well */
        {
            exr_chunk_info_t     cinfo;
            exr_encode_pipeline_t encoder = EXR_ENCODE_PIPELINE_INITIALIZER;
            std::unique_ptr<float[]> pixels (new float[width]);
            for (int i = 0; i < width; ++i) pixels[i] = 2.0f;

            EXRCORE_TEST_RVAL (exr_write_scanline_chunk_info (
                f, 0, 2 * lines_per_chunk, &cinfo));
            EXRCORE_TEST_RVAL (
                exr_encoding_initialize (f, 0, &cinfo, &encoder));
            encoder.channels[0].encode_from_ptr =
                (const uint8_t*) pixels.get ();
            encoder.channels[0].user_pixel_stride = (int) sizeof (float);
            encoder.channels[0].user_line_stride =
                width * (int) sizeof (float);
            EXRCORE_TEST_RVAL (
                exr_encoding_choose_default_routines (f, 0, &encoder));
            EXRCORE_TEST_RVAL_FAIL (
                EXR_ERR_INCORRECT_CHUNK,
                exr_encoding_run (f, 0, &encoder));
            EXRCORE_TEST_RVAL (exr_encoding_destroy (f, &encoder));
        }

        exr_finish (&f);
        remove (fn.c_str ());
    }
}
