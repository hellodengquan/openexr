/*
** SPDX-License-Identifier: BSD-3-Clause
** Copyright Contributors to the OpenEXR Project.
*/

#include "internal_spool.h"

#include "internal_structs.h"
#include "openexr_chunkio.h"

#include <string.h>

/* Forward declarations of the part completion helper in chunk.c. */
exr_result_t internal_exr_complete_output_part (
    exr_context_t ctxt, exr_priv_part_t part);

/**************************************/

static void*
spool_copy (exr_context_t ctxt, const void* src, uint64_t size)
{
    void* dst;

    if (size == 0) return NULL;

    dst = ctxt->alloc_fn ((size_t) size);
    if (!dst) return NULL;

    memcpy (dst, src, (size_t) size);
    return dst;
}

/**************************************/

exr_result_t
internal_exr_spool_init (exr_context_t ctxt)
{
    exr_spool_t* s       = &(ctxt->spool);
    uint64_t     total   = 0;
    size_t       off     = 0;
    size_t       nbytes;

    for (int p = 0; p < ctxt->num_parts; ++p)
        total += (uint64_t) ctxt->parts[p]->chunk_count;

    if (total > (uint64_t) SIZE_MAX)
        return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);

    s->total_entries = (size_t) total;

    nbytes = s->total_entries * sizeof (struct _exr_spool_entry);
    if (nbytes > 0)
    {
        s->entries = (struct _exr_spool_entry*) ctxt->alloc_fn (nbytes);
        if (!s->entries)
            return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
        memset (s->entries, 0, nbytes);
    }

    nbytes = (size_t) ctxt->num_parts * sizeof (size_t);
    if (nbytes > 0)
    {
        s->part_offsets = (size_t*) ctxt->alloc_fn (nbytes);
        if (!s->part_offsets)
        {
            ctxt->free_fn (s->entries);
            s->entries = NULL;
            return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
        }
        for (int p = 0; p < ctxt->num_parts; ++p)
        {
            s->part_offsets[p] = off;
            off += (size_t) ctxt->parts[p]->chunk_count;
        }
    }

    return EXR_ERR_SUCCESS;
}

/**************************************/

exr_result_t
internal_exr_spool_store (
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
    uint64_t               sample_data_size)
{
    exr_spool_t*             s = &(ctxt->spool);
    struct _exr_spool_entry* e;

    if (!s->entries || part_index < 0 || part_index >= ctxt->num_parts ||
        chunk_idx < 0 ||
        chunk_idx >= ctxt->parts[part_index]->chunk_count)
        return ctxt->standard_error (ctxt, EXR_ERR_INVALID_ARGUMENT);

    e = &(s->entries[s->part_offsets[part_index] + (size_t) chunk_idx]);

    if (e->data || e->sample_data)
        return ctxt->print_error (
            ctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Deterministic output: chunk %d of part %d was already written",
            chunk_idx,
            part_index);

    if (data_size > 0)
    {
        e->data = spool_copy (ctxt, data, data_size);
        if (!e->data)
            return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
    }
    e->data_size = data_size;

    if (sample_data_size > 0)
    {
        e->sample_data = spool_copy (ctxt, sample_data, sample_data_size);
        if (!e->sample_data)
        {
            ctxt->free_fn (e->data);
            e->data      = NULL;
            e->data_size = 0;
            return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
        }
    }
    e->sample_data_size = sample_data_size;
    e->unpacked_size    = unpacked_size;

    e->chunk_idx = chunk_idx;
    e->start_x   = start_x;
    e->start_y   = start_y;
    e->level_x   = level_x;
    e->level_y   = level_y;
    e->storage   = storage;

    return EXR_ERR_SUCCESS;
}

/**************************************/

exr_result_t
internal_exr_spool_part_count (
    exr_const_context_t ctxt, int part_index, int32_t* out)
{
    exr_spool_t*    s = EXR_CONST_CAST (exr_spool_t*, &(ctxt->spool));
    exr_priv_part_t part;
    int32_t         count = 0;

    if (!out) return ctxt->standard_error (ctxt, EXR_ERR_INVALID_ARGUMENT);
    if (part_index < 0 || part_index >= ctxt->num_parts)
        return ctxt->standard_error (ctxt, EXR_ERR_ARGUMENT_OUT_OF_RANGE);

    part = ctxt->parts[part_index];
    if (s->entries)
    {
        size_t base = s->part_offsets[part_index];
        for (int ci = 0; ci < part->chunk_count; ++ci)
        {
            if (s->entries[base + (size_t) ci].data ||
                s->entries[base + (size_t) ci].sample_data)
                ++count;
        }
    }
    *out = count;
    return EXR_ERR_SUCCESS;
}

/**************************************/

static exr_result_t
flush_entry (
    exr_context_t ctxt, int part_index, struct _exr_spool_entry* e)
{
    switch ((exr_storage_t) e->storage)
    {
        case EXR_STORAGE_SCANLINE:
            return exr_write_scanline_chunk (
                ctxt,
                part_index,
                e->start_y,
                e->data,
                e->data_size);
        case EXR_STORAGE_TILED:
            return exr_write_tile_chunk (
                ctxt,
                part_index,
                e->start_x,
                e->start_y,
                e->level_x,
                e->level_y,
                e->data,
                e->data_size);
        case EXR_STORAGE_DEEP_SCANLINE:
            return exr_write_deep_scanline_chunk (
                ctxt,
                part_index,
                e->start_y,
                e->data,
                e->data_size,
                e->unpacked_size,
                e->sample_data,
                e->sample_data_size);
        case EXR_STORAGE_DEEP_TILED:
            return exr_write_deep_tile_chunk (
                ctxt,
                part_index,
                e->start_x,
                e->start_y,
                e->level_x,
                e->level_y,
                e->data,
                e->data_size,
                e->unpacked_size,
                e->sample_data,
                e->sample_data_size);
        case EXR_STORAGE_UNKNOWN:
        case EXR_STORAGE_LAST_TYPE: break;
    }
    return ctxt->standard_error (ctxt, EXR_ERR_INVALID_ARGUMENT);
}

/**************************************/

exr_result_t
internal_exr_spool_flush (exr_context_t ctxt)
{
    exr_spool_t* s       = &(ctxt->spool);
    exr_result_t rv      = EXR_ERR_SUCCESS;
    int          missing = 0;

    /* A deterministic context only reaches finish with a spool once the
     * header (and thus spool_init) succeeded. No entries means the
     * header write failed: report it rather than silently finalizing an
     * empty file. */
    if (!s->entries)
        return ctxt->standard_error (ctxt, EXR_ERR_INVALID_ARGUMENT);

    /*
     * Finish runs on the thread tearing the context down, so the spool
     * needs no lock of its own. The flushing flag tells the regular
     * chunk write routines to skip the delivery-order checks and emit
     * whatever we hand them in canonical order.
     */
    ctxt->spool_flushing     = 1;
    ctxt->cur_output_part    = 0;
    ctxt->last_output_chunk  = -1;
    ctxt->output_chunk_count = 0;

    for (int p = 0; rv == EXR_ERR_SUCCESS && p < ctxt->num_parts; ++p)
    {
        exr_priv_part_t part = ctxt->parts[p];
        size_t          base = s->part_offsets[p];

        for (int ci = 0; ci < part->chunk_count; ++ci)
        {
            struct _exr_spool_entry* e = &(s->entries[base + (size_t) ci]);

            if (!e->data && !e->sample_data)
            {
                /* producer never delivered this chunk: leave its
                 * offset table entry at zero. */
                missing = 1;
                continue;
            }

            rv = flush_entry (ctxt, p, e);
            if (rv != EXR_ERR_SUCCESS) break;
        }

        /* A part whose chunks were all missing never triggered the
         * normal completion bookkeeping, so finalize it explicitly to
         * write its (all zero) offset table and advance the sequence. */
        if (rv == EXR_ERR_SUCCESS && ctxt->cur_output_part == p)
            rv = internal_exr_complete_output_part (ctxt, part);
    }

    ctxt->spool_flushing = 0;

    if (rv == EXR_ERR_SUCCESS && missing)
        rv = ctxt->report_error (
            ctxt,
            EXR_ERR_INCOMPLETE_CHUNK_TABLE,
            "Deterministic output: one or more chunks were never written, "
            "the resulting file is incomplete");

    internal_exr_spool_destroy (ctxt);
    return rv;
}

/**************************************/

void
internal_exr_spool_destroy (exr_context_t ctxt)
{
    exr_spool_t* s = &(ctxt->spool);

    if (s->entries)
    {
        for (size_t i = 0; i < s->total_entries; ++i)
        {
            struct _exr_spool_entry* e = &(s->entries[i]);
            if (e->data) ctxt->free_fn (e->data);
            if (e->sample_data) ctxt->free_fn (e->sample_data);
        }
        ctxt->free_fn (s->entries);
        s->entries       = NULL;
        s->total_entries = 0;
    }
    if (s->part_offsets)
    {
        ctxt->free_fn (s->part_offsets);
        s->part_offsets = NULL;
    }
}
