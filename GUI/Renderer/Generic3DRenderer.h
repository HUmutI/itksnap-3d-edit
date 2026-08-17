#ifndef GENERIC3DRENDERER_H
#define GENERIC3DRENDERER_H

#include "AbstractVTKRenderer.h"
#include <vtkSmartPointer.h>
#include "ActorPool.h"
#include <map>

class Generic3DModel;
class vtkGenericOpenGLRenderWindow;
class vtkRenderer;
class vtkRenderWindow;
class vtkLineSource;
class vtkActor;
class vtkActor2D;
class vtkProp3D;
class vtkPropAssembly;
class vtkProperty;
class vtkTransform;
class vtkImplicitPlaneWidget;
class vtkGlyph3D;
class vtkTransformPolyDataFilter;
class vtkCubeSource;
class vtkSphereSource;
class vtkTubeFilter;
class vtkCellPicker;
class vtkCoordinate;
class vtkCamera;
class vtkScalarBarActor;
class vtkPolyDataMapper;
class Window3DPicker;
class ImageWrapperBase;
class VolumeAssembly;
class ImageMeshLayers;

/**
 * A struct representing the state of the VTK camera. This struct
 * can be used to communicate camera state between ITK-SNAP sessions
 */
struct CameraState
{
  Vector3d position, focal_point, view_up;
  Vector2d clipping_range;
  double view_angle, parallel_scale;
  int parallel_projection;

};

bool operator == (const CameraState &c1, const CameraState &c2);
bool operator != (const CameraState &c1, const CameraState &c2);

class Generic3DRenderer : public AbstractVTKRenderer
{
public:

  irisITKObjectMacro(Generic3DRenderer, AbstractVTKRenderer)

  /** An event fired when the camera state updates */
  itkEventMacro(CameraUpdateEvent, IRISEvent)

  FIRES(CameraUpdateEvent)

  void SetRenderWindow(vtkRenderWindow *rwin) override;

  void SetModel(Generic3DModel *model);

  virtual void OnUpdate() override;

  void ResetView();

  // Save the camera state
  void SaveCameraState();

  // Clear the rendering
  void ClearRendering();

  // Restore the camera state from saved
  void RestoreSavedCameraState();

  // Restore the camera state from saved
  void DeleteSavedCameraState();

  // Restore the camera state from saved
  bool IsSavedCameraStateAvailable();

  /** Access the VTK camera object (used for synchronization) */
  CameraState GetCameraState() const;

  /** Change the camera state */
  void SetCameraState(const CameraState &state);

  /** Get the normal to the scalpel plane in world coordinates */
  Vector3d GetScalpelPlaneNormal() const;

  /** Get the origin of the scalpel plane in world coordinates */
  Vector3d GetScalpelPlaneOrigin() const;

  /** Flip the direction of the cutplane */
  void FlipScalpelPlaneNormal();

  /** Compute the world coordinates of a click and a ray pointing inward (not normalized) */
  void ComputeRayFromClick(int x, int y, Vector3d &point, Vector3d &ray, Vector3d &dx, Vector3d &dy);

  // ---------------------------------------------------------------------
  // 3D segmentation editing (PAINT3D_MODE) support
  // ---------------------------------------------------------------------

  /**
   * Pick the rendered mesh surface under a viewport pixel.
   * Returns true on a hit. Outputs are in NIFTI/RAS world coordinates.
   * The label is the segmentation label of the mesh actor that was hit.
   *
   * This uses a dedicated vtkCellPicker that is restricted (via PickFromListOn)
   * to the actors of the current mesh assembly, so the axis lines, the image
   * cube outline, the spray glyphs, the scalpel plane widget and any volume
   * rendering props cannot shadow the mesh.
   *
   * Note that this is completely independent of m_Picker (Window3DPicker), which
   * remains dedicated to the crosshair path.
   */
  bool PickMeshSurface(int x, int y, Vector3d &world_point, Vector3d &world_normal, LabelType &label);

  /**
   * Convert a point in NIFTI/RAS world coordinates (e.g. the output of
   * PickMeshSurface) to a continuous voxel index in the main image.
   * Returns false and leaves the output untouched if no main image is loaded.
   */
  bool WorldToVoxelCIndex(const Vector3d &world_point, Vector3d &voxel_cindex) const;

  /**
   * Convert a continuous voxel index in the main image to NIFTI/RAS world
   * coordinates. Returns false if no main image is loaded.
   */
  bool VoxelCIndexToWorld(const Vector3d &voxel_cindex, Vector3d &world_point) const;

  /** Show or hide the 3D brush preview (a wireframe ellipsoid) */
  void SetBrushPreviewVisible(bool visible);

  /**
   * Position and size the 3D brush preview. Both the center and the per-axis
   * radius are expressed in continuous voxel index units of the main image;
   * the voxel-to-world mapping is applied internally.
   */
  void SetBrushPreviewGeometry(const Vector3d &center_voxel, const Vector3d &radius_voxel);

  /** Show or hide the bridge preview (a wireframe tube between two points) */
  void SetBridgePreviewVisible(bool visible);

  /**
   * Position and size the bridge preview. The endpoints are continuous voxel
   * indices of the main image; the radius is in voxel units and is converted
   * internally to world (mm) units.
   */
  void SetBridgePreviewGeometry(const Vector3d &a_voxel, const Vector3d &b_voxel, double radius_voxel);

  /** Set the color of the brush and bridge previews (both use the same color) */
  void SetBrushPreviewColor(double r, double g, double b);

  /**
   * Force an immediate render. Needed by code paths (e.g. a debounce timer)
   * that update the scene outside of a VTK interactor event.
   */
  void RenderNow();

protected:
  Generic3DRenderer();
  virtual ~Generic3DRenderer() {}

  Generic3DModel *m_Model = nullptr;

  // Update the actors and mappings for the renderer
  void UpdateMeshAssembly();
  void UpdateMeshAppearance();

  // Clear all the meshes being rendered
  void ResetMeshAssembly();

  // Update the actors representing the axes
  void UpdateAxisRendering();

	// Update the scalar bar actor apperance
	void UpdateColorLegendAppearance();

  // Update the spray paint glyph properties
  void UpdateSprayGlyphAppearanceAndShape();

  // Update the scalpel rendering
  void UpdateScalpelRendering();

  // Update the scalpel plane appearance (color, etc)
  void UpdateScalpelPlaneAppearance();

  // Update the camera
  void UpdateCamera(bool reset);

  // Configure the volume rendering
  void UpdateVolumeRendering();

  // Apply changes in the display mapping policy to the actors
  void ApplyDisplayMappingPolicyChange();

  // Storage of ActorMap and a pool of actors for reuse in the map
  SmartPtr<ActorPool> m_ActorPool;

  // Indicate the current layer_id and timepoint that the ActorMap represents
  unsigned long m_CrntActorMapLayerId = 0;
  unsigned int m_CrntActorMapTimePoint = 0;

  // Line sources for drawing the crosshairs
  vtkSmartPointer<vtkLineSource> m_AxisLineSource[3];
  vtkSmartPointer<vtkActor> m_AxisActor[3];

  // Glyph filter used to render spray paint stuff
  vtkSmartPointer<vtkGlyph3D> m_SprayGlyphFilter;

  // The property controlling the spray paint
  vtkSmartPointer<vtkProperty> m_SprayProperty;

  // The transform applied to spray points
  vtkSmartPointer<vtkTransform> m_SprayTransform;
  vtkSmartPointer<vtkActor> m_SprayActor;

  // The actors for the scalpel drawing
  vtkSmartPointer<vtkLineSource> m_ScalpelLineSource;
  vtkSmartPointer<vtkActor2D> m_ScalpelLineActor;

  // The actors for the scalpel plane
  vtkSmartPointer<vtkCubeSource> m_ImageCubeSource;
  vtkSmartPointer<vtkTransformPolyDataFilter> m_ImageCubeTransform;
  vtkSmartPointer<vtkImplicitPlaneWidget> m_ScalpelPlaneWidget;

  // The actor for scalar bar
  vtkSmartPointer<vtkScalarBarActor> m_ScalarBarActor;

  // Coordinate mapper
  vtkSmartPointer<vtkCoordinate> m_CoordinateMapper;

  // Saved camera state
  vtkSmartPointer<vtkCamera> m_SavedCameraState;

  // Picker object
  vtkSmartPointer<Window3DPicker> m_Picker;

  // A separate picker used by the 3D segmentation editing tools. Unlike
  // m_Picker, it is restricted to the mesh actors of the current assembly and
  // reports the exact surface point and normal of the cell that was hit.
  vtkSmartPointer<vtkCellPicker> m_CellPicker;

  // Maps each mesh actor currently in the cell picker's pick list to the
  // segmentation label it represents. This must be rebuilt whenever the actor
  // map is rebuilt and cleared whenever the actors are recycled, because
  // ActorPool::RecycleAll() hands the same actors back out for different labels.
  std::map<vtkProp3D *, LabelType> m_ActorLabelMap;

  // ---------------- 3D BRUSH / BRIDGE PREVIEW ----------------

  // Unit sphere scaled and placed by m_BrushPreviewTransform
  vtkSmartPointer<vtkSphereSource>            m_BrushPreviewSource;
  vtkSmartPointer<vtkTransform>               m_BrushPreviewTransform;
  vtkSmartPointer<vtkTransformPolyDataFilter> m_BrushPreviewTransformFilter;
  vtkSmartPointer<vtkProperty>                m_BrushPreviewProperty;
  vtkSmartPointer<vtkActor>                   m_BrushPreviewActor;

  // Line in voxel space, mapped to world space and then tubed
  vtkSmartPointer<vtkLineSource>              m_BridgePreviewSource;
  vtkSmartPointer<vtkTransform>               m_BridgePreviewTransform;
  vtkSmartPointer<vtkTransformPolyDataFilter> m_BridgePreviewTransformFilter;
  vtkSmartPointer<vtkTubeFilter>              m_BridgePreviewTubeFilter;
  vtkSmartPointer<vtkProperty>                m_BridgePreviewProperty;
  vtkSmartPointer<vtkActor>                   m_BridgePreviewActor;

  // Get the voxel-to-world (NIFTI s-form) matrix of the main image. Returns
  // false if there is no main image, in which case sform is left untouched.
  bool GetMainImageVoxelToWorldMatrix(Matrix4d &sform) const;

  void UpdateVolumeCurves(ImageWrapperBase *layer, VolumeAssembly *va);
  void UpdateVolumeTransform(ImageWrapperBase *layer, VolumeAssembly *va);

  ImageMeshLayers *m_MeshLayers;
};

#endif // GENERIC3DRENDERER_H
