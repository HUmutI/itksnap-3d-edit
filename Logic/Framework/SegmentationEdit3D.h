/*=========================================================================

  Program:   ITK-SNAP
  Module:    SegmentationEdit3D.h
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
#ifndef __SegmentationEdit3D_h_
#define __SegmentationEdit3D_h_

#include "SNAPCommon.h"
#include "LabelImageWrapper.h"
#include "SegmentationUpdateIterator.h"

#include <itkImageRegion.h>
#include <itkIndex.h>

/**
 * \class SegmentationEdit3D
 * \brief Voxel-level segmentation editing operations used by the 3D view
 * editing tool (PAINT3D_MODE).
 *
 * This class is a namespace of static methods. It lives in the Logic tier and
 * has no VTK or Qt dependency: all of the picking / ray casting happens in the
 * renderer and interaction mode, which then hand plain voxel coordinates to
 * these methods.
 *
 * Every method routes its writes through SegmentationUpdateIterator, which is
 * the only supported way to modify a segmentation image (it maintains the
 * undo delta and the modified flags).
 */
class SegmentationEdit3D
{
public:
  typedef LabelImageWrapper::ImageType   LabelImageType;
  typedef itk::ImageRegion<3>            RegionType;
  typedef itk::Index<3>                  IndexType;

  /**
   * Apply a brush stamp to the segmentation.
   *
   * TStamp is any class exposing
   *   RegionType GetBoundingRegion() const;
   *   bool TestInside(const itk::Index<3> &) const;
   * i.e., BrushStamp3D or BrushCapsule3D.
   *
   * @param seg          Segmentation layer being edited
   * @param stamp        Brush footprint to apply
   * @param active_label Label to paint with
   * @param draw_over    Draw-over filter (ignored when erasing)
   * @param erase        When true, paint the clear label over active_label
   * @param undo_string  When null, the delta is staged with
   *                     StoreIntermediateUndoDelta() so that it can be merged
   *                     into a single undo point at the end of a stroke. When
   *                     non-null, a complete undo point is stored with this
   *                     description.
   * @param dirty_out    If non-null, receives the CROPPED region that was
   *                     visited (safe to hand to a mesh/render update)
   * @return Number of voxels whose label actually changed
   */
  template <class TStamp>
  static unsigned long ApplyStamp(LabelImageWrapper *seg, const TStamp &stamp,
                                  LabelType active_label, DrawOverFilter draw_over,
                                  bool erase, const char *undo_string,
                                  RegionType *dirty_out);

  /**
   * Delete the connected component (island) containing the voxel `seed`.
   *
   * The search is a 6-connected flood fill over a region of interest that
   * starts small and doubles until the component is fully contained (or the
   * whole image has been covered). This is much cheaper than running a
   * connected-component filter over the entire segmentation, which is the
   * point: the user clicks on a stray speck and expects it to vanish
   * instantly.
   *
   * @param seg              Segmentation layer being edited
   * @param seed             Seed voxel (must be inside the buffered region)
   * @param match_any_nonzero When true, the component is grown across all
   *                     non-zero labels; when false, only voxels with the
   *                     seed's own label are included
   * @param max_voxels   Safety budget. If the component turns out to be
   *                     larger than this, NOTHING is deleted and the method
   *                     returns false with *n_voxels_out set, so that the
   *                     caller can ask the user for confirmation and call
   *                     again with a larger budget. Pass a very large value
   *                     to disable the check.
   * @param dirty_out    If non-null, receives the region that was modified
   * @param n_voxels_out If non-null, receives the size of the component. This
   *                     is filled in on both the success and the over-budget
   *                     path; on the over-budget path it is a lower bound,
   *                     since the region of interest stops growing once the
   *                     budget is blown.
   * @return true if voxels were deleted
   */
  static bool DeleteConnectedComponent(LabelImageWrapper *seg, const IndexType &seed,
                                       bool match_any_nonzero, unsigned long max_voxels,
                                       RegionType *dirty_out, unsigned long *n_voxels_out);

  /**
   * Perform a local morphological closing (dilate then erode) of the active
   * label in a neighborhood of `seed`. This fills small holes and gaps
   * without touching the rest of the segmentation.
   *
   * Only the active label participates in the closing. Using "any non-zero
   * voxel" as the foreground would silently merge the structure being
   * repaired with whatever neighboring structure happens to be within the
   * structuring element.
   *
   * @param seg            Segmentation layer being edited
   * @param seed           Center of the neighborhood
   * @param active_label   Label being closed (must be non-zero)
   * @param draw_over      Draw-over filter limiting what may be overwritten
   * @param closing_radius Structuring element radius, in voxels
   * @param isotropic      When true, the structuring element is a ball in
   *                       physical space rather than in voxel space
   * @param dirty_out      If non-null, receives the region that was modified
   * @return true if voxels were changed
   */
  static bool CloseHole(LabelImageWrapper *seg, const IndexType &seed,
                        LabelType active_label, DrawOverFilter draw_over,
                        double closing_radius, bool isotropic,
                        RegionType *dirty_out);
};


template <class TStamp>
unsigned long
SegmentationEdit3D::ApplyStamp(LabelImageWrapper *seg, const TStamp &stamp,
                               LabelType active_label, DrawOverFilter draw_over,
                               bool erase, const char *undo_string,
                               RegionType *dirty_out)
{
  if(!seg)
    return 0;

  // The stamp's bounding region is unbounded; crop it to the image. Crop()
  // returns false when there is no overlap at all, in which case there is
  // nothing to do (and the region it leaves behind is not usable).
  RegionType region = stamp.GetBoundingRegion();
  if(!region.Crop(seg->GetImage()->GetBufferedRegion()))
    {
    // Leave the caller with an empty (not garbage) dirty region. A
    // default-constructed itk::ImageRegion has zero index and zero size.
    if(dirty_out)
      *dirty_out = RegionType();
    return 0;
    }

  // Walk over the cropped region. Note that ++it_update runs for EVERY voxel
  // in the region, including the ones we skip: the undo delta is an RLE
  // stream in raster order, so skipping an increment would corrupt it.
  SegmentationUpdateIterator it_update(seg, region, active_label, draw_over);
  for(; !it_update.IsAtEnd(); ++it_update)
    {
    if(!stamp.TestInside(it_update.GetIndex()))
      continue;

    if(erase)
      it_update.PaintAsBackground();
    else
      it_update.PaintAsForeground();
    }

  // Grab the count before Finalize(), since Finalize() may relinquish the delta
  unsigned long n_changed = it_update.GetNumberOfChangedVoxels();

  if(undo_string)
    {
    // End of a stroke (or a one-shot operation): commit an undo point that
    // also absorbs any deltas staged earlier in the stroke
    it_update.Finalize(undo_string);
    }
  else if(it_update.Finalize())
    {
    // Mid-stroke: stage the delta so that the whole stroke becomes a single
    // undo point when the mouse is released
    seg->StoreIntermediateUndoDelta(it_update.RelinquishDelta());
    }

  if(dirty_out)
    *dirty_out = region;

  return n_changed;
}

#endif // __SegmentationEdit3D_h_
