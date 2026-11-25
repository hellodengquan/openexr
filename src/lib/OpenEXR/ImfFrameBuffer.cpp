//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

//-----------------------------------------------------------------------------
//
//      class Slice
//      class FrameBuffer
//
//-----------------------------------------------------------------------------

#include "Iex.h"
#include "ImfFrameBuffer.h"

#include <cstring>

using namespace std;

#include "ImfNamespace.h"

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_ENTER

Slice::Slice (
    PixelType t,
    char*     b,
    size_t    xst,
    size_t    yst,
    int       xsm,
    int       ysm,
    double    fv,
    bool      xtc,
    bool      ytc)
    : type (t)
    , base (b)
    , xStride (xst)
    , yStride (yst)
    , xSampling (xsm)
    , ySampling (ysm)
    , fillValue (fv)
    , xTileCoords (xtc)
    , yTileCoords (ytc)
{
    // empty
}

Slice
Slice::Make (
    PixelType                   type,
    const void*                 ptr,
    const IMATH_NAMESPACE::V2i& origin,
    int64_t                     w,
    int64_t                     h,
    size_t                      xStride,
    size_t                      yStride,
    int                         xSampling,
    int                         ySampling,
    double                      fillValue,
    bool                        xTileCoords,
    bool                        yTileCoords)
{
    intptr_t base = reinterpret_cast<intptr_t> (const_cast<void*> (ptr));
    if (xStride == 0)
    {
        switch (type)
        {
            case UINT: xStride = sizeof (uint32_t); break;
            case HALF: xStride = sizeof (uint16_t); break;
            case FLOAT: xStride = sizeof (float); break;
            case NUM_PIXELTYPES:
                THROW (IEX_NAMESPACE::ArgExc, "Invalid pixel type.");
        }
    }
    if (yStride == 0) yStride = static_cast<size_t> (w / xSampling) * xStride;

    // data window is an int, so force promote to higher type to avoid
    // overflow for off y (degenerate size checks should be in
    // ImfHeader::sanityCheck, but offset can be large-ish)
    int64_t offx =
        (static_cast<int64_t> (origin.x) / static_cast<int64_t> (xSampling));
    offx *= static_cast<int64_t> (xStride);

    int64_t offy =
        (static_cast<int64_t> (origin.y) / static_cast<int64_t> (ySampling));
    offy *= static_cast<int64_t> (yStride);

    return Slice (
        type,
        reinterpret_cast<char*> (base - offx - offy),
        xStride,
        yStride,
        xSampling,
        ySampling,
        fillValue,
        xTileCoords,
        yTileCoords);
}

Slice
Slice::Make (
    PixelType                     type,
    const void*                   ptr,
    const IMATH_NAMESPACE::Box2i& dataWindow,
    size_t                        xStride,
    size_t                        yStride,
    int                           xSampling,
    int                           ySampling,
    double                        fillValue,
    bool                          xTileCoords,
    bool                          yTileCoords)
{
    return Make (
        type,
        ptr,
        dataWindow.min,
        static_cast<int64_t> (dataWindow.max.x) -
            static_cast<int64_t> (dataWindow.min.x) + 1,
        static_cast<int64_t> (dataWindow.max.y) -
            static_cast<int64_t> (dataWindow.min.y) + 1,
        xStride,
        yStride,
        xSampling,
        ySampling,
        fillValue,
        xTileCoords,
        yTileCoords);
}

int64_t
floorSampleIndex (int64_t coordinate, int sampling)
{
    // sampling is expected to be positive; be defensive anyway.
    int64_t s = sampling;
    if (s <= 0) s = 1;

    int64_t q = coordinate / s;
    int64_t r = coordinate % s;

    // C++ truncates towards zero.  With a positive divisor a negative
    // remainder means the mathematical floor is one less.
    if (r < 0) --q;
    return q;
}

int64_t
ceilSampleIndex (int64_t coordinate, int sampling)
{
    int64_t s = sampling;
    if (s <= 0) s = 1;

    int64_t q = coordinate / s;
    int64_t r = coordinate % s;

    // With a positive divisor a positive remainder means the
    // mathematical ceiling is one more; a negative remainder already
    // makes truncation equal to the ceiling.
    if (r > 0) ++q;
    return q;
}

static size_t
sliceElementSize (PixelType type)
{
    switch (type)
    {
        case UINT: return sizeof (uint32_t);
        case HALF: return sizeof (uint16_t);
        case FLOAT: return sizeof (float);
        case NUM_PIXELTYPES:
        default:
            THROW (IEX_NAMESPACE::ArgExc, "Invalid pixel type.");
    }
}

Slice
Slice::MakeRegion (
    PixelType                     type,
    void*                         ptr,
    const IMATH_NAMESPACE::Box2i& region,
    size_t                        xStride,
    size_t                        yStride,
    int                           xSampling,
    int                           ySampling,
    double                        fillValue)
{
    if (xSampling < 1 || ySampling < 1)
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Slice sampling factors must be strictly positive.");
    }

    const int64_t width =
        static_cast<int64_t> (region.max.x) - region.min.x + 1;
    const int64_t height =
        static_cast<int64_t> (region.max.y) - region.min.y + 1;

    if (width <= 0 || height <= 0)
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Region passed to Slice::MakeRegion must be non-empty.");
    }

    if (xStride == 0) xStride = sliceElementSize (type);
    if (yStride == 0)
    {
        if (static_cast<int64_t> (xStride) > 0 &&
            width > INT64_MAX / static_cast<int64_t> (xStride))
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Row stride overflow while building region slice.");
        }
        int64_t tight = width * static_cast<int64_t> (xStride);
        if (tight <= 0)
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Row stride overflow while building region slice.");
        }
        yStride = static_cast<size_t> (tight);
    }

    // The Slice addressing contract is
    //   addr(x,y) = base + gridIndex(x) * slice.xStride
    //                   + gridIndex(y) * slice.yStride
    // where gridIndex is the sample-grid index.  To place samples on
    // a LOGICAL pixel grid inside the caller's region we therefore
    // scale the logical strides by the sampling factor...
    if (xStride > static_cast<size_t> (INT64_MAX) /
                      static_cast<size_t> (xSampling) ||
        yStride > static_cast<size_t> (INT64_MAX) /
                      static_cast<size_t> (ySampling))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Slice stride overflow while building region slice.");
    }

    const int64_t sliceXStride64 =
        static_cast<int64_t> (xStride) * static_cast<int64_t> (xSampling);
    const int64_t sliceYStride64 =
        static_cast<int64_t> (yStride) * static_cast<int64_t> (ySampling);

    // ... and offset base by the RAW region origin times the logical
    // strides (not divided by sampling).  For a sample site
    // x = k * xSampling this yields
    //   ptr + (x - region.min.x) * xStride
    // even when region.min.x is negative or not grid aligned.
    const int64_t originOffsetX =
        static_cast<int64_t> (region.min.x) * static_cast<int64_t> (xStride);
    const int64_t originOffsetY =
        static_cast<int64_t> (region.min.y) * static_cast<int64_t> (yStride);

    const intptr_t address = reinterpret_cast<intptr_t> (ptr);
    char*          base = reinterpret_cast<char*> (
        address - originOffsetX - originOffsetY);

    return Slice (
        type,
        base,
        static_cast<size_t> (sliceXStride64),
        static_cast<size_t> (sliceYStride64),
        xSampling,
        ySampling,
        fillValue);
}

IMATH_NAMESPACE::Box2i
copyRegionPixels (
    const FrameBuffer&            source,
    const FrameBuffer&            destination,
    const IMATH_NAMESPACE::Box2i& region,
    const IMATH_NAMESPACE::Box2i& sourceBounds)
{
    const int64_t clipMinX =
        max (static_cast<int64_t> (region.min.x),
             static_cast<int64_t> (sourceBounds.min.x));
    const int64_t clipMinY =
        max (static_cast<int64_t> (region.min.y),
             static_cast<int64_t> (sourceBounds.min.y));
    const int64_t clipMaxX =
        min (static_cast<int64_t> (region.max.x),
             static_cast<int64_t> (sourceBounds.max.x));
    const int64_t clipMaxY =
        min (static_cast<int64_t> (region.max.y),
             static_cast<int64_t> (sourceBounds.max.y));

    if (clipMinX > clipMaxX || clipMinY > clipMaxY)
        return IMATH_NAMESPACE::Box2i ();

    const IMATH_NAMESPACE::Box2i intersection (
        IMATH_NAMESPACE::V2i (
            static_cast<int> (clipMinX), static_cast<int> (clipMinY)),
        IMATH_NAMESPACE::V2i (
            static_cast<int> (clipMaxX), static_cast<int> (clipMaxY)));

    for (FrameBuffer::ConstIterator dit = destination.begin ();
         dit != destination.end ();
         ++dit)
    {
        const Slice&      dst = dit.slice ();
        const Slice*      src = source.findSlice (dit.name ());

        // A destination slice without a matching source has no data
        // to copy; leave its pre-existing pixels untouched.
        if (!src) continue;

        if (src->type != dst.type)
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Cannot copy region for slice \"" << dit.name ()
                                                  << "\": source and "
                                                     "destination pixel "
                                                     "types differ.");
        }

        const int sx = dst.xSampling > 0 ? dst.xSampling : 1;
        const int sy = dst.ySampling > 0 ? dst.ySampling : 1;

        if (src->xSampling != sx || src->ySampling != sy)
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Cannot copy region for slice \"" << dit.name ()
                                                  << "\": source and "
                                                     "destination sampling "
                                                     "factors differ.");
        }

        const size_t elementBytes = sliceElementSize (dst.type);

        // Inclusive range of grid indices whose sample site lies in
        // the clipped rectangle.  ceil/floor matter for negative
        // coordinates and odd rectangle origins.
        const int64_t kxFirst = ceilSampleIndex (clipMinX, sx);
        const int64_t kxLast  = floorSampleIndex (clipMaxX, sx);
        const int64_t kyFirst = ceilSampleIndex (clipMinY, sy);
        const int64_t kyLast  = floorSampleIndex (clipMaxY, sy);

        for (int64_t ky = kyFirst; ky <= kyLast; ++ky)
        {
            const int64_t srcRowOffset =
                ky * static_cast<int64_t> (src->yStride);
            const int64_t dstRowOffset =
                ky * static_cast<int64_t> (dst.yStride);

            char*       dstRow = dst.base + static_cast<ptrdiff_t> (dstRowOffset);
            const char* srcRow =
                src->base + static_cast<ptrdiff_t> (srcRowOffset);

            for (int64_t kx = kxFirst; kx <= kxLast; ++kx)
            {
                const int64_t srcColOffset =
                    kx * static_cast<int64_t> (src->xStride);
                const int64_t dstColOffset =
                    kx * static_cast<int64_t> (dst.xStride);

                memcpy (
                    dstRow + static_cast<ptrdiff_t> (dstColOffset),
                    srcRow + static_cast<ptrdiff_t> (srcColOffset),
                    elementBytes);
            }
        }
    }

    return intersection;
}

void
FrameBuffer::insert (const char name[], const Slice& slice)
{
    if (name[0] == 0)
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Frame buffer slice name cannot be an empty string.");
    }

    _map[name] = slice;
}

void
FrameBuffer::insert (const string& name, const Slice& slice)
{
    insert (name.c_str (), slice);
}

Slice&
FrameBuffer::operator[] (const char name[])
{
    SliceMap::iterator i = _map.find (name);

    if (i == _map.end ())
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Cannot find frame buffer slice \"" << name << "\".");
    }

    return i->second;
}

const Slice&
FrameBuffer::operator[] (const char name[]) const
{
    SliceMap::const_iterator i = _map.find (name);

    if (i == _map.end ())
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Cannot find frame buffer slice \"" << name << "\".");
    }

    return i->second;
}

Slice&
FrameBuffer::operator[] (const string& name)
{
    return this->operator[] (name.c_str ());
}

const Slice&
FrameBuffer::operator[] (const string& name) const
{
    return this->operator[] (name.c_str ());
}

Slice*
FrameBuffer::findSlice (const char name[])
{
    SliceMap::iterator i = _map.find (name);
    return (i == _map.end ()) ? 0 : &i->second;
}

const Slice*
FrameBuffer::findSlice (const char name[]) const
{
    SliceMap::const_iterator i = _map.find (name);
    return (i == _map.end ()) ? 0 : &i->second;
}

Slice*
FrameBuffer::findSlice (const string& name)
{
    return findSlice (name.c_str ());
}

const Slice*
FrameBuffer::findSlice (const string& name) const
{
    return findSlice (name.c_str ());
}

FrameBuffer::Iterator
FrameBuffer::begin ()
{
    return _map.begin ();
}

FrameBuffer::ConstIterator
FrameBuffer::begin () const
{
    return _map.begin ();
}

FrameBuffer::Iterator
FrameBuffer::end ()
{
    return _map.end ();
}

FrameBuffer::ConstIterator
FrameBuffer::end () const
{
    return _map.end ();
}

FrameBuffer::Iterator
FrameBuffer::find (const char name[])
{
    return _map.find (name);
}

FrameBuffer::ConstIterator
FrameBuffer::find (const char name[]) const
{
    return _map.find (name);
}

FrameBuffer::Iterator
FrameBuffer::find (const string& name)
{
    return find (name.c_str ());
}

FrameBuffer::ConstIterator
FrameBuffer::find (const string& name) const
{
    return find (name.c_str ());
}

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_EXIT
