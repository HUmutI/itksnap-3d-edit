/*=========================================================================

  Program:   ITK-SNAP
  Module:    Brush3DModel.h
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
#ifndef BRUSH3DMODEL_H
#define BRUSH3DMODEL_H

#include "AbstractModel.h"
#include "SNAPEvents.h"
#include "GlobalState.h"
#include "SegmentationEdit3D.h"

class Generic3DModel;
class Generic3DRenderer;
class IRISApplication;
class LabelImageWrapper;

/**
 * \class Brush3DModel
 * \brief Interaction state machine for the 3D view editing tool (PAINT3D_MODE).
 *
 * This model owns the state of a 3D editing gesture: where the brush currently
 * hovers, whether a stroke is in progress, and which endpoint of a bridge has
 * been picked. It is toolkit-independent; the Qt interactor style
 * (Brush3DInteractorStyle in GenericView3D.cxx) does nothing but forward mouse
 * coordinates here.
 *
 * Picking works against the *rendered mesh surface* (Generic3DRenderer::
 * PickMeshSurface, a vtkCellPicker restricted to the mesh actors) rather than
 * against the label image. That distinction is what makes the brush able to add
 * material: the picked point lies on the marching-cubes isosurface, i.e.
 * between labeled and unlabeled voxels, so a ball centered there covers
 * unlabeled voxels and the surface advances outward. The label-image ray march
 * (Generic3DModel::IntersectSegmentation) is only a fallback for when the mesh
 * is stale or absent, and it can only ever return an already-labeled voxel.
 */
class Brush3DModel : public AbstractModel
{
public:
  irisITKObjectMacro(Brush3DModel, AbstractModel)

  /** Fired when the hover position or brush geometry changes */
  itkEventMacro(Brush3DHoverEvent, IRISEvent)

  /** Fired when the gesture state changes (bridge endpoint set/cleared, etc.) */
  itkEventMacro(Brush3DStateEvent, IRISEvent)

  /** Fired after an edit has been committed and the mesh should be refreshed */
  itkEventMacro(Brush3DEditCommittedEvent, IRISEvent)

  /**
   * Fired when an additive action was refused because the active drawing label
   * is the clear label. Painting with label 0 erases, so for the additive
   * tools it is never what the user meant.
   */
  itkEventMacro(Brush3DInvalidLabelEvent, IRISEvent)

  FIRES(Brush3DHoverEvent)
  FIRES(Brush3DStateEvent)
  FIRES(Brush3DEditCommittedEvent)
  FIRES(Brush3DInvalidLabelEvent)

  typedef SegmentationEdit3D::IndexType  IndexType;
  typedef SegmentationEdit3D::RegionType RegionType;

  /** Outcome of a one-shot (non-stroke) sub-tool action */
  enum ActionResult
  {
    // The action was performed
    ACTION_DONE = 0,

    // Nothing under the cursor, or nothing to do; the caller should let the
    // camera have the event
    ACTION_NONE,

    // The action was refused because it would affect more voxels than the
    // safety budget allows. The caller must ask the user to confirm and then
    // call ConfirmPendingIslandDelete().
    ACTION_NEEDS_CONFIRMATION
  };

  void SetParent(Generic3DModel *parent);
  irisGetMacro(Parent, Generic3DModel *)

  /**
   * Subscribe to upstream events. Must be called after the parent model has
   * been given its GlobalUIModel, i.e. from Generic3DModel::Initialize.
   */
  void Initialize();

  // ------------------------------------------------------------------
  // Event handlers, called from the VTK interactor style. Each returns
  // true if the event was consumed (i.e., must NOT be forwarded to the
  // trackball camera).
  // ------------------------------------------------------------------

  /** Left/right button press. Returns true if a gesture was started. */
  bool ProcessPushEvent(int px, int py, bool erase);

  /** Mouse move with the button held. Returns true if it was consumed. */
  bool ProcessDragEvent(int px, int py);

  /** Button release. Returns true if a stroke was completed. */
  bool ProcessReleaseEvent(int px, int py);

  /** Mouse move with no button held; updates the hover preview. */
  void ProcessHoverEvent(int px, int py);

  /** Mouse left the view; hides the preview. */
  void ProcessLeaveEvent();

  /** Escape / Cancel. Returns true if there was something to cancel. */
  bool ProcessCancelEvent();

  // ------------------------------------------------------------------
  // State queries, used by the UI to enable buttons
  // ------------------------------------------------------------------

  /** Is a paint/erase stroke currently in progress? */
  irisIsMacro(Engaged)

  /** Has the first endpoint of a bridge been picked? */
  irisIsMacro(BridgePointPending)

  /** Is an island deletion awaiting user confirmation? */
  irisIsMacro(IslandConfirmPending)

  /** Number of voxels in an island whose deletion is awaiting confirmation */
  irisGetMacro(PendingIslandVoxels, unsigned long)

  /**
   * Delete the island that ACTION_NEEDS_CONFIRMATION was reported for. Safe to
   * call only immediately after that result; returns false otherwise.
   */
  bool ConfirmPendingIslandDelete();

  /** Discard a pending island-delete confirmation */
  void CancelPendingIslandDelete();

  /** Adjust the brush size by the given number of steps (keyboard shortcut) */
  void IncrementBrushSize(int delta);

  /** Adjust the brush depth by the given number of voxels (keyboard shortcut) */
  void IncrementDepth(int delta);

  /**
   * Abandon any gesture in progress. A stroke that has already written voxels
   * is committed first, because the image has been modified and its staged
   * undo deltas would otherwise be absorbed into an unrelated later undo point.
   */
  void AbandonGesture();

  /** Refresh the preview actors from the current settings (e.g. after a
   *  radius change from the options panel) */
  void UpdatePreview();

  /** Hide all preview actors. Called when leaving PAINT3D_MODE. */
  void HidePreview();

protected:
  Brush3DModel();
  virtual ~Brush3DModel() {}

  void OnUpdate() override;

  // ---- helpers -------------------------------------------------------

  Generic3DRenderer *GetRenderer() const;
  IRISApplication   *GetDriver() const;
  LabelImageWrapper *GetSegmentation() const;
  Brush3DSettings    GetSettings() const;

  /** Image spacing of the segmentation, as a Vector3d */
  bool GetSpacing(Vector3d &spacing) const;

  /**
   * Compute the view ray at a viewport pixel, expressed in continuous voxel
   * index units of the main image. Returns false if there is no main image.
   */
  bool ComputeVoxelRay(int px, int py, Vector3d &origin, Vector3d &direction) const;

  /**
   * Determine the brush center, in continuous voxel index units, for a
   * viewport pixel.
   *
   * The chain is: rendered mesh surface (vtkCellPicker) -> label image ray
   * march -> if allow_plane and a stroke is engaged, the plane through the
   * stroke anchor perpendicular to the anchor's view ray. The last step is what
   * lets the user drag out across a gap between two fragments; it must never be
   * used for an initial press, because a cold click on empty space has to fall
   * through to the camera.
   *
   * The depth setting is applied here, as an offset along the (voxel-space
   * normalized) view ray.
   */
  bool ComputeBrushCenter(int px, int py, bool allow_plane, bool allow_ray_march,
                          Vector3d &center, LabelType *label_out = nullptr) const;

  /**
   * Are the editing tools applicable at all right now? False when there is no
   * segmentation to edit, or when the active mesh layer is an externally
   * loaded mesh whose geometry has nothing to do with the label image.
   */
  bool IsEditingPossible() const;

  /** Per-axis brush radius in voxels, honoring the isotropic setting */
  Vector3d ComputeRadiusPerAxis(const Brush3DSettings &bs, const Vector3d &spacing) const;

  /** Paint a single stamp of the stroke */
  void ApplyStrokeStamp(const Vector3d &center);

  /** Finish a stroke: commit the undo point and fire the refresh event */
  void CommitStroke();

  /** Announce a committed edit (label use, SegmentationChangeEvent, refresh) */
  void NotifyEditCommitted();

  /** One-shot sub-tool entry points */
  ActionResult DoDeleteIsland(const Vector3d &center);
  ActionResult DoFillHole(const Vector3d &center);
  ActionResult DoBridge(const Vector3d &center);

  /** Round a continuous voxel index and test that it is inside the image */
  bool ToVoxelIndex(const Vector3d &center, IndexType &index) const;

  // ---- state ---------------------------------------------------------

  Generic3DModel *m_Parent;

  // A paint/erase stroke is in progress
  bool m_Engaged;

  // The current stroke is erasing rather than painting
  bool m_Erase;

  // Whether anything at all was changed during the current stroke, so that we
  // do not store an empty undo point
  bool m_StrokeChangedAnything;

  // Anchor of the current stroke: the first brush center and the view ray
  // there, both in voxel index units. Used for the drag-over-empty-space plane.
  Vector3d m_AnchorCenter, m_AnchorRay;

  // Center of the previous stamp, for stroke interpolation
  Vector3d m_LastCenter;

  // Hover state
  bool     m_HoverValid;
  Vector3d m_HoverCenter;

  // Bridge state
  bool     m_BridgePointPending;
  Vector3d m_BridgeA;

  // Island-delete confirmation state
  bool          m_IslandConfirmPending;
  IndexType     m_PendingIslandSeed;
  unsigned long m_PendingIslandVoxels;
};

#endif // BRUSH3DMODEL_H
