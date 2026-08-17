#ifndef BRUSH3DTOOLPANEL_H
#define BRUSH3DTOOLPANEL_H

#include <QWidget>
#include "SNAPComponent.h"

class Brush3DSettingsModel;

namespace Ui {
class Brush3DToolPanel;
}

class Brush3DToolPanel : public SNAPComponent
{
  Q_OBJECT

public:
  explicit Brush3DToolPanel(QWidget *parent = 0);
  ~Brush3DToolPanel();

  void SetModel(Brush3DSettingsModel *model);

private slots:

  void on_btnDepthReset_clicked();

private:
  Ui::Brush3DToolPanel *ui;
  Brush3DSettingsModel *m_Model;
};

#endif // BRUSH3DTOOLPANEL_H
