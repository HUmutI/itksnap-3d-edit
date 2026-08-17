/*=========================================================================

  Program:   ITK-SNAP
  Module:    BrushStamp3D.h
  Copyright (c) 2026 Paul A. Yushkevich

  This file is part of ITK-SNAP

  ITK-SNAP is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

=========================================================================*/
#ifndef __BrushStamp3D_h_
#define __BrushStamp3D_h_

#include "SNAPCommon.h"
#include "IRISVectorTypes.h"
#include "GlobalState.h"

#include <itkImageRegion.h>
#include <itkIndex.h>

#include <cmath>

/**
 * Helpers shared by the 3D brush stamp classes. Not part of the public API.
 */
namespace brush_stamp_3d_detail
{

/**
 * Per-axis scaling applied to a voxel-space delta before the inside/outside
 * test. When the brush is isotropic (i.e., a sphere in physical space), a
 * delta of one voxel along axis i corresponds to spacing[i] mm, so we express
 * everything in units of the smallest spacing. When the brush is not
 * isotropic, the scale is 1 on every axis and the brush is a sphere in
 * voxel space.
 */
inline Vector3d
ComputeAxisScale(bool isotropic, const Vector3d &spacing)
{
  Vector3d scale(1.0, 1.0, 1.0);
  if(isotropic)
    {
    double s_min = spacing.min_value();

    // Guard against degenerate (zero or negative) spacing, which would
    // otherwise produce inf/NaN scaling and an empty brush
    if(s_min > 0.0)
      {
      for(unsigned int i = 0; i < 3; i++)
        scale[i] = spacing[i] / s_min;
      }
    }
  return scale;
}

/** Effective radius, mirroring PaintbrushModel::TestInside */
inline double
ComputeEffectiveRadius(double radius)
{
  // The 0.25 offset is the convention used by the 2D paintbrush
  // (see PaintbrushModel::TestInside). It is what makes a radius-1 brush
  // paint the 7-voxel cross rather than a single voxel or a 3x3x3 cube.
  double r_eff = radius - 0.25;

  // The 2D code does not clamp; without the clamp a radius below 0.25 would
  // square to a positive number and the round brush would paint a *larger*
  // footprint than a radius just above 0.25.
  return r_eff > 0.0 ? r_eff : 0.0;
}

} // namespace brush_stamp_3d_detail


/**
 * \class BrushStamp3D
 * \brief A single 3D brush footprint (ball or cube) placed at a continuous
 * voxel-space location.
 *
 * This is a pure geometric predicate: it knows nothing about the segmentation
 * image, ITK filters, VTK or Qt. It exposes a bounding region in voxel index
 * space and an inside/outside test, which is everything
 * SegmentationEdit3D::ApplyStamp needs.
 *
 * The inside/outside semantics deliberately mirror
 * PaintbrushModel::TestInside so that the 3D brush and the volumetric 2D
 * brush paint the same footprint for the same radius.
 */
class BrushStamp3D
{
public:
  typedef itk::ImageRegion<3> RegionType;
  typedef itk::Index<3>       IndexType;

  /**
   * @param center    Brush center as a CONTINUOUS voxel index (not rounded)
   * @param radius    Brush radius, in voxels
   * @param shape     Round (ball) or rectangular (cube)
   * @param isotropic When true, the brush is a sphere in physical space
   * @param spacing   Image spacing in mm; only used when isotropic is true
   */
  BrushStamp3D(const Vector3d &center, double radius,
               PaintbrushShape shape, bool isotropic,
               const Vector3d &spacing)
    : m_Center(center), m_Shape(shape)
  {
    m_RadiusEff = brush_stamp_3d_detail::ComputeEffectiveRadius(radius);
    m_RadiusEffSquared = m_RadiusEff * m_RadiusEff;
    m_Scale = brush_stamp_3d_detail::ComputeAxisScale(isotropic, spacing);

    // Half-extent of the bounding box along each axis, in VOXELS.
    //
    // The inside test compares scale[i] * delta[i] against the radius, so a
    // voxel is inside only if |delta[i]| <= radius / scale[i]. We therefore
    // DIVIDE by the scale here. This is the opposite of what one might write
    // by reflex, and getting it backwards silently truncates the brush along
    // the coarsely-spaced axis (scale > 1 => a physical radius spans FEWER
    // voxels there, so multiplying would over-estimate; along the finely
    // spaced axis, scale < 1 => MORE voxels, and multiplying would clip the
    // brush without any visible error).
    for(unsigned int i = 0; i < 3; i++)
      m_HalfExtent[i] = radius / m_Scale[i];

    // The voxel the brush is pointing at.
    //
    // Unlike the 2D paintbrush, whose center is snapped to a voxel, the 3D
    // brush center comes from a pick on the rendered isosurface and therefore
    // lies BETWEEN voxel centers -- typically 0.3-0.5 voxels from the nearest
    // one, up to sqrt(3)/2 in the worst case. A purely geometric test then
    // makes small radii miss every voxel center, so the brush silently does
    // nothing at exactly the sizes needed for fine work. This voxel is
    // therefore always part of the stamp, which also makes the smallest brush
    // mean precisely "one voxel".
    for(unsigned int i = 0; i < 3; i++)
      m_SeedVoxel[i] = (long) std::floor(m_Center[i] + 0.5);
  }

  /**
   * Bounding region of the stamp, in voxel index space. This region is NOT
   * cropped to the image; the caller must call Crop() on it.
   */
  RegionType GetBoundingRegion() const
  {
    RegionType region;
    for(unsigned int i = 0; i < 3; i++)
      {
      // One extra voxel of margin on each side, so that rounding in the
      // continuous center never clips the footprint
      long lo = (long) std::floor(m_Center[i] - m_HalfExtent[i] - 1.0);
      long hi = (long) std::ceil (m_Center[i] + m_HalfExtent[i] + 1.0);
      region.SetIndex(i, lo);
      region.SetSize(i, (unsigned long) (hi - lo + 1));
      }
    return region;
  }

  /** Test whether a voxel index falls inside the brush footprint */
  bool TestInside(const IndexType &idx) const
  {
    // The pointed-at voxel is always inside; see the constructor
    if(idx == m_SeedVoxel)
      return true;

    return this->TestInside(to_double(idx));
  }

  /**
   * Test whether a continuous voxel-space position falls inside the brush.
   * Note that x is an ABSOLUTE position, not a delta from the center.
   */
  bool TestInside(const Vector3d &x) const
  {
    Vector3d delta;
    for(unsigned int i = 0; i < 3; i++)
      delta[i] = (x[i] - m_Center[i]) * m_Scale[i];

    if(m_Shape == PAINTBRUSH_ROUND)
      return delta.squared_magnitude() <= m_RadiusEffSquared;
    else
      return delta.inf_norm() <= m_RadiusEff;
  }

  /** Center of the stamp, as a continuous voxel index */
  const Vector3d &GetCenter() const { return m_Center; }

protected:

  // Center of the brush, continuous voxel index
  Vector3d m_Center;

  // Per-axis scaling applied before the distance test (all ones when the
  // brush is not isotropic)
  Vector3d m_Scale;

  // Half-extent of the bounding box along each axis, in voxels
  Vector3d m_HalfExtent;

  // The voxel containing the brush center; always part of the footprint
  IndexType m_SeedVoxel;

  // Effective radius (radius - 0.25) and its square
  double m_RadiusEff, m_RadiusEffSquared;

  // Footprint shape
  PaintbrushShape m_Shape;
};


/**
 * \class BrushCapsule3D
 * \brief The volume swept by a BrushStamp3D moving along a straight segment.
 *
 * This is used by the 3D "bridge" sub-tool, which connects two picked points
 * with a solid tube of the current brush radius. Using a capsule rather than
 * a chain of individual stamps guarantees a gap-free bridge regardless of how
 * far apart the two endpoints are.
 */
class BrushCapsule3D
{
public:
  typedef itk::ImageRegion<3> RegionType;
  typedef itk::Index<3>       IndexType;

  /**
   * @param a         First endpoint, as a CONTINUOUS voxel index
   * @param b         Second endpoint, as a CONTINUOUS voxel index
   * @param radius    Tube radius, in voxels
   * @param shape     Round (circular cross-section) or rectangular (square)
   * @param isotropic When true, the tube is round in physical space
   * @param spacing   Image spacing in mm; only used when isotropic is true
   */
  BrushCapsule3D(const Vector3d &a, const Vector3d &b, double radius,
                 PaintbrushShape shape, bool isotropic,
                 const Vector3d &spacing)
    : m_A(a), m_B(b), m_Shape(shape)
  {
    m_RadiusEff = brush_stamp_3d_detail::ComputeEffectiveRadius(radius);
    m_RadiusEffSquared = m_RadiusEff * m_RadiusEff;
    m_Scale = brush_stamp_3d_detail::ComputeAxisScale(isotropic, spacing);

    // The scaled axis of the capsule. The scaling MUST be applied before the
    // closest-point parameter t is computed: under anisotropic spacing the
    // Euclidean projection in voxel space and in physical space give
    // different values of t, and only the latter is the physically correct
    // closest point on the segment.
    m_ScaledA = this->ApplyScale(a);
    Vector3d scaled_b = this->ApplyScale(b);
    for(unsigned int i = 0; i < 3; i++)
      m_ScaledBA[i] = scaled_b[i] - m_ScaledA[i];

    m_ScaledBADotSelf = m_ScaledBA.squared_magnitude();

    // See the comment in BrushStamp3D::BrushStamp3D: we DIVIDE by the scale
    // to convert a radius expressed in scaled units back into voxels.
    for(unsigned int i = 0; i < 3; i++)
      m_HalfExtent[i] = radius / m_Scale[i];
  }

  /**
   * Bounding region of the capsule, in voxel index space. NOT cropped to the
   * image; the caller must call Crop() on it.
   */
  RegionType GetBoundingRegion() const
  {
    RegionType region;
    for(unsigned int i = 0; i < 3; i++)
      {
      double c_lo = m_A[i] < m_B[i] ? m_A[i] : m_B[i];
      double c_hi = m_A[i] > m_B[i] ? m_A[i] : m_B[i];

      long lo = (long) std::floor(c_lo - m_HalfExtent[i] - 1.0);
      long hi = (long) std::ceil (c_hi + m_HalfExtent[i] + 1.0);
      region.SetIndex(i, lo);
      region.SetSize(i, (unsigned long) (hi - lo + 1));
      }
    return region;
  }

  /** Test whether a voxel index falls inside the swept volume */
  bool TestInside(const IndexType &idx) const
  {
    return this->TestInside(to_double(idx));
  }

  /**
   * Test whether a continuous voxel-space position falls inside the swept
   * volume. x is an ABSOLUTE position.
   */
  bool TestInside(const Vector3d &x) const
  {
    Vector3d scaled_x = this->ApplyScale(x);

    Vector3d xa;
    for(unsigned int i = 0; i < 3; i++)
      xa[i] = scaled_x[i] - m_ScaledA[i];

    // Closest point on the segment, in scaled coordinates. When the two
    // endpoints coincide the capsule degenerates into a sphere centered at a,
    // which is exactly what t = 0 gives us.
    double t = 0.0;
    if(m_ScaledBADotSelf > 0.0)
      {
      double dot = 0.0;
      for(unsigned int i = 0; i < 3; i++)
        dot += xa[i] * m_ScaledBA[i];

      t = dot / m_ScaledBADotSelf;
      if(t < 0.0) t = 0.0;
      else if(t > 1.0) t = 1.0;
      }

    Vector3d d;
    for(unsigned int i = 0; i < 3; i++)
      d[i] = xa[i] - t * m_ScaledBA[i];

    if(m_Shape == PAINTBRUSH_ROUND)
      return d.squared_magnitude() <= m_RadiusEffSquared;
    else
      return d.inf_norm() <= m_RadiusEff;
  }

protected:

  Vector3d ApplyScale(const Vector3d &x) const
  {
    Vector3d y;
    for(unsigned int i = 0; i < 3; i++)
      y[i] = x[i] * m_Scale[i];
    return y;
  }

  // Endpoints in voxel space (used for the bounding box)
  Vector3d m_A, m_B;

  // Endpoint and axis in scaled coordinates (used for the inside test)
  Vector3d m_ScaledA, m_ScaledBA;
  double   m_ScaledBADotSelf;

  // Per-axis scaling and bounding box half-extent
  Vector3d m_Scale, m_HalfExtent;

  // Effective radius (radius - 0.25) and its square
  double m_RadiusEff, m_RadiusEffSquared;

  // Cross-section shape
  PaintbrushShape m_Shape;
};

#endif // __BrushStamp3D_h_
