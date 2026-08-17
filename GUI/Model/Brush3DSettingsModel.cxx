#include "Brush3DSettingsModel.h"
#include "GlobalUIModel.h"
#include "GlobalState.h"
#include "GlobalPreferencesModel.h"
#include "DefaultBehaviorSettings.h"
#include "IRISApplication.h"

Brush3DSettingsModel::Brush3DSettingsModel()
{
  m_Brush3DSettingsModel =
      wrapGetterSetterPairAsProperty(this,
                                     &Self::GetBrush3DSettings,
                                     &Self::SetBrush3DSettings);

  // Create models for the fields of Brush3DSettings
  m_SubToolModel =
      wrapStructMemberAsSimpleProperty<Brush3DSettings, Paint3DSubTool>(
        m_Brush3DSettingsModel, offsetof(Brush3DSettings, sub_tool));

  m_ShapeModel =
      wrapStructMemberAsSimpleProperty<Brush3DSettings, PaintbrushShape>(
        m_Brush3DSettingsModel, offsetof(Brush3DSettings, shape));

  m_IsotropicModel =
      wrapStructMemberAsSimpleProperty<Brush3DSettings, bool>(
        m_Brush3DSettingsModel, offsetof(Brush3DSettings, isotropic));

  m_IslandAnyLabelModel =
      wrapStructMemberAsSimpleProperty<Brush3DSettings, bool>(
        m_Brush3DSettingsModel, offsetof(Brush3DSettings, island_any_label));

  // The brush size model requires special processing (size = 2 * radius),
  // so it is implemented using a getter/setter pair
  m_BrushSizeModel = wrapGetterSetterPairAsProperty(
        this,
        &Self::GetBrushSizeValueAndRange,
        &Self::SetBrushSizeValue);

  m_DepthModel = wrapGetterSetterPairAsProperty(
        this,
        &Self::GetDepthValueAndRange,
        &Self::SetDepthValue);

  m_ClosingRadiusModel = wrapGetterSetterPairAsProperty(
        this,
        &Self::GetClosingRadiusValueAndRange,
        &Self::SetClosingRadiusValue);

  // Read-only models used to show/hide sub-tool specific controls
  m_BrushControlsVisibleModel = wrapGetterSetterPairAsProperty(
        this, &Self::GetBrushControlsVisibleValue);

  m_FillHoleControlsVisibleModel = wrapGetterSetterPairAsProperty(
        this, &Self::GetFillHoleControlsVisibleValue);

  m_IslandControlsVisibleModel = wrapGetterSetterPairAsProperty(
        this, &Self::GetIslandControlsVisibleValue);
}

Brush3DSettingsModel::~Brush3DSettingsModel()
{
}

void Brush3DSettingsModel::SetParentModel(GlobalUIModel *parent)
{
  m_ParentModel = parent;

  // The maximum brush size is shared with the 2D paintbrush tool
  DefaultBehaviorSettings *dbs =
      m_ParentModel->GetGlobalState()->GetDefaultBehaviorSettings();

  m_BrushSizeModel->RebroadcastFromSourceProperty(
        dbs->GetPaintbrushDefaultMaximumSizeModel());
}

Brush3DSettings Brush3DSettingsModel::GetBrush3DSettings()
{
  return m_ParentModel->GetGlobalState()->GetBrush3DSettings();
}

void Brush3DSettingsModel::SetBrush3DSettings(Brush3DSettings bs)
{
  m_ParentModel->GetGlobalState()->SetBrush3DSettings(bs);
  InvokeEvent(ModelUpdateEvent());
}

bool
Brush3DSettingsModel::GetBrushSizeValueAndRange(int &value, NumericValueRange<int> *domain)
{
  Brush3DSettings bs = GetBrush3DSettings();

  // Round just in case
  value = (int)(bs.radius * 2 + 0.5);
  if(domain)
    {
    int max_size = m_ParentModel->GetGlobalPreferencesModel()
                     ->GetDefaultBehaviorSettings()
                     ->GetPaintbrushDefaultMaximumSize();

    domain->Set(1, max_size, 1);
    }
  return true;
}

void Brush3DSettingsModel::SetBrushSizeValue(int value)
{
  Brush3DSettings bs = GetBrush3DSettings();
  bs.radius = 0.5 * value;
  SetBrush3DSettings(bs);
}

bool
Brush3DSettingsModel::GetDepthValueAndRange(int &value, NumericValueRange<int> *domain)
{
  Brush3DSettings bs = GetBrush3DSettings();

  value = bs.depth;
  if(domain)
    {
    domain->Set(-20, 20, 1);
    }
  return true;
}

void Brush3DSettingsModel::SetDepthValue(int value)
{
  Brush3DSettings bs = GetBrush3DSettings();
  bs.depth = value;
  SetBrush3DSettings(bs);
}

bool
Brush3DSettingsModel::GetClosingRadiusValueAndRange(int &value, NumericValueRange<int> *domain)
{
  Brush3DSettings bs = GetBrush3DSettings();

  // Round just in case
  value = (int)(bs.closing_radius + 0.5);
  if(domain)
    {
    domain->Set(1, 8, 1);
    }
  return true;
}

void Brush3DSettingsModel::SetClosingRadiusValue(int value)
{
  Brush3DSettings bs = GetBrush3DSettings();
  bs.closing_radius = value;
  SetBrush3DSettings(bs);
}

bool Brush3DSettingsModel::GetBrushControlsVisibleValue(bool &value)
{
  Brush3DSettings bs = GetBrush3DSettings();
  value = (bs.sub_tool == PAINT3D_BRUSH || bs.sub_tool == PAINT3D_BRIDGE);
  return true;
}

bool Brush3DSettingsModel::GetFillHoleControlsVisibleValue(bool &value)
{
  Brush3DSettings bs = GetBrush3DSettings();
  value = (bs.sub_tool == PAINT3D_FILL_HOLE);
  return true;
}

bool Brush3DSettingsModel::GetIslandControlsVisibleValue(bool &value)
{
  Brush3DSettings bs = GetBrush3DSettings();
  value = (bs.sub_tool == PAINT3D_DELETE_ISLAND);
  return true;
}
