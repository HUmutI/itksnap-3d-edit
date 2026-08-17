#include "Brush3DToolPanel.h"
#include "ui_Brush3DToolPanel.h"

#include "Brush3DSettingsModel.h"
#include "QtRadioButtonCoupling.h"
#include "QtCheckBoxCoupling.h"
#include "QtSpinBoxCoupling.h"
#include "QtSliderCoupling.h"
#include "QtWidgetCoupling.h"

Brush3DToolPanel::Brush3DToolPanel(QWidget *parent) :
  SNAPComponent(parent),
  ui(new Ui::Brush3DToolPanel),
  m_Model(NULL)
{
  ui->setupUi(this);
}

Brush3DToolPanel::~Brush3DToolPanel()
{
  delete ui;
}

void Brush3DToolPanel::SetModel(Brush3DSettingsModel *model)
{
  m_Model = model;

  // Couple the sub-tool radio buttons
  std::map<Paint3DSubTool, QAbstractButton *> rmap_subtool{
    { PAINT3D_BRUSH, ui->radBrush },
    { PAINT3D_DELETE_ISLAND, ui->radDeleteIsland },
    { PAINT3D_BRIDGE, ui->radBridge },
    { PAINT3D_FILL_HOLE, ui->radFillHole }
  };
  makeRadioGroupCoupling(ui->grpSubTool, rmap_subtool, m_Model->GetSubToolModel());

  // Couple the brush shape buttons
  std::map<PaintbrushShape, QAbstractButton *> rmap_shape{
    { PAINTBRUSH_RECTANGULAR, ui->btnSquare },
    { PAINTBRUSH_ROUND, ui->btnRound }
  };
  makeRadioGroupCoupling(ui->grpBrushShape, rmap_shape, m_Model->GetShapeModel());

  // Couple the brush size and depth controls
  makeCoupling(ui->inBrushSizeSlider, model->GetBrushSizeModel());
  makeCoupling(ui->inBrushSizeSpinbox, model->GetBrushSizeModel());

  makeCoupling(ui->inDepthSlider, model->GetDepthModel());
  makeCoupling(ui->inDepthSpinbox, model->GetDepthModel());

  // Couple the other controls
  makeCoupling(ui->chkIsotropic, model->GetIsotropicModel());
  makeCoupling(ui->inClosingRadius, model->GetClosingRadiusModel());
  makeCoupling(ui->chkIslandAnyLabel, model->GetIslandAnyLabelModel());

  // Only show the controls relevant to the active sub-tool
  makeWidgetVisibilityCoupling(ui->grpBrushControls,
                               model->GetBrushControlsVisibleModel());
  makeWidgetVisibilityCoupling(ui->grpFillHoleControls,
                               model->GetFillHoleControlsVisibleModel());
  makeWidgetVisibilityCoupling(ui->grpIslandControls,
                               model->GetIslandControlsVisibleModel());
}

void Brush3DToolPanel::on_btnDepthReset_clicked()
{
  if(m_Model)
    m_Model->GetDepthModel()->SetValue(0);
}
