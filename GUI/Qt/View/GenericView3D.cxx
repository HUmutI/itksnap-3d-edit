#include "GenericView3D.h"
#include "Generic3DModel.h"
#include "Generic3DRenderer.h"
#include "GlobalUIModel.h"
#include "GlobalState.h"
#include "vtkGenericRenderWindowInteractor.h"
#include <QEvent>
#include <QMouseEvent>
#include <vtkInteractorStyle.h>
#include <vtkInteractorStyleUser.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkCommand.h>
#include <vtkCallbackCommand.h>
#include <QtVTKInteractionDelegateWidget.h>
#include <vtkPointPicker.h>
#include <vtkRendererCollection.h>
#include <vtkObjectFactory.h>
#include <vtkInteractorStyleSwitch.h>
#include "Window3DPicker.h"
#include "IRISApplication.h"
#include "Brush3DModel.h"
#include <cassert>
#include <string>

class CursorPlacementInteractorStyle : public vtkInteractorStyleTrackballCamera
{
public:
  static CursorPlacementInteractorStyle* New();
  vtkTypeMacro(CursorPlacementInteractorStyle, vtkInteractorStyleTrackballCamera)

  irisGetSetMacro(Model, Generic3DModel *)

  virtual void OnLeftButtonDown() override
  {
    if(!m_Model->PickSegmentationVoxelUnderMouse(
         this->Interactor->GetEventPosition()[0],
         this->Interactor->GetEventPosition()[1]))
      {
      // Forward events
      vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
      }
  }

private:
  Generic3DModel *m_Model;
};

class SpraycanInteractorStyle : public vtkInteractorStyleTrackballCamera
{
public:
  static SpraycanInteractorStyle* New();
  vtkTypeMacro(SpraycanInteractorStyle, vtkInteractorStyleTrackballCamera)

  irisGetSetMacro(Model, Generic3DModel *)

  virtual void OnLeftButtonDown() override
  {
    // Spray a voxel
    if(m_Model->SpraySegmentationVoxelUnderMouse(
         this->Interactor->GetEventPosition()[0],
         this->Interactor->GetEventPosition()[1]))
      {
      m_IsPainting = true;
      }
    else
      {
      // Forward events
      vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
      }
  }

  virtual void OnLeftButtonUp() override
  {
    if(m_IsPainting)
      m_IsPainting = false;
    else
      vtkInteractorStyleTrackballCamera::OnLeftButtonUp();
  }

  virtual void OnMouseMove() override
  {
    if(m_IsPainting)
      m_Model->SpraySegmentationVoxelUnderMouse(
               this->Interactor->GetEventPosition()[0],
               this->Interactor->GetEventPosition()[1]);
    else
      vtkInteractorStyleTrackballCamera::OnMouseMove();
  }

protected:

  SpraycanInteractorStyle() : m_Model(NULL), m_IsPainting(false) {}
  virtual ~SpraycanInteractorStyle() {}

private:
  Generic3DModel *m_Model;
  bool m_IsPainting;
};


class ScalpelInteractorStyle : public vtkInteractorStyleTrackballCamera
{
public:
  static ScalpelInteractorStyle* New();
  vtkTypeMacro(ScalpelInteractorStyle, vtkInteractorStyleTrackballCamera)

  irisGetSetMacro(Model, Generic3DModel *)

  virtual void OnLeftButtonDown() override
  {
    Vector2i xevent(this->Interactor->GetEventPosition());
    switch(m_Model->GetScalpelStatus())
      {
      case Generic3DModel::SCALPEL_LINE_NULL:
        // Holding and dragging the LMB acts as a trackball, only clicking
        // the LMB causes drawing operations
        m_ClickStart = xevent;
        vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
        // m_Model->SetScalpelStartPoint(xevent[0], xevent[1]);
        break;
      case Generic3DModel::SCALPEL_LINE_STARTED:
        m_Model->SetScalpelEndPoint(xevent[0], xevent[1], true);
        break;
      case Generic3DModel::SCALPEL_LINE_COMPLETED:
        vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
        break;
      }
  }

  virtual void OnMouseMove() override
  {
    Vector2i xevent(this->Interactor->GetEventPosition());
    switch(m_Model->GetScalpelStatus())
      {
      case Generic3DModel::SCALPEL_LINE_STARTED:
        m_Model->SetScalpelEndPoint(xevent[0], xevent[1], false);
        break;
      default:
        vtkInteractorStyleTrackballCamera::OnMouseMove();
        break;
      }
 }

  virtual void OnLeftButtonUp() override
  {
    Vector2i xevent(this->Interactor->GetEventPosition());
    Vector2i delta = xevent - m_ClickStart;

    // Detect a click, which starts scalpel drawing
    if(m_Model->GetScalpelStatus() == Generic3DModel::SCALPEL_LINE_NULL)
      {
      if(delta.squared_magnitude() < 4)
        {
        m_Model->SetScalpelStartPoint(xevent[0], xevent[1]);
        }
      }

    vtkInteractorStyleTrackballCamera::OnLeftButtonUp();
  }

  virtual void OnEnter() override
  {
    vtkInteractorStyleTrackballCamera::OnEnter();

    // Record that we're inside
    m_Inside = true;

    // Record the end point
    OnMouseMove();
  }

  virtual void OnLeave() override
  {
    vtkInteractorStyleTrackballCamera::OnLeave();

    // Record that we're inside
    m_Inside = false;
  }

  irisGetMacro(Inside,bool)

protected:

  ScalpelInteractorStyle() : m_Model(NULL) {}
  virtual ~ScalpelInteractorStyle() {}

  Generic3DModel *m_Model;

  Vector2i m_ClickStart;
  bool m_Inside;
};



/**
 * Interactor style for the 3D editing tool (PAINT3D_MODE).
 *
 * All of the logic lives in Brush3DModel; this class only routes mouse and key
 * events and decides what to forward to the trackball camera.
 *
 * Bindings (see also the tooltip on action3DPaint):
 *   left drag                 sub-tool primary (paint)
 *   right drag                erase (brush sub-tool only)
 *   middle drag               rotate camera
 *   shift/ctrl + left drag    pan / spin camera (VTK defaults)
 *   wheel                     dolly (VTK default)
 *   [ ]                       brush radius
 *   , .                       depth
 *   Escape                    cancel a pending bridge endpoint
 *
 * Nothing is bound to Alt and nothing REQUIRES the wheel, because the OSMesa
 * widget backend (QtVTKInteractionDelegateWidget) forwards only Ctrl and Shift
 * and has no wheel handler.
 */
class Brush3DInteractorStyle : public vtkInteractorStyleTrackballCamera
{
public:
  static Brush3DInteractorStyle* New();
  vtkTypeMacro(Brush3DInteractorStyle, vtkInteractorStyleTrackballCamera)

  irisGetSetMacro(Model, Generic3DModel *)

  virtual void OnLeftButtonDown() override
  {
    // Ignore a second button pressed during a gesture. Letting it through would
    // start a new stroke on top of the live one and orphan its undo deltas.
    if(m_ActiveButton != 0)
      return;

    // A modifier always means "camera", so that muscle memory is preserved
    if(this->Interactor->GetShiftKey() || this->Interactor->GetControlKey())
      {
      vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
      return;
      }

    if(this->Push(false))
      m_ActiveButton = 1;
    else
      // Clicking empty space still rotates, exactly like the spray tool
      vtkInteractorStyleTrackballCamera::OnLeftButtonDown();
  }

  virtual void OnLeftButtonUp() override
  {
    if(m_ActiveButton == 1)
      {
      this->Release();
      m_ActiveButton = 0;
      }
    else
      {
      vtkInteractorStyleTrackballCamera::OnLeftButtonUp();
      }
  }

  virtual void OnRightButtonDown() override
  {
    if(m_ActiveButton != 0)
      return;

    // Right-drag erases, mirroring the 2D paintbrush. It steals the trackball's
    // dolly, which the mouse wheel already provides.
    if(this->Interactor->GetShiftKey() || this->Interactor->GetControlKey()
       || !this->IsBrushSubTool())
      {
      vtkInteractorStyleTrackballCamera::OnRightButtonDown();
      return;
      }

    if(this->Push(true))
      m_ActiveButton = 2;
    else
      vtkInteractorStyleTrackballCamera::OnRightButtonDown();
  }

  virtual void OnRightButtonUp() override
  {
    if(m_ActiveButton == 2)
      {
      this->Release();
      m_ActiveButton = 0;
      }
    else
      {
      vtkInteractorStyleTrackballCamera::OnRightButtonUp();
      }
  }

  // Middle button rotates, replacing the trackball's pan (which is still
  // available as shift + left drag)
  virtual void OnMiddleButtonDown() override
  {
    // Rotating mid-stroke would leave the style in VTKIS_ROTATE for the rest of
    // the gesture, so ignore it entirely while painting.
    if(m_ActiveButton != 0)
      return;

    this->GrabFocus(this->EventCallbackCommand);
    this->StartRotate();
  }

  virtual void OnMiddleButtonUp() override
  {
    this->EndRotate();
    if(this->Interactor)
      this->ReleaseFocus();
  }

  virtual void OnMouseMove() override
  {
    Brush3DModel *bm = this->GetBrushModel();
    if(!bm)
      {
      vtkInteractorStyleTrackballCamera::OnMouseMove();
      return;
      }

    if(m_ActiveButton != 0)
      {
      bm->ProcessDragEvent(this->Interactor->GetEventPosition()[0],
                           this->Interactor->GetEventPosition()[1]);
      this->Interactor->Render();
      return;
      }

    // A camera gesture is in progress: do not spend a pick on hovering
    if(this->State != VTKIS_NONE)
      {
      vtkInteractorStyleTrackballCamera::OnMouseMove();
      return;
      }

    bm->ProcessHoverEvent(this->Interactor->GetEventPosition()[0],
                          this->Interactor->GetEventPosition()[1]);
    this->Interactor->Render();
  }

  virtual void OnLeave() override
  {
    if(Brush3DModel *bm = this->GetBrushModel())
      {
      bm->ProcessLeaveEvent();
      this->Interactor->Render();
      }
    vtkInteractorStyleTrackballCamera::OnLeave();
  }

  virtual void OnKeyPress() override
  {
    Brush3DModel *bm = this->GetBrushModel();
    const char *key = this->Interactor->GetKeySym();
    if(!bm || !key)
      {
      vtkInteractorStyleTrackballCamera::OnKeyPress();
      return;
      }

    std::string k(key);
    if(k == "bracketleft")        bm->IncrementBrushSize(-1);
    else if(k == "bracketright")  bm->IncrementBrushSize(+1);
    else if(k == "comma")         bm->IncrementDepth(-1);
    else if(k == "period")        bm->IncrementDepth(+1);
    else if(k == "Escape")        bm->ProcessCancelEvent();
    else
      {
      vtkInteractorStyleTrackballCamera::OnKeyPress();
      return;
      }

    this->Interactor->Render();
  }

protected:

  Brush3DInteractorStyle() : m_Model(NULL), m_ActiveButton(0) {}
  virtual ~Brush3DInteractorStyle() {}

  Brush3DModel *GetBrushModel() const
  {
    return m_Model ? m_Model->GetBrush3DModel() : NULL;
  }

  bool IsBrushSubTool() const
  {
    if(!m_Model)
      return false;
    return m_Model->GetParentUI()->GetGlobalState()
             ->GetBrush3DSettings().sub_tool == PAINT3D_BRUSH;
  }

  bool Push(bool erase)
  {
    Brush3DModel *bm = this->GetBrushModel();
    if(!bm)
      return false;

    bool consumed = bm->ProcessPushEvent(this->Interactor->GetEventPosition()[0],
                                         this->Interactor->GetEventPosition()[1],
                                         erase);
    if(consumed)
      this->Interactor->Render();
    return consumed;
  }

  void Release()
  {
    Brush3DModel *bm = this->GetBrushModel();
    if(!bm)
      return;

    bm->ProcessReleaseEvent(this->Interactor->GetEventPosition()[0],
                            this->Interactor->GetEventPosition()[1]);
    this->Interactor->Render();
  }

private:
  Generic3DModel *m_Model;

  // 0 = none, 1 = left (paint), 2 = right (erase)
  int m_ActiveButton;
};


vtkStandardNewMacro(CursorPlacementInteractorStyle)

vtkStandardNewMacro(Brush3DInteractorStyle)

vtkStandardNewMacro(SpraycanInteractorStyle)

vtkStandardNewMacro(ScalpelInteractorStyle)



GenericView3D::GenericView3D(QWidget *parent) :
    QtVTKRenderWindowBox(parent)
{
  // Create the interactor styles for each mode
  m_InteractionStyle[TRACKBALL_MODE]
      = vtkSmartPointer<vtkInteractorStyleTrackballCamera>::New();

  m_InteractionStyle[CROSSHAIRS_3D_MODE]
      = vtkSmartPointer<CursorPlacementInteractorStyle>::New();

  m_InteractionStyle[SCALPEL_MODE]
      = vtkSmartPointer<ScalpelInteractorStyle>::New();

  m_InteractionStyle[SPRAYPAINT_MODE]
      = vtkSmartPointer<SpraycanInteractorStyle>::New();

  m_InteractionStyle[PAINT3D_MODE]
      = vtkSmartPointer<Brush3DInteractorStyle>::New();
}

GenericView3D::~GenericView3D()
{
}

void GenericView3D::SetModel(Generic3DModel *model)
{
  m_Model = model;

  // Assign the renderer
  this->SetRenderer(m_Model->GetRenderer());

  // Pass the model to the different interactors
  CursorPlacementInteractorStyle::SafeDownCast(
        m_InteractionStyle[CROSSHAIRS_3D_MODE])->SetModel(model);

  SpraycanInteractorStyle::SafeDownCast(
        m_InteractionStyle[SPRAYPAINT_MODE])->SetModel(model);

  ScalpelInteractorStyle::SafeDownCast(
        m_InteractionStyle[SCALPEL_MODE])->SetModel(model);

  Brush3DInteractorStyle::SafeDownCast(
        m_InteractionStyle[PAINT3D_MODE])->SetModel(model);

  // Listen to toolbar changes
  connectITK(m_Model->GetParentUI()->GetGlobalState()->GetToolbarMode3DModel(),
             ValueChangedEvent(), SLOT(onToolbarModeChange()));

  // This should cause the model to redraw when model changes
  connectITK(m_Model, ModelUpdateEvent());

  // Use the current toolbar settings
  this->onToolbarModeChange();
}

void GenericView3D::onToolbarModeChange()
{
  int mode = (int) m_Model->GetParentUI()->GetGlobalState()->GetToolbarMode3D();
  assert(mode >= 0 && mode < TOOLBAR_MODE_3D_COUNT);
  this->GetRenderWindow()->GetInteractor()->SetInteractorStyle(m_InteractionStyle[mode]);
  setMouseTracking(mode == SCALPEL_MODE || mode == PAINT3D_MODE);

  // Leaving the editing tool must take its preview actors and any half-finished
  // gesture with it
  if(mode != PAINT3D_MODE && m_Model->GetBrush3DModel())
    m_Model->GetBrush3DModel()->AbandonGesture();
}

void
GenericView3D::enterEvent(QEnterEvent *ev)
{
  emit mouseEntered();
  QtVTKRenderWindowBox::enterEvent(ev);
}

void
GenericView3D::leaveEvent(QEvent *ev)
{
  emit mouseLeft();
  QtVTKRenderWindowBox::leaveEvent(ev);
}

void
GenericView3D::resizeEvent(QResizeEvent *evt)
{
  emit resized();
  QtVTKRenderWindowBox::resizeEvent(evt);
}
