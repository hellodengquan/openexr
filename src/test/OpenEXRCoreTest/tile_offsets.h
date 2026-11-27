// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenEXR Project.

#ifndef OPENEXR_CORE_TEST_TILE_OFFSETS_H
#define OPENEXR_CORE_TEST_TILE_OFFSETS_H

#include <string>

// Integration tests for tiled image chunk location handling.
//
// The EXR chunk offset table stores each chunk's location as a full
// 64-bit file offset, and a multipart file places later parts' pixel
// data well past the header and the preceding parts' data. These tests
// build small in-memory tiled images through the public Core API and
// then read them back over custom memory streams, so that:
//
//  * a valid chunk whose logical tile coordinates are large and whose
//    64-bit location is far from the file header delivers the pixels
//    written for that particular part / tile, and
//  * a chunk location that cannot be satisfied is reported as a failure
//    before any output pixel is delivered, with a distinction between
//    a physically truncated read position and a chunk that the file
//    simply does not contain.
void testReadTilesFarOffsets (const std::string& tempdir);
void testReadTilesTruncated (const std::string& tempdir);
void testReadTilesMissingChunk (const std::string& tempdir);

#endif // OPENEXR_CORE_TEST_TILE_OFFSETS_H
