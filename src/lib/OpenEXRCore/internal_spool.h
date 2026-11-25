/*
** SPDX-License-Identifier: BSD-3-Clause
** Copyright Contributors to the OpenEXR Project.
*/

#ifndef OPENEXR_INTERNAL_SPOOL_H
#define OPENEXR_INTERNAL_SPOOL_H

#include "openexr_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deterministic write spool.
 *
 * When a write context is created with
 * EXR_CONTEXT_FLAG_DETERMINISTIC_WRITE, data chunks are not written to
 * the output stream as they arrive from the (possibly multi-threaded)
 * producers. Instead each completed chunk is copied into memory here,
 * keyed by its logical part and chunk index. When the context is
 * finished, the spool is flushed in the canonical physical order:
 * parts in ascending part index order, and the chunks of each part in
 * ascending chunk index order, which is the same order the chunk
 * offset table uses.
 *
 * All access happens while the context write lock is held, so the
 * spool itself needs no additional synchronization.
 */

typedef struct
{
    /*
     * One entry per logical chunk across the whole file
     * (sum of all part chunk counts), laid out part by part. An entry
     * with data == NULL has not been produced yet.
     */
    struct _exr_spool_entry* entries;

    /* First entry index in entries for each part. */
    size_t* part_offsets;

    /* Total number of entries (total number of chunks in the file). */
    size_t total_entries;
} exr_spool_t;

struct _exr_spool_entry
{
    /* Chunk coordinates, copied from the write request. */
    int32_t chunk_idx;
    int32_t start_x;
    int32_t start_y;
    uint8_t level_x;
    uint8_t level_y;
    uint8_t storage;

    /* Packed pixel data (always present once the entry is stored). */
    void*    data;
    uint64_t data_size;
    uint64_t unpacked_size;

    /* Deep sample count table (NULL / 0 for non-deep chunks). */
    void*    sample_data;
    uint64_t sample_data_size;
};

/* Allocate the spool and its entry table once the header is written
 * and the per-part chunk counts are known. */
exr_result_t internal_exr_spool_init (exr_context_t ctxt);

/* Store one completed chunk. Returns EXR_ERR_INVALID_ARGUMENT if the
 * chunk index was already stored (each chunk may only be written once). */
exr_result_t internal_exr_spool_store (
    exr_context_t          ctxt,
    int                    part_index,
    int                    chunk_idx,
    int                    start_x,
    int                    start_y,
    uint8_t                level_x,
    uint8_t                level_y,
    uint8_t                storage,
    const void*            data,
    uint64_t               data_size,
    uint64_t               unpacked_size,
    const void*            sample_data,
    uint64_t               sample_data_size);

/* Number of chunks currently held for the given part. */
exr_result_t internal_exr_spool_part_count (
    exr_const_context_t ctxt, int part_index, int32_t* out);

/*
 * Flush the spool to the output stream in canonical order and release
 * the spool memory.
 *
 * Chunks that were never produced are skipped: their chunk offset
 * table entries stay zero, producing the same on-disk layout as a
 * write that crashed mid-file (readable with chunk reconstruction,
 * reported incomplete by exr_validate_chunk_table()).
 *
 * Returns EXR_ERR_INCOMPLETE_CHUNK_TABLE when one or more chunks are
 * missing, EXR_ERR_SUCCESS when the whole file was produced, or the
 * first I/O error encountered.
 */
exr_result_t internal_exr_spool_flush (exr_context_t ctxt);

/* Free any spool memory (called when the context is destroyed). */
void internal_exr_spool_destroy (exr_context_t ctxt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENEXR_INTERNAL_SPOOL_H */
