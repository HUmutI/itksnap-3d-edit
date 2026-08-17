/*=========================================================================

  Program:   ITK-SNAP
  Module:    Brush3DModel.cxx
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
#include "Brush3DModel.h"
#include "Generic3DModel.h"
#include "Generic3DRenderer.h"
#include "GlobalUIModel.h"
#include "GlobalState.h"
#include "IRISApplication.h"
#include "GenericImageData.h"
#include "LabelImageWrapper.h"
#include "ColorLabelTable.h"
#include "BrushStamp3D.h"
#include "Brush3DSettingsModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
// How far, in voxels, the drag-continuation plane point may sit from the stroke
// anchor before we treat the intersection as degenerate
const double kMaxPlaneDistance = 4096.0;

// Upper bound on the number of interpolated stamps in a single mouse move
const int kMaxInterpolationSteps = 256;

// Voxel budget above which deleting a connected region asks for confirmation.
// A stray fragment in an airway mask is a few hundred to a few thousand voxels;
// anything much larger is far more likely to be the airway itself.
const unsigned long kIslandConfirmThreshold = 20000ul;
}

Brush3DModel::Brush3DModel()
{
  m_Parent = NULL;
  m_Engaged = false;
  m_Erase = false;
  m_StrokeChangedAnything = false;
  m_AnchorCenter.fill(0.0);
  m_AnchorRay.fill(0.0);
  m_LastCenter.fill(0.0);
  m_HoverValid = false;
  m_HoverCenter.fill(0.0);
  m_BridgePointPending = false;
  m_BridgeA.fill(0.0);
  m_IslandConfirmPending = false;
  m_PendingIslandSeed.Fill(0);
  m_PendingIslandVoxels = 0;
}

void Brush3DModel::SetParent(Generic3DModel *parent)
{
  m_Parent = parent;
}

void Brush3DModel::Initialize()
{
  GlobalUIModel *ui = m_Parent->GetParentUI();
  IRISApplication *driver = ui->GetDriver();

  // The keyboard shortcuts and the options panel write to the same settings, so
  // a settings change has to refresh the preview.
  Rebroadcast(ui->GetBrush3DSettingsModel(), ModelUpdateEvent(), ModelUpdateEvent());
  Rebroadcast(driver->GetGlobalState()->GetDrawingColorLabelModel(),
              ValueChangedEvent(), ModelUpdateEvent());

  // Any change of context invalidates a gesture in progress: the voxel indices
  // we are holding on to refer to an image that is no longer displayed.
  Rebroadcast(driver, MainImageDimensionsChangeEvent(), ModelUpdateEvent());
  Rebroadcast(driver, LayerChangeEvent(), ModelUpdateEvent());
  Rebroadcast(driver, ActiveLayerChangeEvent(), ModelUpdateEvent());
  Rebroadcast(driver, CursorTimePointUpdateEvent(), ModelUpdateEvent());
}

void Brush3DModel::OnUpdate()
{
  GlobalUIModel *ui = m_Parent ? m_Parent->GetParentUI() : NULL;
  if(!ui)
    return;

  IRISApplication *driver = ui->GetDriver();

  bool context_changed =
      m_EventBucket->HasEvent(MainImageDimensionsChangeEvent(), driver) ||
      m_EventBucket->HasEvent(LayerChangeEvent(), driver) ||
      m_EventBucket->HasEvent(ActiveLayerChangeEvent(), driver) ||
      m_EventBucket->HasEvent(CursorTimePointUpdateEvent(), driver);

  if(context_changed)
    {
    // Drops the bridge endpoint and any half-finished stroke, and hides the
    // preview: everything it refers to belongs to the previous context.
    this->AbandonGesture();
    return;
    }

  // Otherwise this is a settings or label change; just refresh the preview
  this->UpdatePreview();
  if(Generic3DRenderer *ren = this->GetRenderer())
    ren->RenderNow();
}

Generic3DRenderer *Brush3DModel::GetRenderer() const
{
  return m_Parent ? m_Parent->GetRenderer() : NULL;
}

IRISApplication *Brush3DModel::GetDriver() const
{
  return m_Parent ? m_Parent->GetDriver() : NULL;
}

LabelImageWrapper *Brush3DModel::GetSegmentation() const
{
  IRISApplication *driver = this->GetDriver();
  return driver ? driver->GetSelectedSegmentationLayer() : NULL;
}

Brush3DSettings Brush3DModel::GetSettings() const
{
  return m_Parent->GetParentUI()->GetDriver()->GetGlobalState()->GetBrush3DSettings();
}

bool Brush3DModel::GetSpacing(Vector3d &spacing) const
{
  LabelImageWrapper *seg = this->GetSegmentation();
  if(!seg || !seg->GetImage())
    return false;

  for(int d = 0; d < 3; d++)
    spacing[d] = seg->GetImage()->GetSpacing()[d];

  return true;
}

bool Brush3DModel::ComputeVoxelRay(int px, int py, Vector3d &origin, Vector3d &direction) const
{
  Generic3DRenderer *ren = this->GetRenderer();
  if(!ren)
    return false;

  // The world-space ray at this pixel
  Vector3d x_world, ray_world, dx_world, dy_world;
  ren->ComputeRayFromClick(px, py, x_world, ray_world, dx_world, dy_world);

  // Map two points on the ray into voxel index space. The mapping is affine, so
  // the difference of the images is the image of the direction; doing it this
  // way avoids needing a separate vector (as opposed to point) transform.
  Vector3d c0, c1;
  if(!ren->WorldToVoxelCIndex(x_world, c0))
    return false;
  if(!ren->WorldToVoxelCIndex(x_world + ray_world, c1))
    return false;

  Vector3d dir = c1 - c0;
  double mag = dir.magnitude();
  if(mag <= 0.0)
    return false;

  origin = c0;

  // Normalize in VOXEL space, not world space: that is what makes the depth
  // setting mean "this many voxels".
  direction = dir / mag;
  return true;
}

bool Brush3DModel::IsEditingPossible() const
{
  if(!m_Parent || !this->GetSegmentation())
    return false;

  // An externally loaded mesh has no relationship to the label image, so a
  // surface pick on it would edit the segmentation at an unrelated location.
  if(m_Parent->CheckState(Generic3DModel::UIF_MESH_EXTERNAL))
    return false;

  return true;
}

bool Brush3DModel::ComputeBrushCenter(int px, int py, bool allow_plane, bool allow_ray_march,
                                      Vector3d &center, LabelType *label_out) const
{
  Generic3DRenderer *ren = this->GetRenderer();
  if(!ren || !this->IsEditingPossible())
    return false;

  Vector3d ray_origin, ray_dir;
  if(!this->ComputeVoxelRay(px, py, ray_origin, ray_dir))
    return false;

  bool found = false;
  Vector3d c;

  // 1. The rendered mesh surface. This is the preferred source, because the
  //    point lies ON the isosurface rather than at the center of an already
  //    labeled voxel, so a ball centered there covers unlabeled voxels and the
  //    brush can actually add material.
  Vector3d p_world, n_world;
  LabelType label = 0;
  if(ren->PickMeshSurface(px, py, p_world, n_world, label))
    {
    if(ren->WorldToVoxelCIndex(p_world, c))
      found = true;
    }

  // 2. Fall back to the label-image ray march. This matters right after the
  //    user has painted in 2D and before pressing Update: the mesh is stale but
  //    the label image is truth. It is a full DDA walk of the label image, so
  //    it is reserved for presses and drags -- never for hover, which would run
  //    it on every mouse-move frame.
  if(!found && allow_ray_march && m_Parent)
    {
    Vector3i hit;
    if(m_Parent->IntersectSegmentation(px, py, hit))
      {
      c = to_double(hit);
      label = 0;
      found = true;
      }
    }

  // The point from 1 and 2 still needs the depth offset; the one from 3 does
  // not, because the anchor it is derived from was already depth-adjusted.
  bool needs_depth = true;

  // 3. Drag continuation: the user has dragged off the surface, into the gap
  //    they are trying to bridge. Intersect this pixel's ray with the plane
  //    through the stroke anchor, perpendicular to the anchor's own ray.
  if(!found && allow_plane && m_Engaged)
    {
    double denom = dot_product(ray_dir, m_AnchorRay);
    if(std::fabs(denom) > 1e-8)
      {
      double t = dot_product(m_AnchorCenter - ray_origin, m_AnchorRay) / denom;

      // t <= 0 is behind the camera; a grazing ray also produces an absurdly
      // distant point. Either way it is not something the user pointed at.
      if(t > 0.0)
        {
        Vector3d p = ray_origin + ray_dir * t;
        if((p - m_AnchorCenter).magnitude() < kMaxPlaneDistance)
          {
          c = p;
          found = true;
          needs_depth = false;
          }
        }
      }
    }

  if(!found)
    return false;

  // Apply the depth offset, along the view ray, in voxels
  if(needs_depth)
    {
    Brush3DSettings bs = this->GetSettings();
    if(bs.depth != 0)
      c += ray_dir * (double) bs.depth;
    }

  center = c;
  if(label_out)
    *label_out = label;

  return true;
}

Vector3d Brush3DModel::ComputeRadiusPerAxis(const Brush3DSettings &bs,
                                            const Vector3d &spacing) const
{
  Vector3d r;
  if(bs.isotropic)
    {
    // A fixed physical radius spans FEWER voxels along a coarsely spaced axis,
    // hence the division. This matches BrushStamp3D::GetBoundingRegion.
    double s_min = std::min(spacing[0], std::min(spacing[1], spacing[2]));
    for(int d = 0; d < 3; d++)
      r[d] = bs.radius * s_min / spacing[d];
    }
  else
    {
    r.fill(bs.radius);
    }
  return r;
}

bool Brush3DModel::ToVoxelIndex(const Vector3d &center, IndexType &index) const
{
  LabelImageWrapper *seg = this->GetSegmentation();
  if(!seg || !seg->GetImage())
    return false;

  for(int d = 0; d < 3; d++)
    index[d] = (long) std::floor(center[d] + 0.5);

  return seg->GetImage()->GetBufferedRegion().IsInside(index);
}

void Brush3DModel::UpdatePreview()
{
  Generic3DRenderer *ren = this->GetRenderer();
  if(!ren)
    return;

  Vector3d spacing;
  if(!m_HoverValid || !this->GetSpacing(spacing))
    {
    ren->SetBrushPreviewVisible(false);
    ren->SetBridgePreviewVisible(false);
    return;
    }

  Brush3DSettings bs = this->GetSettings();

  // Color the preview with the active drawing label
  IRISApplication *driver = this->GetDriver();
  if(driver)
    {
    LabelType dl = driver->GetGlobalState()->GetDrawingColorLabel();
    const ColorLabel &cl = driver->GetColorLabelTable()->GetColorLabel(dl);
    Vector3d rgb = cl.GetRGBAsDoubleVector();
    ren->SetBrushPreviewColor(rgb[0], rgb[1], rgb[2]);
    }

  // Delete-island acts at a point rather than over a footprint, so a big sphere
  // would misrepresent what it does; fill-hole's footprint is its structuring
  // element, not the brush radius.
  Brush3DSettings bs_preview = bs;
  if(bs.sub_tool == PAINT3D_DELETE_ISLAND)
    bs_preview.radius = 1.0;
  else if(bs.sub_tool == PAINT3D_FILL_HOLE)
    bs_preview.radius = bs.closing_radius;

  // Draw the footprint that will actually be painted, i.e. the effective
  // radius, not the nominal one. At radius 1 the difference is 33%, which is
  // exactly the regime this tool is meant to be precise in.
  bs_preview.radius = brush_stamp_3d_detail::ComputeEffectiveRadius(bs_preview.radius);

  // The stamp always includes the voxel under the cursor, so the marker must
  // never shrink below one voxel: at the smallest brush size the geometric
  // radius is 0.25, which would draw an almost invisible dot for something
  // that does in fact paint a whole voxel.
  Vector3d r_preview = this->ComputeRadiusPerAxis(bs_preview, spacing);
  for(int d = 0; d < 3; d++)
    r_preview[d] = std::max(0.5, r_preview[d]);

  ren->SetBrushPreviewGeometry(m_HoverCenter, r_preview);
  ren->SetBrushPreviewVisible(true);

  // Bridge preview: from the pending endpoint A to the current hover point
  if(bs.sub_tool == PAINT3D_BRIDGE && m_BridgePointPending)
    {
    ren->SetBridgePreviewGeometry(m_BridgeA, m_HoverCenter, bs.radius);
    ren->SetBridgePreviewVisible(true);
    }
  else
    {
    ren->SetBridgePreviewVisible(false);
    }
}

void Brush3DModel::HidePreview()
{
  m_HoverValid = false;
  Generic3DRenderer *ren = this->GetRenderer();
  if(ren)
    {
    ren->SetBrushPreviewVisible(false);
    ren->SetBridgePreviewVisible(false);
    }
}

void Brush3DModel::AbandonGesture()
{
  // A stroke that already wrote voxels must be committed: the image has been
  // modified, and leaving its deltas staged would attach them to whatever undo
  // point some unrelated later edit creates.
  if(m_Engaged)
    {
    m_Engaged = false;
    this->CommitStroke();
    }

  m_StrokeChangedAnything = false;

  bool had_state = m_BridgePointPending || m_IslandConfirmPending;
  m_BridgePointPending = false;
  m_IslandConfirmPending = false;
  m_PendingIslandVoxels = 0;

  this->HidePreview();

  if(had_state)
    this->InvokeEvent(Brush3DStateEvent());
}

void Brush3DModel::ProcessHoverEvent(int px, int py)
{
  Vector3d c;
  bool ok = this->ComputeBrushCenter(px, py, false, false, c);

  bool changed = (ok != m_HoverValid) || (ok && (c - m_HoverCenter).magnitude() > 1e-6);

  m_HoverValid = ok;
  if(ok)
    m_HoverCenter = c;

  if(changed)
    {
    this->UpdatePreview();
    this->InvokeEvent(Brush3DHoverEvent());
    }
}

void Brush3DModel::ProcessLeaveEvent()
{
  if(m_HoverValid)
    {
    this->HidePreview();
    this->InvokeEvent(Brush3DHoverEvent());
    }
}

bool Brush3DModel::ProcessPushEvent(int px, int py, bool erase)
{
  // Never start a second gesture on top of a live one: it would reset the
  // stroke bookkeeping and orphan the first stroke's staged undo deltas.
  if(m_Engaged || !this->IsEditingPossible())
    return false;

  Brush3DSettings bs = this->GetSettings();

  // Every sub-tool except the eraser and delete-island ADDS material, and adds
  // it with the active drawing label. If that label is the clear label, then
  // "paint" writes zeros, i.e. it silently carves material away -- the exact
  // opposite of what the tool is for. Refuse rather than destroy data.
  bool is_additive = (bs.sub_tool == PAINT3D_BRIDGE)
                  || (bs.sub_tool == PAINT3D_FILL_HOLE)
                  || (bs.sub_tool == PAINT3D_BRUSH && !erase);

  if(is_additive && this->GetDriver()->GetGlobalState()->GetDrawingColorLabel() == 0)
    {
    this->InvokeEvent(Brush3DInvalidLabelEvent());

    // Consume the click: letting it through would spin the camera, which reads
    // as "the tool did something random" rather than "the tool refused".
    return true;
    }

  // A cold press must never use the drag-continuation plane: clicking on empty
  // space has to fall through to the camera.
  Vector3d c;
  if(!this->ComputeBrushCenter(px, py, false, true, c))
    return false;

  m_HoverValid = true;
  m_HoverCenter = c;

  switch(bs.sub_tool)
    {
    case PAINT3D_BRUSH:
      {
      Vector3d ray_origin, ray_dir;
      if(!this->ComputeVoxelRay(px, py, ray_origin, ray_dir))
        return false;

      m_Engaged = true;
      m_Erase = erase;
      m_StrokeChangedAnything = false;
      m_AnchorCenter = c;
      m_AnchorRay = ray_dir;
      m_LastCenter = c;
      this->ApplyStrokeStamp(c);
      this->UpdatePreview();
      return true;
      }

    case PAINT3D_DELETE_ISLAND:
      return this->DoDeleteIsland(c) != ACTION_NONE;

    case PAINT3D_BRIDGE:
      return this->DoBridge(c) != ACTION_NONE;

    case PAINT3D_FILL_HOLE:
      return this->DoFillHole(c) != ACTION_NONE;
    }

  return false;
}

bool Brush3DModel::ProcessDragEvent(int px, int py)
{
  if(!m_Engaged)
    return false;

  Vector3d c;
  if(!this->ComputeBrushCenter(px, py, true, true, c))
    return true;   // still engaged; just nothing to paint at this pixel

  Brush3DSettings bs = this->GetSettings();
  double step = std::max(0.5, bs.radius);

  // Interpolate along the path, otherwise a fast drag comes out dotted
  Vector3d delta = c - m_LastCenter;
  double dist = delta.magnitude();
  if(dist > step)
    {
    // Clamp: a degenerate plane intersection can put c very far away, and on
    // arm64 an out-of-range double->int conversion saturates to INT_MAX rather
    // than wrapping, which would be a multi-billion-iteration freeze.
    double steps = std::ceil(dist / step);
    int n = (steps >= (double) kMaxInterpolationSteps)
              ? kMaxInterpolationSteps
              : (int) steps;
    for(int i = 1; i <= n; i++)
      this->ApplyStrokeStamp(m_LastCenter + delta * ((double) i / n));
    }
  else
    {
    this->ApplyStrokeStamp(c);
    }

  m_LastCenter = c;
  m_HoverValid = true;
  m_HoverCenter = c;
  this->UpdatePreview();
  return true;
}

bool Brush3DModel::ProcessReleaseEvent(int, int)
{
  if(!m_Engaged)
    return false;

  m_Engaged = false;
  this->CommitStroke();
  return true;
}

bool Brush3DModel::ProcessCancelEvent()
{
  bool did_something = false;

  if(m_IslandConfirmPending)
    {
    this->CancelPendingIslandDelete();
    did_something = true;
    }

  if(m_BridgePointPending)
    {
    m_BridgePointPending = false;
    this->UpdatePreview();
    this->InvokeEvent(Brush3DStateEvent());
    did_something = true;
    }

  return did_something;
}

void Brush3DModel::ApplyStrokeStamp(const Vector3d &center)
{
  LabelImageWrapper *seg = this->GetSegmentation();
  Vector3d spacing;
  if(!seg || !this->GetSpacing(spacing))
    return;

  IRISApplication *driver = this->GetDriver();
  Brush3DSettings bs = this->GetSettings();

  BrushStamp3D stamp(center, bs.radius, bs.shape, bs.isotropic, spacing);

  RegionType dirty;
  unsigned long n = SegmentationEdit3D::ApplyStamp(
        seg, stamp,
        driver->GetGlobalState()->GetDrawingColorLabel(),
        driver->GetGlobalState()->GetDrawOverFilter(),
        m_Erase,
        NULL,      // mid-stroke: stage the delta, do not commit an undo point
        &dirty);

  if(n > 0)
    m_StrokeChangedAnything = true;
}

void Brush3DModel::CommitStroke()
{
  LabelImageWrapper *seg = this->GetSegmentation();
  if(!seg || !m_StrokeChangedAnything)
    return;

  seg->StoreUndoPoint(m_Erase ? "3D brush erase" : "3D brush");
  m_StrokeChangedAnything = false;
  this->NotifyEditCommitted();
}

void Brush3DModel::NotifyEditCommitted()
{
  IRISApplication *driver = this->GetDriver();
  if(driver)
    {
    driver->RecordCurrentLabelUse();
    driver->InvokeEvent(SegmentationChangeEvent());
    }

  this->InvokeEvent(Brush3DEditCommittedEvent());
}

Brush3DModel::ActionResult Brush3DModel::DoDeleteIsland(const Vector3d &center)
{
  LabelImageWrapper *seg = this->GetSegmentation();
  IndexType seed;
  if(!seg || !this->ToVoxelIndex(center, seed))
    return ACTION_NONE;

  Brush3DSettings bs = this->GetSettings();

  // The picked point sits ON the isosurface, so rounding it can land just
  // outside the labeled voxel. Nudge toward the first labeled voxel in a small
  // neighborhood before giving up.
  if(seg->GetImage()->GetPixel(seed) == 0)
    {
    bool fixed = false;
    for(int dz = -1; dz <= 1 && !fixed; dz++)
      for(int dy = -1; dy <= 1 && !fixed; dy++)
        for(int dx = -1; dx <= 1 && !fixed; dx++)
          {
          IndexType probe = seed;
          probe[0] += dx; probe[1] += dy; probe[2] += dz;
          if(seg->GetImage()->GetBufferedRegion().IsInside(probe)
             && seg->GetImage()->GetPixel(probe) != 0)
            {
            seed = probe;
            fixed = true;
            }
          }
    if(!fixed)
      return ACTION_NONE;
    }

  const unsigned long budget = kIslandConfirmThreshold;

  RegionType dirty;
  unsigned long n_voxels = 0;
  if(SegmentationEdit3D::DeleteConnectedComponent(
       seg, seed, bs.island_any_label, budget, &dirty, &n_voxels))
    {
    this->NotifyEditCommitted();
    return ACTION_DONE;
    }

  if(n_voxels > 0)
    {
    // Too big to delete without asking. Stash the seed so that the view can put
    // up a confirmation dialog and then call ConfirmPendingIslandDelete().
    m_IslandConfirmPending = true;
    m_PendingIslandSeed = seed;
    m_PendingIslandVoxels = n_voxels;
    this->InvokeEvent(Brush3DStateEvent());
    return ACTION_NEEDS_CONFIRMATION;
    }

  return ACTION_NONE;
}

bool Brush3DModel::ConfirmPendingIslandDelete()
{
  LabelImageWrapper *seg = this->GetSegmentation();
  if(!m_IslandConfirmPending || !seg)
    return false;

  Brush3DSettings bs = this->GetSettings();
  m_IslandConfirmPending = false;

  RegionType dirty;
  unsigned long n_voxels = 0;
  bool ok = SegmentationEdit3D::DeleteConnectedComponent(
        seg, m_PendingIslandSeed, bs.island_any_label,
        std::numeric_limits<unsigned long>::max(), &dirty, &n_voxels);

  if(ok)
    this->NotifyEditCommitted();

  this->InvokeEvent(Brush3DStateEvent());
  return ok;
}

void Brush3DModel::CancelPendingIslandDelete()
{
  if(m_IslandConfirmPending)
    {
    m_IslandConfirmPending = false;
    m_PendingIslandVoxels = 0;
    this->InvokeEvent(Brush3DStateEvent());
    }
}

Brush3DModel::ActionResult Brush3DModel::DoFillHole(const Vector3d &center)
{
  LabelImageWrapper *seg = this->GetSegmentation();
  IndexType seed;
  if(!seg || !this->ToVoxelIndex(center, seed))
    return ACTION_NONE;

  IRISApplication *driver = this->GetDriver();
  Brush3DSettings bs = this->GetSettings();

  RegionType dirty;
  bool changed = SegmentationEdit3D::CloseHole(
        seg, seed,
        driver->GetGlobalState()->GetDrawingColorLabel(),
        driver->GetGlobalState()->GetDrawOverFilter(),
        bs.closing_radius, bs.isotropic, &dirty);

  if(!changed)
    return ACTION_NONE;

  this->NotifyEditCommitted();
  return ACTION_DONE;
}

Brush3DModel::ActionResult Brush3DModel::DoBridge(const Vector3d &center)
{
  LabelImageWrapper *seg = this->GetSegmentation();
  Vector3d spacing;
  if(!seg || !this->GetSpacing(spacing))
    return ACTION_NONE;

  // First click: remember the endpoint and wait for the second
  if(!m_BridgePointPending)
    {
    m_BridgeA = center;
    m_BridgePointPending = true;
    this->UpdatePreview();
    this->InvokeEvent(Brush3DStateEvent());
    return ACTION_DONE;
    }

  IRISApplication *driver = this->GetDriver();
  Brush3DSettings bs = this->GetSettings();

  // Extend both endpoints outward along the axis by the brush radius, so that
  // the tube overlaps both fragments rather than merely touching them. Surface
  // smoothing routinely offsets the picked point by a fraction of a voxel, and
  // without this the "connect" visibly fails to connect.
  Vector3d a = m_BridgeA, b = center;
  Vector3d axis = b - a;
  double len = axis.magnitude();
  if(len > 1e-6)
    {
    Vector3d u = axis / len;
    a -= u * bs.radius;
    b += u * bs.radius;
    }

  BrushCapsule3D capsule(a, b, bs.radius, bs.shape, bs.isotropic, spacing);

  RegionType dirty;
  unsigned long n = SegmentationEdit3D::ApplyStamp(
        seg, capsule,
        driver->GetGlobalState()->GetDrawingColorLabel(),
        driver->GetGlobalState()->GetDrawOverFilter(),
        false,               // bridging always adds
        "3D connect",        // one-shot: its own undo point
        &dirty);

  m_BridgePointPending = false;
  this->UpdatePreview();
  this->InvokeEvent(Brush3DStateEvent());

  if(n == 0)
    return ACTION_NONE;

  this->NotifyEditCommitted();
  return ACTION_DONE;
}

// The keyboard shortcuts must go through the settings model rather than
// straight to GlobalState: GlobalState::SetBrush3DSettings fires nothing, so a
// direct write would leave the options panel showing a stale value, and the
// next panel interaction would silently write that stale value back.

void Brush3DModel::IncrementBrushSize(int delta)
{
  Brush3DSettingsModel *sm = m_Parent->GetParentUI()->GetBrush3DSettingsModel();

  int value = 0;
  NumericValueRange<int> range;
  if(!sm->GetBrushSizeModel()->GetValueAndDomain(value, &range))
    return;

  int v = std::max(range.Minimum, std::min(range.Maximum, value + delta * range.StepSize));
  if(v != value)
    sm->SetBrushSize(v);
}

void Brush3DModel::IncrementDepth(int delta)
{
  Brush3DSettingsModel *sm = m_Parent->GetParentUI()->GetBrush3DSettingsModel();

  int value = 0;
  NumericValueRange<int> range;
  if(!sm->GetDepthModel()->GetValueAndDomain(value, &range))
    return;

  int v = std::max(range.Minimum, std::min(range.Maximum, value + delta * range.StepSize));
  if(v != value)
    sm->SetDepth(v);
}
