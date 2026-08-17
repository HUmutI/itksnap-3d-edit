#ifndef BRUSH3DSETTINGSMODEL_H
#define BRUSH3DSETTINGSMODEL_H

#include "AbstractModel.h"
#include "PropertyModel.h"
#include "GlobalState.h"

class GlobalUIModel;

/**
 * \class Brush3DSettingsModel
 * \brief Model that manages the settings of the 3D editing tool (PAINT3D_MODE).
 *
 * This model wraps the Brush3DSettings structure stored in GlobalState and
 * exposes its fields as individual property models that can be coupled to
 * Qt widgets. It follows the same pattern as PaintbrushSettingsModel.
 */
class Brush3DSettingsModel : public AbstractModel
{
public:
  irisITKObjectMacro(Brush3DSettingsModel, AbstractModel)

  typedef AbstractPropertyModel<Paint3DSubTool> AbstractSubToolModel;
  typedef AbstractPropertyModel<PaintbrushShape> AbstractPaintbrushShapeModel;

  irisGetMacro(ParentModel, GlobalUIModel *)
  void SetParentModel(GlobalUIModel *parent);

  /** The active sub-tool (brush, delete island, bridge, fill hole) */
  irisGenericPropertyAccessMacro(SubTool, Paint3DSubTool, TrivialDomain)

  /** Brush footprint shape (round = ball, rectangular = cube) */
  irisGenericPropertyAccessMacro(Shape, PaintbrushShape, TrivialDomain)

  /** Brush diameter in voxels (radius = 0.5 * size) */
  irisRangedPropertyAccessMacro(BrushSize, int)

  /** Whether the brush is isotropic in physical space */
  irisSimplePropertyAccessMacro(Isotropic, bool)

  /** Offset of the brush center along the view ray, in voxels */
  irisRangedPropertyAccessMacro(Depth, int)

  /** Structuring element radius for the fill-hole sub-tool, in voxels */
  irisRangedPropertyAccessMacro(ClosingRadius, int)

  /** Delete-island: match any non-zero label rather than the picked one */
  irisSimplePropertyAccessMacro(IslandAnyLabel, bool)

  /** Whether the brush size/depth controls apply to the active sub-tool */
  irisReadOnlySimplePropertyAccessMacro(BrushControlsVisible, bool)

  /** Whether the fill-hole controls apply to the active sub-tool */
  irisReadOnlySimplePropertyAccessMacro(FillHoleControlsVisible, bool)

  /** Whether the delete-island controls apply to the active sub-tool */
  irisReadOnlySimplePropertyAccessMacro(IslandControlsVisible, bool)

protected:

  Brush3DSettingsModel();
  virtual ~Brush3DSettingsModel();

  GlobalUIModel *m_ParentModel;

  // Model that sets and retrieves the whole Brush3DSettings structure
  typedef AbstractPropertyModel<Brush3DSettings> Brush3DSettingsStructModel;
  SmartPtr<Brush3DSettingsStructModel> m_Brush3DSettingsModel;
  Brush3DSettings GetBrush3DSettings();
  void SetBrush3DSettings(Brush3DSettings bs);

  SmartPtr<AbstractSubToolModel> m_SubToolModel;
  SmartPtr<AbstractPaintbrushShapeModel> m_ShapeModel;
  SmartPtr<AbstractSimpleBooleanProperty> m_IsotropicModel;
  SmartPtr<AbstractSimpleBooleanProperty> m_IslandAnyLabelModel;

  SmartPtr<AbstractRangedIntProperty> m_BrushSizeModel;
  bool GetBrushSizeValueAndRange(int &value, NumericValueRange<int> *domain);
  void SetBrushSizeValue(int value);

  SmartPtr<AbstractRangedIntProperty> m_DepthModel;
  bool GetDepthValueAndRange(int &value, NumericValueRange<int> *domain);
  void SetDepthValue(int value);

  SmartPtr<AbstractRangedIntProperty> m_ClosingRadiusModel;
  bool GetClosingRadiusValueAndRange(int &value, NumericValueRange<int> *domain);
  void SetClosingRadiusValue(int value);

  SmartPtr<AbstractSimpleBooleanProperty> m_BrushControlsVisibleModel;
  bool GetBrushControlsVisibleValue(bool &value);

  SmartPtr<AbstractSimpleBooleanProperty> m_FillHoleControlsVisibleModel;
  bool GetFillHoleControlsVisibleValue(bool &value);

  SmartPtr<AbstractSimpleBooleanProperty> m_IslandControlsVisibleModel;
  bool GetIslandControlsVisibleValue(bool &value);
};

#endif // BRUSH3DSETTINGSMODEL_H
