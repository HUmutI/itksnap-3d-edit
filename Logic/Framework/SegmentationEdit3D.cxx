/*=========================================================================

  Program:   ITK-SNAP
  Module:    SegmentationEdit3D.cxx
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
#include "SegmentationEdit3D.h"
#include "SegmentationUpdateIterator.h"
#include "RLERegionOfInterestImageFilter.h"

#include <itkImage.h>
#include <itkImageRegionConstIterator.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkBinaryBallStructuringElement.h>
#include <itkBinaryMorphologicalClosingImageFilter.h>

#include <vector>
#include <cmath>

namespace
{

// Dense (non-RLE) counterpart of the segmentation image, used as scratch space
// for the flood fill and the morphological closing. We never run these
// algorithms on the RLE image directly: RLEImage::GetPixel/SetPixel are linear
// in the length of the run-length line, which would make the flood fill
// quadratic.
typedef itk::Image<LabelType, 3>    DenseLabelImageType;
typedef itk::Image<unsigned char, 3> MaskImageType;

// Extract a region of the segmentation into a dense image. The output image is
// 0-based: its largest possible region has index (0,0,0) and the size of the
// requested ROI (see RLERegionOfInterestImageFilter.txx). All of the code
// below therefore works in ROI-local offsets and converts back to global
// indices only where needed.
DenseLabelImageType::Pointer
ExtractDenseROI(const SegmentationEdit3D::LabelImageType *src,
                const itk::ImageRegion<3> &roi)
{
  typedef itk::RegionOfInterestImageFilter<
      SegmentationEdit3D::LabelImageType, DenseLabelImageType> ROIFilterType;

  ROIFilterType::Pointer fltROI = ROIFilterType::New();
  fltROI->SetInput(src);
  fltROI->SetRegionOfInterest(roi);
  fltROI->Update();

  DenseLabelImageType::Pointer result = fltROI->GetOutput();
  result->DisconnectPipeline();
  return result;
}

} // anonymous namespace


bool
SegmentationEdit3D::DeleteConnectedComponent(LabelImageWrapper *seg, const IndexType &seed,
                                             bool match_any_nonzero, unsigned long max_voxels,
                                             RegionType *dirty_out, unsigned long *n_voxels_out)
{
  if(!seg)
    return false;

  const LabelImageType *img = seg->GetImage();
  RegionType full = img->GetBufferedRegion();

  // The seed must be a voxel of this image
  if(!full.IsInside(seed))
    return false;

  // Half-width of the initial region of interest: a 32-voxel cube around the
  // seed. Most islands the user clicks on are specks, so this is usually the
  // only iteration.
  long half = 16;

  RegionType                   roi;
  DenseLabelImageType::Pointer roi_image;
  MaskImageType::Pointer       mask;
  LabelType                    seed_label = 0;
  unsigned long                n_visited = 0;

  // The 6-connected neighborhood
  static const int nbr[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };

  // The ROI doubles on each iteration and is always cropped to the image, so
  // this loop terminates after log2(max image dimension) passes; the counter is
  // just a belt-and-braces guard. 24 passes reaches a half-width of 2^27, well
  // beyond any real image and still safely inside a 32-bit IndexValueType.
  for(int pass = 0; pass < 24; pass++)
    {
    // Build the candidate ROI and crop it to the image
    RegionType candidate;
    for(unsigned int i = 0; i < 3; i++)
      {
      candidate.SetIndex(i, seed[i] - half);
      candidate.SetSize(i, (unsigned long)(2 * half + 1));
      }

    if(!candidate.Crop(full))
      return false;

    roi = candidate;

    // Pull the ROI into a dense image
    roi_image = ExtractDenseROI(img, roi);

    // ROI-local offset of the seed
    IndexType seed_off;
    for(unsigned int i = 0; i < 3; i++)
      seed_off[i] = seed[i] - roi.GetIndex(i);

    // Determine which label we are deleting. Clicking on background is a no-op.
    seed_label = roi_image->GetPixel(seed_off);
    if(seed_label == 0)
      return false;

    // Allocate the visited/marked mask, which shares the ROI's 0-based geometry
    RegionType local_region = roi_image->GetLargestPossibleRegion();

    mask = MaskImageType::New();
    mask->SetRegions(local_region);
    mask->Allocate();
    mask->FillBuffer(0);

    // Breadth/depth-first flood fill from the seed. Voxels are marked at push
    // time so that each one enters the stack exactly once.
    std::vector<IndexType> stack;
    stack.reserve(1024);

    mask->SetPixel(seed_off, 1);
    stack.push_back(seed_off);
    n_visited = 1;

    // Whether the component runs into the edge of the ROI at a place where the
    // ROI edge is not also the edge of the image - i.e., whether the component
    // is likely to continue outside the ROI
    bool touches_roi_face = false;

    while(!stack.empty())
      {
      IndexType cur = stack.back();
      stack.pop_back();

      // Does this voxel sit on an "artificial" face of the ROI?
      for(unsigned int i = 0; i < 3; i++)
        {
        long g_lo = roi.GetIndex(i);
        long g_hi = g_lo + (long) roi.GetSize(i) - 1;
        long g = g_lo + cur[i];

        if((g == g_lo && g_lo > full.GetIndex(i)) ||
           (g == g_hi && g_hi < full.GetIndex(i) + (long) full.GetSize(i) - 1))
          touches_roi_face = true;
        }

      for(unsigned int k = 0; k < 6; k++)
        {
        IndexType nxt;
        nxt[0] = cur[0] + nbr[k][0];
        nxt[1] = cur[1] + nbr[k][1];
        nxt[2] = cur[2] + nbr[k][2];

        if(!local_region.IsInside(nxt))
          continue;

        if(mask->GetPixel(nxt))
          continue;

        LabelType v = roi_image->GetPixel(nxt);
        bool match = match_any_nonzero ? (v != 0) : (v == seed_label);
        if(!match)
          continue;

        mask->SetPixel(nxt, 1);
        stack.push_back(nxt);
        n_visited++;
        }
      }

    // Grow the ROI if the component appears to spill out of it. There is no
    // point in growing once we are already over budget (the caller is going to
    // be asked to confirm anyway) or once the ROI covers the whole image.
    if(touches_roi_face && n_visited <= max_voxels && roi != full)
      {
      half *= 2;
      continue;
      }

    break;
    }

  if(n_voxels_out)
    *n_voxels_out = n_visited;

  // Over budget: change nothing and let the caller confirm with the user
  if(n_visited > max_voxels)
    return false;

  // Erase every marked voxel. The mask and the update iterator walk the same
  // region in the same raster order, so they stay in lockstep; note that both
  // are incremented on every voxel, whether or not it is painted.
  SegmentationUpdateIterator it_update(seg, roi, 0, DrawOverFilter(PAINT_OVER_ALL, 0));
  itk::ImageRegionConstIterator<MaskImageType> it_mask(mask, mask->GetLargestPossibleRegion());

  for(; !it_update.IsAtEnd(); ++it_update, ++it_mask)
    {
    if(it_mask.Get())
      it_update.PaintLabel(0);
    }

  bool changed = it_update.Finalize("3D delete island");

  if(dirty_out)
    *dirty_out = roi;

  return changed;
}


bool
SegmentationEdit3D::CloseHole(LabelImageWrapper *seg, const IndexType &seed,
                              LabelType active_label, DrawOverFilter draw_over,
                              double closing_radius, bool isotropic,
                              RegionType *dirty_out)
{
  if(!seg || active_label == 0)
    return false;

  const LabelImageType *img = seg->GetImage();
  RegionType full = img->GetBufferedRegion();

  if(!full.IsInside(seed))
    return false;

  // Per-axis radius of the structuring element, in voxels. When the brush is
  // isotropic, a physical radius of closing_radius * s_min millimeters spans
  // closing_radius * s_min / s_i voxels along axis i.
  const LabelImageType::SpacingType &spacing = img->GetSpacing();
  double s_min = spacing[0];
  for(unsigned int i = 1; i < 3; i++)
    if(spacing[i] < s_min)
      s_min = spacing[i];

  itk::Size<3> se_radius;
  for(unsigned int i = 0; i < 3; i++)
    {
    long r = 1;
    if(isotropic && s_min > 0.0 && spacing[i] > 0.0)
      r = (long) std::lround(closing_radius * s_min / spacing[i]);
    else
      r = (long) std::lround(closing_radius);

    se_radius[i] = (itk::SizeValueType)(r < 1 ? 1 : r);
    }

  // Region of interest around the seed. The margin is 2r + 2 rather than the
  // r one might expect: closing is a dilation followed by an erosion, and if
  // the dilation is truncated at the ROI face the subsequent erosion eats away
  // material that should have survived, leaving a visible bite out of the
  // result at the ROI boundary.
  RegionType roi;
  for(unsigned int i = 0; i < 3; i++)
    {
    long k = (long) se_radius[i] * 2 + 2;
    roi.SetIndex(i, seed[i] - k);
    roi.SetSize(i, (unsigned long)(2 * k + 1));
    }

  if(!roi.Crop(full))
    return false;

  // Extract the ROI into a dense image and binarize it on the ACTIVE LABEL.
  // Using "any non-zero label" as the foreground would merge the structure
  // being repaired with any neighbor within reach of the structuring element.
  DenseLabelImageType::Pointer roi_image = ExtractDenseROI(img, roi);

  typedef itk::BinaryThresholdImageFilter<DenseLabelImageType, MaskImageType> ThresholdFilterType;
  ThresholdFilterType::Pointer fltThreshold = ThresholdFilterType::New();
  fltThreshold->SetInput(roi_image);
  fltThreshold->SetLowerThreshold(active_label);
  fltThreshold->SetUpperThreshold(active_label);
  fltThreshold->SetInsideValue(1);
  fltThreshold->SetOutsideValue(0);

  // Morphological closing with a ball (ellipsoid under anisotropic radii)
  typedef itk::BinaryBallStructuringElement<unsigned char, 3> StructuringElementType;
  StructuringElementType se;
  se.SetRadius(se_radius);
  se.CreateStructuringElement();

  typedef itk::BinaryMorphologicalClosingImageFilter<
      MaskImageType, MaskImageType, StructuringElementType> ClosingFilterType;
  ClosingFilterType::Pointer fltClosing = ClosingFilterType::New();
  fltClosing->SetInput(fltThreshold->GetOutput());
  fltClosing->SetKernel(se);
  fltClosing->SetForegroundValue(1);
  fltClosing->SetSafeBorder(true);
  fltClosing->Update();

  MaskImageType::Pointer closed = fltClosing->GetOutput();

  // Write the result back. Closing is extensive (output is a superset of the
  // input), so voxels can only be added: PaintAsForeground() on the closed
  // foreground is enough, and nothing is ever erased. Voxels that already
  // carry the active label are no-ops inside the iterator.
  SegmentationUpdateIterator it_update(seg, roi, active_label, draw_over);
  itk::ImageRegionConstIterator<MaskImageType> it_src(closed, closed->GetBufferedRegion());

  for(; !it_update.IsAtEnd(); ++it_update, ++it_src)
    {
    if(it_src.Get())
      it_update.PaintAsForeground();
    }

  bool changed = it_update.Finalize("3D fill hole");

  if(dirty_out)
    *dirty_out = roi;

  return changed;
}
