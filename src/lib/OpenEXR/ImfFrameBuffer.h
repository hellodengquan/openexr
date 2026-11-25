//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

#ifndef INCLUDED_IMF_FRAME_BUFFER_H
#define INCLUDED_IMF_FRAME_BUFFER_H

//-----------------------------------------------------------------------------
//
//      class Slice
//      class FrameBuffer
//
//-----------------------------------------------------------------------------

#include "ImfForward.h"

#include "ImfName.h"
#include "ImfPixelType.h"

#include <Imath/ImathBox.h>

#include <cstdint>
#include <map>
#include <string>

OPENEXR_IMF_INTERNAL_NAMESPACE_HEADER_ENTER

//-------------------------------------------------------
// Description of a single slice of the frame buffer:
//
// Note -- terminology: as part of a file, a component of
// an image (e.g. red, green, blue, depth etc.) is called
// a "channel".  As part of a frame buffer, an image
// component is called a "slice".
//-------------------------------------------------------

struct IMF_EXPORT_TYPE Slice
{
    //------------------------------
    // Data type; see ImfPixelType.h
    //------------------------------

    PixelType type;

    //---------------------------------------------------------------------
    // Memory layout:  The address of pixel (x, y) is
    //
    //  base + (xp / xSampling) * xStride + (yp / ySampling) * yStride
    //
    // where xp and yp are computed as follows:
    //
    //  * If we are reading or writing a scanline-based file:
    //
    //      xp = x
    //      yp = y
    //
    //  * If we are reading a tile whose upper left coorner is at (xt, yt):
    //
    //      if xTileCoords is true then xp = x - xt, else xp = x
    //      if yTileCoords is true then yp = y - yt, else yp = y
    //
    //---------------------------------------------------------------------

    char*  base;
    size_t xStride;
    size_t yStride;

    //--------------------------------------------
    // Subsampling: pixel (x, y) is present in the
    // slice only if
    //
    //  x % xSampling == 0 && y % ySampling == 0
    //
    //--------------------------------------------

    int xSampling;
    int ySampling;

    //----------------------------------------------------------
    // Default value, used to fill the slice when a file without
    // a channel that corresponds to this slice is read.
    //----------------------------------------------------------

    double fillValue;

    //-------------------------------------------------------
    // For tiled files, the xTileCoords and yTileCoords flags
    // determine whether pixel addressing is performed using
    // absolute coordinates or coordinates relative to a
    // tile's upper left corner.  (See the comment on base,
    // xStride and yStride, above.)
    //
    // For scanline-based files these flags have no effect;
    // pixel addressing is always done using absolute
    // coordinates.
    //-------------------------------------------------------

    bool xTileCoords;
    bool yTileCoords;

    //------------
    // Constructor
    //------------

    IMF_EXPORT
    Slice (
        PixelType type        = HALF,
        char*     base        = 0,
        size_t    xStride     = 0,
        size_t    yStride     = 0,
        int       xSampling   = 1,
        int       ySampling   = 1,
        double    fillValue   = 0.0,
        bool      xTileCoords = false,
        bool      yTileCoords = false);

    // Does the heavy lifting of computing the base pointer for a slice,
    // avoiding overflow issues with large origin offsets
    //
    // if xStride == 0, assumes sizeof(pixeltype)
    // if yStride == 0, assumes xStride * ( w / xSampling )
    IMF_EXPORT
    static Slice Make (
        PixelType                   type,
        const void*                 ptr,
        const IMATH_NAMESPACE::V2i& origin,
        int64_t                     w,
        int64_t                     h,
        size_t                      xStride     = 0,
        size_t                      yStride     = 0,
        int                         xSampling   = 1,
        int                         ySampling   = 1,
        double                      fillValue   = 0.0,
        bool                        xTileCoords = false,
        bool                        yTileCoords = false);
    // same as above, just computes w and h for you
    // from a data window
    IMF_EXPORT
    static Slice Make (
        PixelType                     type,
        const void*                   ptr,
        const IMATH_NAMESPACE::Box2i& dataWindow,
        size_t                        xStride     = 0,
        size_t                        yStride     = 0,
        int                           xSampling   = 1,
        int                           ySampling   = 1,
        double                        fillValue   = 0.0,
        bool                          xTileCoords = false,
        bool                          yTileCoords = false);

    //------------------------------------------------------------
    // Region destination builder.
    //
    // Builds a slice for a rectangular DESTINATION REGION expressed
    // in the file's original (data/display window) coordinates.
    // `ptr` addresses the destination pixel corresponding to
    // `region.min`; neighboring logical pixels are xStride bytes
    // apart and neighboring destination rows are yStride bytes
    // apart.  yStride is completely caller owned, so rows may carry
    // extra padding; pass 0 for a tight region.
    //
    // Unlike Make(), the strides passed in here are LOGICAL pixel
    // strides, not strides between subsampled samples.  The returned
    // slice scales them by the sampling factors and offsets its base
    // by the RAW region origin.  This keeps every channel sample on
    // its original sampling grid even when region.min is negative or
    // odd: for a 2-sampled channel and region.min.x == -3 the first
    // sample (global x == -2) lands one logical pixel inside the
    // destination, exactly as the original coordinates require.
    //
    // The resulting slice is meant to be handed to
    // InputFile::readRegion() / copyRegionPixels(); reads are clipped
    // to the region intersection, so bytes outside it are never
    // written and pre-existing background/padding/sentinels survive.
    //------------------------------------------------------------

    IMF_EXPORT
    static Slice MakeRegion (
        PixelType                     type,
        void*                         ptr,
        const IMATH_NAMESPACE::Box2i& region,
        size_t                        xStride   = 0,
        size_t                        yStride   = 0,
        int                           xSampling = 1,
        int                           ySampling = 1,
        double                        fillValue = 0.0);
};

class IMF_EXPORT_TYPE FrameBuffer
{
public:
    //------------
    // Add a slice
    //------------

    IMF_EXPORT
    void insert (const char name[], const Slice& slice);

    IMF_EXPORT
    void insert (const std::string& name, const Slice& slice);

    //----------------------------------------------------------------
    // Access to existing slices:
    //
    // [n]              Returns a reference to the slice with name n.
    //                  If no slice with name n exists, an IEX_NAMESPACE::ArgExc
    //                  is thrown.
    //
    // findSlice(n)     Returns a pointer to the slice with name n,
    //                  or 0 if no slice with name n exists.
    //
    //----------------------------------------------------------------

    IMF_EXPORT
    Slice& operator[] (const char name[]);
    IMF_EXPORT
    const Slice& operator[] (const char name[]) const;

    IMF_EXPORT
    Slice& operator[] (const std::string& name);
    IMF_EXPORT
    const Slice& operator[] (const std::string& name) const;

    IMF_EXPORT
    Slice* findSlice (const char name[]);
    IMF_EXPORT
    const Slice* findSlice (const char name[]) const;

    IMF_EXPORT
    Slice* findSlice (const std::string& name);
    IMF_EXPORT
    const Slice* findSlice (const std::string& name) const;

    //-----------------------------------------
    // Iterator-style access to existing slices
    //-----------------------------------------

    typedef std::map<Name, Slice> SliceMap;

    class Iterator;
    class ConstIterator;

    IMF_EXPORT
    Iterator begin ();
    IMF_EXPORT
    ConstIterator begin () const;

    IMF_EXPORT
    Iterator end ();
    IMF_EXPORT
    ConstIterator end () const;

    IMF_EXPORT
    Iterator find (const char name[]);
    IMF_EXPORT
    ConstIterator find (const char name[]) const;

    IMF_EXPORT
    Iterator find (const std::string& name);
    IMF_EXPORT
    ConstIterator find (const std::string& name) const;

private:
    SliceMap _map;
};

//------------------------------------------------------------
// Sampling-grid helpers.
//
// OpenEXR sample sites for sampling factor s are the integer
// multiples of s: ..., -2s, -s, 0, s, 2s, ...  Indexing that grid
// requires mathematical floor/ceil division; C++ truncation towards
// zero gives a different grid cell for negative non-multiples (for
// example -1 / 2 truncates to 0 while floor (-1/2) is -1).
// `sampling` must be positive.
//------------------------------------------------------------

IMF_EXPORT
int64_t floorSampleIndex (int64_t coordinate, int sampling);

IMF_EXPORT
int64_t ceilSampleIndex (int64_t coordinate, int sampling);

//------------------------------------------------------------
// Copy the samples of every same-named slice present in both
// frame buffers that fall inside
//
//     region intersect sourceBounds
//
// (both boxes are expressed in original/image coordinates).
//
// Addressing goes through each slice's sampling grid, so negative
// coordinates and negative odd region origins preserve the original
// channel sample positions.  Only actual sample sites inside the
// intersection are touched; everything else in the destination
// slices (margins, row padding, previously painted background,
// sentinels) is left unchanged.
//
// Source and destination slices must have the same pixel type and
// sampling factors.  Returns the intersected box, or an empty box
// when there is no intersection.
//------------------------------------------------------------

IMF_EXPORT
IMATH_NAMESPACE::Box2i copyRegionPixels (
    const FrameBuffer&                   source,
    const FrameBuffer&                   destination,
    const IMATH_NAMESPACE::Box2i&        region,
    const IMATH_NAMESPACE::Box2i&        sourceBounds);

//----------
// Iterators
//----------

class IMF_EXPORT_TYPE FrameBuffer::Iterator
{
public:
    IMF_EXPORT
    Iterator ();
    IMF_EXPORT
    Iterator (const FrameBuffer::SliceMap::iterator& i);

    IMF_EXPORT
    Iterator& operator++ ();
    IMF_EXPORT
    Iterator operator++ (int);

    IMF_EXPORT
    const char* name () const;
    IMF_EXPORT
    Slice& slice () const;

private:
    friend class FrameBuffer::ConstIterator;

    FrameBuffer::SliceMap::iterator _i;
};

class IMF_EXPORT_TYPE FrameBuffer::ConstIterator
{
public:
    IMF_EXPORT
    ConstIterator ();
    IMF_EXPORT
    ConstIterator (const FrameBuffer::SliceMap::const_iterator& i);
    IMF_EXPORT
    ConstIterator (const FrameBuffer::Iterator& other);

    IMF_EXPORT
    ConstIterator& operator++ ();
    IMF_EXPORT
    ConstIterator operator++ (int);

    IMF_EXPORT
    const char* name () const;
    IMF_EXPORT
    const Slice& slice () const;

private:
    friend bool operator== (const ConstIterator&, const ConstIterator&);
    friend bool operator!= (const ConstIterator&, const ConstIterator&);

    FrameBuffer::SliceMap::const_iterator _i;
};

//-----------------
// Inline Functions
//-----------------

inline FrameBuffer::Iterator::Iterator () : _i ()
{
    // empty
}

inline FrameBuffer::Iterator::Iterator (
    const FrameBuffer::SliceMap::iterator& i)
    : _i (i)
{
    // empty
}

inline FrameBuffer::Iterator&
FrameBuffer::Iterator::operator++ ()
{
    ++_i;
    return *this;
}

inline FrameBuffer::Iterator
FrameBuffer::Iterator::operator++ (int)
{
    Iterator tmp = *this;
    ++_i;
    return tmp;
}

inline const char*
FrameBuffer::Iterator::name () const
{
    return *_i->first;
}

inline Slice&
FrameBuffer::Iterator::slice () const
{
    return _i->second;
}

inline FrameBuffer::ConstIterator::ConstIterator () : _i ()
{
    // empty
}

inline FrameBuffer::ConstIterator::ConstIterator (
    const FrameBuffer::SliceMap::const_iterator& i)
    : _i (i)
{
    // empty
}

inline FrameBuffer::ConstIterator::ConstIterator (
    const FrameBuffer::Iterator& other)
    : _i (other._i)
{
    // empty
}

inline FrameBuffer::ConstIterator&
FrameBuffer::ConstIterator::operator++ ()
{
    ++_i;
    return *this;
}

inline FrameBuffer::ConstIterator
FrameBuffer::ConstIterator::operator++ (int)
{
    ConstIterator tmp = *this;
    ++_i;
    return tmp;
}

inline const char*
FrameBuffer::ConstIterator::name () const
{
    return *_i->first;
}

inline const Slice&
FrameBuffer::ConstIterator::slice () const
{
    return _i->second;
}

inline bool
operator== (
    const FrameBuffer::ConstIterator& x, const FrameBuffer::ConstIterator& y)
{
    return x._i == y._i;
}

inline bool
operator!= (
    const FrameBuffer::ConstIterator& x, const FrameBuffer::ConstIterator& y)
{
    return !(x == y);
}

OPENEXR_IMF_INTERNAL_NAMESPACE_HEADER_EXIT

#endif
