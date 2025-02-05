/* Copyright (C) 2015 The Android Open Source Project
 **
 ** This software is licensed under the terms of the GNU General Public
 ** License version 2, as published by the Free Software Foundation, and
 ** may be copied, distributed, and modified under those terms.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 */

#include "android/skin/qt/tool-window.h"
#include <stdint.h>
#include <stdio.h>
#include <cassert>
#include <istream>
#include <memory>
#include <string>
#include <string_view>

#include <QAbstractButton>
#include <QApplication>
#include <QBitmap>
#include <QByteArray>
#include <QClipboard>
#include <QCloseEvent>
#include <QEvent>
#include <QFlags>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QString>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <QtCore>

#include "aemu/base/logging/Log.h"
#include "aemu/base/logging/Log.h"
#include "aemu/base/logging/LogSeverity.h"
#include "aemu/base/memory/OnDemand.h"
#include "android/avd/info.h"
#include "android/avd/util.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/emulation/control/adb/AdbInterface.h"
#include "android/emulation/control/clipboard_agent.h"
#include "android/emulation/control/sensors_agent.h"
#include "android/emulator-window.h"
#include "android/hw-events.h"
#include "android/hw-sensors.h"
#include "android/metrics/MetricsReporter.h"
#include "android/metrics/MetricsWriter.h"
#include "android/physics/Physics.h"
#include "android/skin/android_keycodes.h"
#include "android/skin/event.h"
#include "android/skin/linux_keycodes.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/extended-pages/common.h"
#include "android/skin/qt/extended-pages/virtual-sensors-page.h"
#include "android/skin/qt/extended-window-grpc.h"
#include "android/skin/qt/extended-window.h"
#include "android/skin/qt/posture-selection-dialog.h"
#include "android/skin/qt/qt-settings.h"
#include "android/skin/qt/qt-ui-commands.h"
#include "android/skin/qt/stylesheet.h"
#include "android/skin/qt/virtualscene-control-window.h"
#include "android/ui-emu-agent.h"
#include "android/utils/debug.h"
#include "android/utils/system.h"
#include "host-common/FeatureControl.h"
#include "host-common/Features.h"
#include "host-common/misc.h"
#include "host-common/hw-config-helper.h"
#include "host-common/screen-recorder.h"
#include "host-common/window_agent.h"
#include "snapshot/common.h"
#include "snapshot/interface.h"
#include "studio_stats.pb.h"
#include "host-common/opengles.h"
#include "ui_tools.h"

namespace {

void ChangeIcon(QPushButton* button, const char* icon, const char* tip) {
    button->setIcon(getIconForCurrentTheme(icon));
    button->setProperty("themeIconName", icon);
    button->setProperty("toolTip", tip);
}

}  // namespace

using android::base::System;
using android::base::WorkerProcessingResult;
using android::emulation::OptionalAdbCommandResult;
using android::metrics::MetricsReporter;
using Ui::Settings::SaveSnapshotOnExit;

namespace proto = android_studio;
namespace fc = android::featurecontrol;
using fc::Feature;

template <typename T>
ToolWindow::WindowHolder<T>::WindowHolder(ToolWindow* tw,
                                          OnCreatedCallback onCreated)
    : mWindow(new T(tw->mEmulatorWindow, tw)) {
    (tw->*onCreated)(mWindow);
}

template <typename T>
ToolWindow::WindowHolder<T>::~WindowHolder() {
    // The window may have slots with subscribers, so use deleteLater() instead
    // of a regular delete for it.
    mWindow->deleteLater();
}

ToolWindow::ExtendedWindowHolder::ExtendedWindowHolder(
        ToolWindow* tw,
        OnCreatedCallback onCreated) {
    bool grpc = getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui;
    if (grpc) {
        dwarning("Using experimental gRPC UI, not yet stable!");
        mWindow = new ExtendedWindowGrpc(tw->mEmulatorWindow, tw);
    } else {
        mWindow = new ExtendedWindow(tw->mEmulatorWindow, tw);
    }
    (tw->*onCreated)(mWindow);
}

const UiEmuAgent* ToolWindow::sUiEmuAgent = nullptr;
static ToolWindow* sToolWindow = nullptr;

ToolWindow::ToolWindow(EmulatorQtWindow* window,
                       QWidget* parent,
                       ToolWindow::UIEventRecorderPtr event_recorder,
                       ToolWindow::UserActionsCounterPtr user_actions_counter)
    : QFrame(parent),
      mEmulatorWindow(window),
      mExtendedWindow(this, &ToolWindow ::onExtendedWindowCreated),
      mVirtualSceneControlWindow(this,
                                 &ToolWindow::onVirtualSceneWindowCreated),
      mToolsUi(new Ui::ToolControls),
      mUIEventRecorder(event_recorder),
      mUserActionsCounter(user_actions_counter),
      mPostureSelectionDialog(new PostureSelectionDialog(this)),
      mResizableDialog(new ResizableDialog(this)),
      mFoldableSyncToAndroidSuccess(false),
      mFoldableSyncToAndroidTimeout(false),
      mFoldableSyncToAndroid([this](FoldableSyncToAndroidItem&& item) {
          return foldableSyncToAndroidItemFunction(item);
      }) {
// "Tool" type windows live in another layer on top of everything in OSX, which
// is undesirable because it means the extended window must be on top of the
// emulator window. However, on Windows and Linux, "Tool" type windows are the
// only way to make a window that does not have its own taskbar item.
#ifdef __APPLE__
    Qt::WindowFlags flag = Qt::Dialog;
#else
    Qt::WindowFlags flag = Qt::Tool;
#endif
    setWindowFlags(flag | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    mToolsUi->setupUi(this);

    mToolsUi->mainLayout->setAlignment(Qt::AlignCenter);
    mToolsUi->winButtonsLayout->setAlignment(Qt::AlignCenter);
    mToolsUi->controlsLayout->setAlignment(Qt::AlignCenter);
    if (android_foldable_any_folded_area_configured() ||
        android_foldable_hinge_configured() || resizableEnabled()) {
        mToolsUi->zoom_button->hide();
        mToolsUi->zoom_button->setEnabled(false);
    }
    // Get the latest user selections from the user-config code.
    SettingsTheme theme = getSelectedTheme();
    adjustAllButtonsForTheme(theme);
    updateTheme(Ui::stylesheetForTheme(theme));

    QString default_shortcuts =
            "Ctrl+Shift+A SHOW_PANE_CAMERA\n"
            "Ctrl+Shift+U SHOW_PANE_BUGREPORT\n"
            "Ctrl+Shift+M SHOW_PANE_MICROPHONE\n"
            "Ctrl+Shift+N SHOW_PANE_SNAPSHOT\n"
            "Ctrl+Shift+S SHOW_PANE_SETTINGS\n"
#ifdef __APPLE__
            "Ctrl+/     SHOW_PANE_HELP\n"
#else
            "F1         SHOW_PANE_HELP\n"
#endif
            "Ctrl+S     TAKE_SCREENSHOT\n"
            "Ctrl+Shift+Up    PAN_UP\n"
            "Ctrl+Shift+Down  PAN_DOWN\n"
            "Ctrl+Shift+Left  PAN_LEFT\n"
            "Ctrl+Shift+Right PAN_RIGHT\n"
            "Ctrl+P     POWER\n"
            "Ctrl+M     MENU\n"
            "Ctrl+T     TOGGLE_TRACKBALL\n"
#ifndef __APPLE__
            "Ctrl+H     HOME\n"
#else
            "Ctrl+Shift+H  HOME\n"
#endif
            "Ctrl+O     OVERVIEW\n"
            "Ctrl+Backspace BACK\n";

    if (!getConsoleAgents()->settings->avdInfo() ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) !=
                AVD_WEAR) {
        default_shortcuts += "Ctrl+=     VOLUME_UP\n";
        default_shortcuts += "Ctrl+-     VOLUME_DOWN\n";
        default_shortcuts += "Ctrl+Left ROTATE_LEFT\n";
        default_shortcuts += "Ctrl+Right ROTATE_RIGHT\n";
    }

    if (!android_foldable_any_folded_area_configured() &&
        !android_foldable_hinge_configured() &&
        !android_foldable_rollable_configured() && !resizableEnabled()) {
        // Zoom is not available for foldable and resizable AVDs
        default_shortcuts += "Ctrl+Z    ENTER_ZOOM\n";
        default_shortcuts += "Ctrl+Up   ZOOM_IN\n";
        default_shortcuts += "Ctrl+Down ZOOM_OUT\n";
    }
    if (fc::isEnabled(fc::PlayStoreImage) &&
        getConsoleAgents()->settings->hw()->PlayStore_enabled) {
        default_shortcuts += "Ctrl+Shift+G SHOW_PANE_GPLAY\n";
    }
    if (fc::isEnabled(fc::ScreenRecording)) {
        default_shortcuts += "Ctrl+Shift+R SHOW_PANE_RECORD\n";
    }

    if (getConsoleAgents()->settings->avdInfo()) {
        if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
            AVD_WEAR) {
            if (avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) >=
                28) {
                default_shortcuts += "Ctrl+Shift+O WEAR_1\n";
                default_shortcuts += "Ctrl+Shift+T TILT\n";
                default_shortcuts += "Ctrl+Shift+E PALM\n";
            }
            if (avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) >
                28) {
                default_shortcuts += "Ctrl+Shift+I WEAR_2\n";
            }
        } else if (avdInfo_getAvdFlavor(
                           getConsoleAgents()->settings->avdInfo()) ==
                   AVD_ANDROID_AUTO) {
            default_shortcuts += "Ctrl+Shift+T SHOW_PANE_CAR\n";
            default_shortcuts += "Ctrl+Shift+O SHOW_PANE_CAR_ROTARY\n";
            default_shortcuts += "Ctrl+Shift+I SHOW_PANE_SENSOR_REPLAY\n";
        } else if (android::featurecontrol::isEnabled(
                           android::featurecontrol::MultiDisplay) &&
                   !android_foldable_any_folded_area_configured() &&
                   !android_foldable_hinge_configured() &&
                   !android_foldable_rollable_configured() &&
                   !resizableEnabled()) {
            // Multi display pane should not be available on cars.
            // Multi display pane should not be available on foldable
            // or resizable AVDs
            default_shortcuts += "Ctrl+Shift+M SHOW_PANE_MULTIDISPLAY\n";
        }
    }

    if (fc::isEnabled(fc::TvRemote) &&
        getConsoleAgents()->settings->avdInfo() &&
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_TV) {
        default_shortcuts += "Ctrl+Shift+D SHOW_PANE_TV_REMOTE\n";
    } else {
        default_shortcuts += "Ctrl+Shift+D SHOW_PANE_DPAD\n";
    }

    if (!getConsoleAgents()->settings->avdInfo() ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) !=
                AVD_TV) {
        default_shortcuts +=
                "Ctrl+Shift+L SHOW_PANE_LOCATION\n"
                "Ctrl+Shift+C SHOW_PANE_CELLULAR\n"
                "Ctrl+Shift+B SHOW_PANE_BATTERY\n"
                "Ctrl+Shift+P SHOW_PANE_PHONE\n"
                "Ctrl+Shift+V SHOW_PANE_VIRTSENSORS\n"
                "Ctrl+Shift+F SHOW_PANE_FINGER\n";
    }

    QTextStream stream(&default_shortcuts);
    mShortcutKeyStore.populateFromTextStream(stream, parseQtUICommand);
    // Need to add this one separately because QKeySequence cannot parse
    // the string "Ctrl".
    mShortcutKeyStore.add(QKeySequence(Qt::Key_Control | Qt::ControlModifier),
                          QtUICommand::SHOW_MULTITOUCH);

    VirtualSceneControlWindow::addShortcutKeysToKeyStore(mShortcutKeyStore);

    // Update tool tips on all push buttons.
    const QList<QPushButton*> childButtons =
            findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly);
    for (auto button : childButtons) {
        QVariant uiCommand = button->property("uiCommand");
        if (uiCommand.isValid()) {
            QtUICommand cmd;
            if (parseQtUICommand(uiCommand.toString(), &cmd)) {
                QVector<QKeySequence>* shortcuts =
                        mShortcutKeyStore.reverseLookup(cmd);
                if (shortcuts && shortcuts->length() > 0) {
                    button->setToolTip(getQtUICommandDescription(cmd) + " (" +
                                       shortcuts->at(0).toString(
                                               QKeySequence::NativeText) +
                                       ")");
                }
            }
        } else if (button != mToolsUi->close_button &&
                   button != mToolsUi->minimize_button &&
                   button != mToolsUi->more_button) {
            // Almost all toolbar buttons are required to have a uiCommand
            // property.
            // Unfortunately, we have no way of enforcing it at compile time.
            assert(0);
        }
    }

    if (getConsoleAgents()->settings->hw()->hw_arc) {
        // Chrome OS doesn't support rotation now.
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
    } else {
        // Android doesn't support tablet mode now.
        mToolsUi->tablet_mode_button->setHidden(true);
    }

    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_TV) {
        // Android TV should not rotate
        // TODO: emulate VESA mounts for use with
        // vertically scrolling arcade games
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
    }

    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_WEAR) {
        // Wear OS shouldn't get rotate nor volume up/down buttons.
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->volume_up_button->setHidden(true);
        mToolsUi->volume_down_button->setHidden(true);
    }

    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_WEAR &&
        avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) >= 28) {
        // Use new button layout for >= API 28 wear emulators
        mToolsUi->overview_button->setHidden(true);
        mToolsUi->power_button->setHidden(true);
        mToolsUi->home_button->setHidden(true);

        mToolsUi->controlsLayout->removeWidget(mToolsUi->back_button);
        mToolsUi->controlsLayout->insertWidget(0, mToolsUi->back_button);

        if (avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) ==
            28) {
            mToolsUi->wear_button_2->setHidden(true);
        }
    } else {
        mToolsUi->wear_button_1->setHidden(true);
        mToolsUi->wear_button_2->setHidden(true);
        mToolsUi->palm_button->setHidden(true);
        mToolsUi->tilt_button->setHidden(true);
    }

    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_ANDROID_AUTO) {
        // Android Auto doesn't support rotate, home, back, recent
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->back_button->setHidden(true);
        mToolsUi->overview_button->setHidden(true);
    }

    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_DESKTOP) {
        // Desktop device does not rotate
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
    }

    if (getConsoleAgents()->settings->android_cmdLineOptions()->fuchsia) {
        // These don't apply to Fuchsia
        mToolsUi->prev_layout_button->setHidden(true);
        mToolsUi->next_layout_button->setHidden(true);
        mToolsUi->back_button->setHidden(true);
        mToolsUi->home_button->setHidden(true);
        mToolsUi->overview_button->setHidden(true);
    }

#ifndef Q_OS_MAC
    // Swap minimize and close buttons on non-apple OSes
    auto closeBtn = mToolsUi->winButtonsLayout->takeAt(0);
    mToolsUi->winButtonsLayout->insertItem(1, closeBtn);
#endif
    // How big is the toolbar naturally? This is just tall enough
    // to show all the buttons, so we should never shrink it
    // smaller than this.

    if (android_foldable_hinge_configured() ||
        android_foldable_folded_area_configured(0) ||
        android_foldable_rollable_configured()) {
        mFoldableSyncToAndroid.start();
    } else {
        mToolsUi->change_posture_button->setHidden(true);
    }
    updateFoldableButtonVisibility();

    connect(mPostureSelectionDialog, SIGNAL(newPostureRequested(int)), this,
            SLOT(on_new_posture_requested(int)));
    connect(mPostureSelectionDialog, SIGNAL(finished(int)), this,
            SLOT(on_dismiss_posture_selection_dialog()));
    connect(mResizableDialog,
            SIGNAL(newResizableRequested(PresetEmulatorSizeType)), this,
            SLOT(on_new_resizable_requested(PresetEmulatorSizeType)));
    connect(mResizableDialog, SIGNAL(finished(int)), this,
            SLOT(on_dismiss_resizable_dialog()));

    if (!resizableEnabled()) {
        mToolsUi->resizable_button->setHidden(true);
    } else {
        resizableChangeIcon(getResizableActiveConfigId());
    }

    sToolWindow = this;
    skin_winsys_touch_qt_extended_virtual_sensors();

    remaskButtons();
    installEventFilter(this);

    mSleepTimer.setSingleShot(true);
    connect(&mSleepTimer, SIGNAL(timeout()), this, SLOT(on_sleep_timer_done()));
    mUnfoldTimer.setSingleShot(true);
    connect(&mUnfoldTimer, SIGNAL(timeout()), this, SLOT(on_unfold_timer_done()));
}

ToolWindow::~ToolWindow() {
    mFoldableSyncToAndroid.enqueue({
            SEND_EXIT,
    });
    mFoldableSyncToAndroid.join();
}

void ToolWindow::startSleepTimer() {
    mSleepTimer.start(2000); // 2 second
}

void ToolWindow::startUnfoldTimer(PresetEmulatorSizeType newSize) {
    mDesiredNewSize = newSize;
    on_new_posture_requested(POSTURE_OPENED);
    mUnfoldTimer.start(2000);
}

void ToolWindow::on_sleep_timer_done() {
    if (emugl::shouldSkipDraw()) {
        emugl::setShouldSkipDraw(false);
        android_redrawOpenglesWindow();
    }
    if (isResizableTransitionInProgress()) {
        setResizableTransitionInProgress(false);
        updateFoldableButtonVisibility();
    }
    if (mSleepKeySent) {
        mEmulatorWindow->getAdbInterface()->
            enqueueCommand( {"shell", "input", "keyevent", "KEYCODE_WAKEUP"});
    }
}

void ToolWindow::on_unfold_timer_done() {
    on_new_resizable_requested(mDesiredNewSize);
}

void ToolWindow::updateFoldableButtonVisibility() {
    mToolsUi->change_posture_button->setEnabled(
            android_foldable_hinge_enabled());
    mExtendedWindow.ifExists([&] {
        mExtendedWindow.get()->getVirtualSensorsPage()->updateHingeSensorUI(); });
}

void ToolWindow::updateButtonUiCommand(QPushButton* button,
                                       const char* uiCommand) {
    button->setProperty("uiCommand", uiCommand);
    QtUICommand cmd;
    if (parseQtUICommand(QString(uiCommand), &cmd)) {
        QVector<QKeySequence>* shortcuts = mShortcutKeyStore.reverseLookup(cmd);
        if (shortcuts && shortcuts->length() > 0) {
            button->setToolTip(
                    getQtUICommandDescription(cmd) + " (" +
                    shortcuts->at(0).toString(QKeySequence::NativeText) + ")");
        }
    }
}

void ToolWindow::raise() {
    if (mVirtualSceneControlWindow.hasInstance() &&
        mVirtualSceneControlWindow.get()->isActive()) {
        mVirtualSceneControlWindow.get()->raise();
    }
    if (mTopSwitched) {
        mExtendedWindow.ifExists([&] {
            mExtendedWindow.get()->raise();
            mExtendedWindow.get()->activateWindow();
        });
        mTopSwitched = false;
    }
}

void ToolWindow::allowExtWindowCreation() {
    mAllowExtWindow = true;
}

void ToolWindow::switchClipboardSharing(bool enabled) {
    if (sUiEmuAgent && sUiEmuAgent->clipboard) {
        sUiEmuAgent->clipboard->setEnabled(enabled);
    }
}

void ToolWindow::ensureVirtualSceneWindowCreated() {
    // Creates the virtual scene control window if it does not exist.
    mVirtualSceneControlWindow.get();
}

void ToolWindow::showVirtualSceneControls(bool show) {
    mVirtualSceneControlWindow.get()->setActiveForCamera(show);
}

void ToolWindow::onExtendedWindowCreated(ExtendedBaseWindow* extendedWindow) {
    if (auto userActionsCounter = mUserActionsCounter.lock()) {
        userActionsCounter->startCountingForExtendedWindow(extendedWindow);
    }

    setupSubwindow(extendedWindow);

    mVirtualSceneControlWindow.ifExists([&] {
        extendedWindow->connectVirtualSceneWindow(
                mVirtualSceneControlWindow.get().get());
    });
}

void ToolWindow::onVirtualSceneWindowCreated(
        VirtualSceneControlWindow* virtualSceneWindow) {
    if (auto userActionsCounter = mUserActionsCounter.lock()) {
        userActionsCounter->startCountingForVirtualSceneWindow(
                virtualSceneWindow);
    }

    setupSubwindow(virtualSceneWindow);

    mExtendedWindow.ifExists([&] {
        mExtendedWindow.get()->connectVirtualSceneWindow(virtualSceneWindow);
    });
}

void ToolWindow::setupSubwindow(QWidget* window) {
    if (auto recorderPtr = mUIEventRecorder.lock()) {
        recorderPtr->startRecording(window);
    }

    // If window is created before it's activated (for example, before the "..."
    // button is pressed for the extended window) it should be hid until it is
    // actually activated.
    window->hide();
}

void ToolWindow::hide() {
    QFrame::hide();
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->hide(); });
    hideExtendedWindow();
}

void ToolWindow::closeEvent(QCloseEvent* ce) {
    ce->ignore();
    on_close_button_clicked();
}

void ToolWindow::mousePressEvent(QMouseEvent* event) {
    raiseMainWindow();
    QFrame::mousePressEvent(event);
}

void ToolWindow::hideEvent(QHideEvent*) {
    mIsExtendedWindowVisibleOnShow = mExtendedWindow.hasInstance() &&
                                     mExtendedWindow.get()->isVisible() &&
                                     mExtendedWindow.get()->isActiveWindow();
    mIsVirtualSceneWindowVisibleOnShow =
            mVirtualSceneControlWindow.hasInstance() &&
            mVirtualSceneControlWindow.get()->isVisible();
}

void ToolWindow::show() {
    QFrame::show();
    setFixedSize(size());

    if (mIsVirtualSceneWindowVisibleOnShow) {
        mVirtualSceneControlWindow.get()->show();
    }

    if (mIsExtendedWindowVisibleOnShow) {
    mExtendedWindow.ifExists([&] { mExtendedWindow.get()->show(); });
    }
}

bool ToolWindow::needExtendedWindow(QtUICommand cmd) const {
    switch (cmd) {
        case QtUICommand::HOME:
        case QtUICommand::BACK:
        case QtUICommand::OVERVIEW:
        case QtUICommand::POWER:
        case QtUICommand::TAKE_SCREENSHOT:
        case QtUICommand::VOLUME_DOWN:
        case QtUICommand::VOLUME_UP:
            return false;
        default:
            return true;
    }
    return true;
}

void ToolWindow::ensureExtendedWindowExists() {
    if (mAllowExtWindow && !mIsExiting) {
        mExtendedWindow.get();
    }
}

bool ToolWindow::setUiTheme(SettingsTheme theme, bool persist) {
    if (theme < 0 || theme >= SETTINGS_THEME_NUM_ENTRIES) {
        // Out of range--ignore
        return false;
    }
    if (getSelectedTheme() != theme) {
        setSelectedTheme(theme, persist);
        ensureExtendedWindowExists();
        emit(themeChanged(theme));
    }
    return true;
}

void ToolWindow::showExtendedWindow(ExtendedWindowPane pane) {
    ensureExtendedWindowExists();
    if (pane == PANE_IDX_UNKNOWN) {
        on_more_button_clicked();
    } else {
        showOrRaiseExtendedWindow(pane);
    }
}

void ToolWindow::waitForExtendedWindowVisibility(bool visible) {
    if (!visible && !mExtendedWindow.hasInstance()) {
        // No extended window and we are wating to be hidden..
        // so no need to wait
        return;
    }
    ensureExtendedWindowExists();
    mExtendedWindow.get()->waitForVisibility(visible);
}

void ToolWindow::hideExtendedWindow() {
    mExtendedWindow.ifExists([&] { mExtendedWindow.get()->hide(); });
}

void ToolWindow::handleUICommand(QtUICommand cmd,
                                 bool down,
                                 std::string extra) {
    // Many UI commands require the extended window
    if (needExtendedWindow(cmd)) {
        ensureExtendedWindowExists();
    }

    switch (cmd) {
        case QtUICommand::SHOW_PANE_LOCATION:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_LOCATION);
            }
            break;
        case QtUICommand::SHOW_PANE_CELLULAR:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CELLULAR);
            }
            break;
        case QtUICommand::SHOW_PANE_BATTERY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_BATTERY);
            }
            break;
        case QtUICommand::SHOW_PANE_CAMERA:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CAMERA);
            }
            break;
        case QtUICommand::SHOW_PANE_BUGREPORT:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_BUGREPORT);
            }
            break;
        case QtUICommand::SHOW_PANE_PHONE:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_TELEPHONE);
            }
            break;
        case QtUICommand::SHOW_PANE_MICROPHONE:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_MICROPHONE);
            }
            break;
        case QtUICommand::SHOW_PANE_VIRTSENSORS:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_VIRT_SENSORS);
            }
            break;
        case QtUICommand::SHOW_PANE_SNAPSHOT:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_SNAPSHOT);
            }
            break;
        case QtUICommand::SHOW_PANE_DPAD:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_DPAD);
            }
            break;
        case QtUICommand::SHOW_PANE_TV_REMOTE:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_TV_REMOTE);
            }
            break;
        case QtUICommand::SHOW_PANE_FINGER:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_FINGER);
            }
            break;
        case QtUICommand::SHOW_PANE_GPLAY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_GOOGLE_PLAY);
            }
            break;
        case QtUICommand::SHOW_PANE_RECORD:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_RECORD);
            }
            break;
        case QtUICommand::SHOW_PANE_CAR:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CAR);
            }
            break;
        case QtUICommand::SHOW_PANE_CAR_ROTARY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_CAR_ROTARY);
            }
            break;
        case QtUICommand::SHOW_PANE_MULTIDISPLAY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_MULTIDISPLAY);
            }
            break;
        case QtUICommand::SHOW_PANE_SETTINGS:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_SETTINGS);
            }
            break;
        case QtUICommand::SHOW_PANE_HELP:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_HELP);
            }
            break;
        case QtUICommand::TAKE_SCREENSHOT:
            if (down) {
                mEmulatorWindow->screenshot();
            }
            break;
        case QtUICommand::ENTER_ZOOM:
            if (down) {
                mEmulatorWindow->toggleZoomMode();
            }
            mToolsUi->zoom_button->setChecked(mEmulatorWindow->isInZoomMode());
            break;
        case QtUICommand::ZOOM_IN:
            if (down) {
                if (mEmulatorWindow->isInZoomMode()) {
                    mEmulatorWindow->zoomIn();
                } else {
                    mEmulatorWindow->scaleUp();
                }
            }
            break;
        case QtUICommand::ZOOM_OUT:
            if (down) {
                if (mEmulatorWindow->isInZoomMode()) {
                    mEmulatorWindow->zoomOut();
                } else {
                    mEmulatorWindow->scaleDown();
                }
            }
            break;
        case QtUICommand::PAN_UP:
            if (down) {
                mEmulatorWindow->panVertical(true);
            }
            break;
        case QtUICommand::PAN_DOWN:
            if (down) {
                mEmulatorWindow->panVertical(false);
            }
            break;
        case QtUICommand::PAN_LEFT:
            if (down) {
                mEmulatorWindow->panHorizontal(true);
            }
            break;
        case QtUICommand::PAN_RIGHT:
            if (down) {
                mEmulatorWindow->panHorizontal(false);
            }
            break;
        case QtUICommand::VOLUME_UP:
            forwardKeyToEmulator(LINUX_KEY_VOLUMEUP, down);
            break;
        case QtUICommand::VOLUME_DOWN:
            forwardKeyToEmulator(LINUX_KEY_VOLUMEDOWN, down);
            break;
        case QtUICommand::POWER:
            forwardKeyToEmulator(LINUX_KEY_POWER, down);
            break;
        case QtUICommand::TABLET_MODE:
            if (getConsoleAgents()->settings->hw()->hw_arc) {
                forwardGenericEventToEmulator(EV_SW, SW_TABLET_MODE, down);
                forwardGenericEventToEmulator(EV_SYN, 0, 0);
            }
            break;
        case QtUICommand::MENU:
            forwardKeyToEmulator(LINUX_KEY_SOFT1, down);
            break;
        case QtUICommand::HOME:
            forwardKeyToEmulator(LINUX_KEY_HOME, down);
            break;
        case QtUICommand::BACK:
            forwardKeyToEmulator(LINUX_KEY_BACK, down);
            break;
        case QtUICommand::OVERVIEW:
            forwardKeyToEmulator(ANDROID_KEY_APPSWITCH, down);
            break;
        case QtUICommand::WEAR_1:
            forwardKeyToEmulator(LINUX_KEY_HOME, down);
            break;
        case QtUICommand::WEAR_2:
            forwardKeyToEmulator(LINUX_KEY_POWER, down);
            break;
        case QtUICommand::PALM:
            forwardKeyToEmulator(LINUX_KEY_SLEEP, down);
            break;
        case QtUICommand::TILT:
            if (down) {
                float tilt = 1.0f;
                sUiEmuAgent->sensors->setPhysicalParameterTarget(
                        PHYSICAL_PARAMETER_WRIST_TILT, &tilt, 1,
                        PHYSICAL_INTERPOLATION_SMOOTH);
            }
            break;
        case QtUICommand::ROTATE_RIGHT:
        case QtUICommand::ROTATE_LEFT:
            if (down) {
                emulator_window_rotate_90(cmd == QtUICommand::ROTATE_RIGHT);
            }
            break;
        case QtUICommand::SHOW_PANE_SENSOR_REPLAY:
            if (down) {
                showOrRaiseExtendedWindow(PANE_IDX_SENSOR_REPLAY);
            }
            break;
        case QtUICommand::TOGGLE_TRACKBALL:
            if (down) {
                mEmulatorWindow->queueSkinEvent(createSkinEvent(kEventToggleTrackball));
            }
            break;
        case QtUICommand::CHANGE_FOLDABLE_POSTURE:
            if (down && mLastRequestedFoldablePosture != -1) {
                float posture =
                        static_cast<float>(mLastRequestedFoldablePosture);
                sUiEmuAgent->sensors->setPhysicalParameterTarget(
                        PHYSICAL_PARAMETER_POSTURE, &posture, 1,
                        PHYSICAL_INTERPOLATION_SMOOTH);
            }
            break;
        case QtUICommand::UPDATE_FOLDABLE_POSTURE_INDICATOR:
            if (down) {
                float posture = 0;
                float* out = &posture;
                sUiEmuAgent->sensors->getPhysicalParameter(
                        PHYSICAL_PARAMETER_POSTURE, &out, 1,
                        PARAMETER_VALUE_TYPE_CURRENT);
                applyFoldableQuirk((enum FoldablePostures)posture);
                switch ((enum FoldablePostures)posture) {
                    case POSTURE_OPENED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_open", "Change posture");
                        break;
                    case POSTURE_CLOSED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_closed", "Change posture");
                        break;
                    case POSTURE_HALF_OPENED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_half-open", "Change posture");
                        break;
                    case POSTURE_FLIPPED:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_flipped", "Change posture");
                        break;
                    case POSTURE_TENT:
                        ChangeIcon(mToolsUi->change_posture_button,
                                   "posture_tent", "Change posture");
                        break;
                    default:;
                }
                const bool is_pixel_fold = android_foldable_is_pixel_fold();
                if (is_pixel_fold) {
                    mEmulatorWindow->resizeAndChangeAspectRatio(android_foldable_is_folded());
                } else {
                    if (android_foldable_is_folded()) {
                        int xOffset, yOffset, width, height;
                        if (android_foldable_get_folded_area(&xOffset, &yOffset,
                                                             &width, &height)) {
                            mEmulatorWindow->resizeAndChangeAspectRatio(true);
                            if (android_foldable_rollable_configured()) {
                                // rollable has up to 3 folded-areas, need
                                // guarntee the folded-area are updated in
                                // window manager before sending LID_CLOSE
                                mFoldableSyncToAndroid.enqueue({
                                        SEND_LID_OPEN,
                                });
                                mFoldableSyncToAndroid.enqueue(
                                        {SEND_AREA, xOffset, yOffset, width,
                                         height});
                                mFoldableSyncToAndroid.enqueue(
                                        {CONFIRM_AREA, xOffset, yOffset, width,
                                         height});
                            } else {
                                // hinge or legacy foldable has only one folded
                                // area. Once configured, no need to configure
                                // again. Unless explicitly required, e.g., in
                                // case of rebooting Android itselft only.
                                if (!mFoldableSyncToAndroidSuccess ||
                                    extra == "confirmFoldedArea") {
                                    mFoldableSyncToAndroid.enqueue(
                                            {SEND_AREA, xOffset, yOffset, width,
                                             height});
                                    mFoldableSyncToAndroid.enqueue(
                                            {CONFIRM_AREA, xOffset, yOffset,
                                             width, height});
                                } else {
                                    mFoldableSyncToAndroid.enqueue({
                                            SEND_LID_CLOSE,
                                    });
                                }
                            }
                        }
                    } else {
                        mEmulatorWindow->resizeAndChangeAspectRatio(false);
                        mFoldableSyncToAndroid.enqueue({
                                SEND_LID_OPEN,
                        });
                    }
                }
            }

            break;
        case QtUICommand::SHOW_MULTITOUCH:
        // Multitouch is handled in EmulatorQtWindow, and doesn't
        // really need an element in the QtUICommand enum. This
        // enum element exists solely for the purpose of displaying
        // it in the list of keyboard shortcuts in the Help page.
        default:;
    }
}

void ToolWindow::presetSizeAdvance(PresetEmulatorSizeType newSize) {
    if (!resizableEnabled()) {
        return;
    }
    if (getResizableActiveConfigId() == newSize) {
        return;
    }
    if (isResizableTransitionInProgress()) {
        return;
    }
    if (android_foldable_is_folded()) {
        startUnfoldTimer(newSize);
        return;
    }
    PresetEmulatorSizeInfo info;
    if (!getResizableConfig(newSize, &info)) {
        LOG(ERROR) << "Failed to get size information of resizable type "
                   << newSize;
        return;
    }

    // If folded, we need to unfold it first, otherwise it will mess up
    // non-foldable devices.
    if (getResizableActiveConfigId() == PRESET_SIZE_UNFOLDED &&
        mLastRequestedFoldablePosture == POSTURE_CLOSED) {
        // Directly call posture change function without going through emit,
        // because we want it to be processed before the resizable event.
        on_new_posture_requested(POSTURE_OPENED);
    }

    setResizableTransitionInProgress(true);
    emugl::setShouldSkipDraw(true);
    startSleepTimer();
    std::string updateMsg = "Updating device size\n";
    switch (info.type) {
        case PRESET_SIZE_PHONE:
            updateMsg += "Phone\n";
            break;
        case PRESET_SIZE_UNFOLDED:
            updateMsg += "Unfolded\n";
            break;
        case PRESET_SIZE_TABLET:
            updateMsg += "Tablet\n";
            break;
        case PRESET_SIZE_DESKTOP:
            updateMsg += "Desktop\n";
            break;
        default:;
    }
    updateMsg += std::to_string(info.width) + " x " +
                 std::to_string(info.height) + " dp\n" +
                 std::to_string(info.dpi) + " dpi";

    LOG(INFO) << "Resizable: change to new size: " << newSize;
    resizableChangeIcon(newSize);
    SkinEvent skin_event = createSkinEvent(kEventSetDisplayActiveConfig);
    skin_event.u.display_active_config = static_cast<int>(newSize);
    mEmulatorWindow->queueSkinEvent(std::move(skin_event));
    if (!resizableEnabled34()) {
        mEmulatorWindow->resizeAndChangeAspectRatio(0, 0, info.width,
                                                    info.height);
    }
    sUiEmuAgent->window->showMessage(updateMsg.c_str(), WINDOW_MESSAGE_GENERIC,
                                     3000);
    if (resizableEnabled34()) {
        mToolsUi->change_posture_button->setEnabled(newSize ==
                                                    PRESET_SIZE_UNFOLDED);
        if (mExtendedWindow.hasInstance()) {
            mExtendedWindow.get()
                    ->getVirtualSensorsPage()
                    ->updateHingeSensorUI();
        }
    } else {
        updateFoldableButtonVisibility();
    }
}

void ToolWindow::resizableChangeIcon(PresetEmulatorSizeType type) {
    switch (type) {
        case PRESET_SIZE_PHONE:
            ChangeIcon(mToolsUi->resizable_button, "display_mode_phone_expand",
                       "Display mode: Phone");
            break;
        case PRESET_SIZE_UNFOLDED:
            ChangeIcon(mToolsUi->resizable_button,
                       "display_mode_foldable_expand",
                       "Display mode: Foldable");
            break;
        case PRESET_SIZE_TABLET:
            ChangeIcon(mToolsUi->resizable_button, "display_mode_tablet_expand",
                       "Display mode: Tablet");
            break;
        case PRESET_SIZE_DESKTOP:
            ChangeIcon(mToolsUi->resizable_button,
                       "display_mode_desktop_expand", "Display mode: Desktop");
            break;
        default:
            LOG(ERROR) << "Invalid display mode " << type;
    }
}

// static
void ToolWindow::forwardGenericEventToEmulator(int type, int code, int value) {
    EmulatorQtWindow* emuQtWindow = EmulatorQtWindow::getInstance();
    if (emuQtWindow == nullptr) {
        VLOG(foldable) << "Error send Event, null emulator qt window";
        return;
    }

    SkinEvent skin_event = createSkinEvent(kEventGeneric);
    SkinEventGenericData& genericData = skin_event.u.generic_event;
    genericData.type = type;
    genericData.code = code;
    genericData.value = value;
    emuQtWindow->queueSkinEvent(std::move(skin_event));
}

void ToolWindow::forwardKeyToEmulator(uint32_t keycode, bool down) {
    SkinEvent skin_event = createSkinEvent(down ? kEventKeyDown : kEventKeyUp);
    skin_event.u.key.keycode = keycode;
    skin_event.u.key.mod = 0;
    mEmulatorWindow->queueSkinEvent(std::move(skin_event));
}

bool ToolWindow::handleQtKeyEvent(const QKeyEvent& event, QtKeyEventSource source) {
    // See if this key is handled by the virtual scene control window first.
    if (mVirtualSceneControlWindow.hasInstance() &&
        mVirtualSceneControlWindow.get()->isActive()) {
        if (mVirtualSceneControlWindow.get()->handleQtKeyEvent(event, source)) {
            return true;
        }
    }

    // We don't care about the keypad modifier for anything, and it gets added
    // to the arrow keys of OSX by default, so remove it.
    QKeySequence event_key_sequence(event.key() |
                                    (event.modifiers() & ~Qt::KeypadModifier));
    bool down = event.type() == QEvent::KeyPress;
    bool h = mShortcutKeyStore.handle(event_key_sequence,
                                      [this, down](QtUICommand cmd) {
                                          if (down) {
                                              handleUICommand(cmd, true);
                                              handleUICommand(cmd, false);
                                          }
                                      });
    return h;
}

void ToolWindow::reportMouseButtonDown() {
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->reportMouseButtonDown(); });
}

bool ToolWindow::isExtendedWindowFocused() {
    if (mExtendedWindow.hasInstance()) {
        return mExtendedWindow.get()->isActiveWindow();
    }

    return false;
}

bool ToolWindow::isExtendedWindowVisible() {
    if (mExtendedWindow.hasInstance()) {
        return mExtendedWindow.get()->isVisible();
    }
    return false;
}

void ToolWindow::closeExtendedWindow() {
    mExtendedWindow.clear();
}

void ToolWindow::dockMainWindow() {
    // Align horizontally relative to the main window's frame.
    // Align vertically to its contents.
    // If we're frameless, adjust for a transparent border
    // around the skin.
    bool hasFrame;
    mEmulatorWindow->windowHasFrame(&hasFrame);
    int toolGap = hasFrame ? TOOL_GAP_FRAMED : TOOL_GAP_FRAMELESS;

    move(parentWidget()->frameGeometry().right() + toolGap -
                 mEmulatorWindow->getRightTransparency() + 1,
         parentWidget()->geometry().top() +
                 mEmulatorWindow->getTopTransparency());

    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->dockMainWindow(); });
}

void ToolWindow::raiseMainWindow() {
    mEmulatorWindow->raise();
    mEmulatorWindow->activateWindow();
}

void ToolWindow::updateTheme(const QString& styleSheet) {
    mVirtualSceneControlWindow.ifExists(
            [&] { mVirtualSceneControlWindow.get()->updateTheme(styleSheet); });
    setStyleSheet(styleSheet);
}

// static
void ToolWindow::earlyInitialization(const UiEmuAgent* agentPtr) {
    sUiEmuAgent = agentPtr;
    // This is crazy expensive!
    ExtendedWindow::setAgent(agentPtr);
    VirtualSceneControlWindow::setAgent(agentPtr);

    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (!avdPath) {
        // Can't find the setting!
        return;
    }

    QString avdSettingsFile =
            avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
    QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);

    SaveSnapshotOnExit saveOnExitChoice = static_cast<SaveSnapshotOnExit>(
            avdSpecificSettings
                    .value(Ui::Settings::SAVE_SNAPSHOT_ON_EXIT,
                           static_cast<int>(SaveSnapshotOnExit::Always))
                    .toInt());

    // Synchronize avdParams right here; avoid tight coupling with whether the
    // settings page is initialized.
    switch (saveOnExitChoice) {
        case SaveSnapshotOnExit::Always:
            getConsoleAgents()->settings->avdParams()->flags &=
                    !AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
            break;
        case SaveSnapshotOnExit::Ask:
            // If we can't ask, we'll treat ASK the same as ALWAYS.
            getConsoleAgents()->settings->avdParams()->flags &=
                    !AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
            break;
        case SaveSnapshotOnExit::Never:
            getConsoleAgents()->settings->avdParams()->flags |=
                    AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
            break;
        default:
            break;
    }
}

// static
void ToolWindow::onMainLoopStart() {
    if (sToolWindow) {
        sToolWindow->allowExtWindowCreation();
    }
}

void ToolWindow::setClipboardCallbacks(const UiEmuAgent* agPtr) {
    if (agPtr->clipboard) {
        connect(this, SIGNAL(guestClipboardChanged(QString)), this,
                SLOT(onGuestClipboardChanged(QString)), Qt::QueuedConnection);
        agPtr->clipboard->registerGuestClipboardCallback(
                [](void* context, const uint8_t* data, size_t size) {
                    LOG(DEBUG) << "Clipboard update, guest->host, value='"
                               << std::string_view(
                                          reinterpret_cast<const char*>(data),
                                          size)
                               << "'";

                    auto self = static_cast<ToolWindow*>(context);
                    emit self->guestClipboardChanged(
                            QString::fromUtf8((const char*)data, size));
                },
                this);
        connect(QApplication::clipboard(), SIGNAL(dataChanged()), this,
                SLOT(onHostClipboardChanged()));
    }

    mClipboardSupported = agPtr->clipboard != nullptr;
    emit haveClipboardSharingKnown(mClipboardSupported);
}

void ToolWindow::on_back_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::BACK, true);
}

void ToolWindow::on_back_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::BACK, false);
}

// If we need to ask about saving a snapshot,
// ask here, then set an avdParams flag to
// indicate the choice.
// If we don't need to ask, the avdParams flag
// should already be set.
// If the user cancels the pop-up, return
// 'false' to say we should NOT exit now.
bool ToolWindow::askWhetherToSaveSnapshot() {
    mAskedWhetherToSaveSnapshot = true;
    // Check the UI setting
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (!avdPath) {
        // Can't find the setting! Assume it's not ASK: just return;
        return true;
    }
    QString avdSettingsFile =
            avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
    QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);

    SaveSnapshotOnExit saveOnExitChoice = static_cast<SaveSnapshotOnExit>(
            avdSpecificSettings
                    .value(Ui::Settings::SAVE_SNAPSHOT_ON_EXIT,
                           static_cast<int>(SaveSnapshotOnExit::Always))
                    .toInt());

    if (getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->no_snapshot_save) {
        // The command line was used, so don't ask. That overrides the UI.
        return true;
    }

    if (saveOnExitChoice == SaveSnapshotOnExit::Never) {
        // The avdParams flag should already be set
        return true;
    }

    // If the setting is ALWAYS, we might want to ask anyway, such as when
    // previous saves were known to be slow, or the system has low RAM.
    bool savesWereSlow = androidSnapshot_areSavesSlow(
            android::snapshot::kDefaultBootSnapshot);

#if defined(__APPLE__) && defined(__aarch64__)
    // bug: 222536052
    bool hasLowRam = false;
#else
    bool hasLowRam = System::isUnderMemoryPressure();
#endif

    if (saveOnExitChoice == SaveSnapshotOnExit::Always &&
        (fc::isEnabled(fc::QuickbootFileBacked) ||
         (!savesWereSlow && !hasLowRam))) {
        return true;
    }

    // The setting is ASK or we decided to ask anyway.
    auto askMessageSlow = tr(
            "Do you want to save the current state for the next quick boot?\n\n"
            "Note: Recent saves seem to have been slow. Save can be skipped "
            "by selecting 'No'.");
    auto askMessageLowRam = tr(
            "Do you want to save the current state for the next quick boot?\n\n"
            "Note: Saving the snapshot may take longer because free RAM is "
            "low.");
    auto askMessageDefault = tr(
            "Do you want to save the current state for the next quick boot?");

    auto askMessageNonFileBacked =
            savesWereSlow ? askMessageSlow
                          : (hasLowRam ? askMessageLowRam : askMessageDefault);

    auto askMessage = fc::isEnabled(fc::QuickbootFileBacked)
                              ? tr("In the next emulator session, "
                                   "do you want to auto-save emulator state?")
                              : askMessageNonFileBacked;

    int64_t startTime = get_uptime_ms();
    QMessageBox msgBox(QMessageBox::Question, tr("Save quick-boot state"),
                       askMessage, (QMessageBox::Yes | QMessageBox::No), this);
    // Add a Cancel button to enable the MessageBox's X.
    // Since embedded has already disconnected by this point, we always assume shutdown.
    if(!getConsoleAgents()
            ->settings->android_cmdLineOptions()
            ->qt_hide_window) {
        QPushButton* cancelButton = msgBox.addButton(QMessageBox::Cancel);
        // Hide the Cancel button so X is the only way to cancel.
        cancelButton->setHidden(true);
    }

    // ten seconds
    constexpr int timeout = 10000;
    QTimer::singleShot(timeout, (msgBox.button(QMessageBox::Yes)),
                       SLOT(animateClick()));
    int selection = msgBox.exec();

    int64_t endTime = get_uptime_ms();
    uint64_t dialogTime = endTime - startTime;

    MetricsReporter::get().report([dialogTime](proto::AndroidStudioEvent* event) {
        auto counts =
                event->mutable_emulator_details()->mutable_snapshot_ui_counts();
        counts->set_quickboot_ask_total_time_ms(
                dialogTime + counts->quickboot_ask_total_time_ms());
    });

    if (selection == QMessageBox::Cancel) {
        MetricsReporter::get().report([](proto::AndroidStudioEvent* event) {
            auto counts = event->mutable_emulator_details()
                                  ->mutable_snapshot_ui_counts();
            counts->set_quickboot_ask_canceled(
                    1 + counts->quickboot_ask_canceled());
        });
        mAskedWhetherToSaveSnapshot = false;
        return false;
    }

    if (selection == QMessageBox::Yes) {
        MetricsReporter::get().report([](proto::AndroidStudioEvent* event) {
            auto counts = event->mutable_emulator_details()
                                  ->mutable_snapshot_ui_counts();
            counts->set_quickboot_ask_yes(1 + counts->quickboot_ask_yes());
        });
        getConsoleAgents()->settings->avdParams()->flags &=
                !AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
    } else {
        MetricsReporter::get().report([](proto::AndroidStudioEvent* event) {
            auto counts = event->mutable_emulator_details()
                                  ->mutable_snapshot_ui_counts();
            counts->set_quickboot_ask_no(1 + counts->quickboot_ask_no());
        });
        getConsoleAgents()->settings->avdParams()->flags |=
                AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
    }
    return true;
}

bool ToolWindow::shouldClose() {
    // we already asked, just return the answer
    if (mAskedWhetherToSaveSnapshot) {
        return mShouldClose;
    }

    mShouldClose = askWhetherToSaveSnapshot();

    return mShouldClose;
}

void ToolWindow::on_close_button_clicked() {
    mIsExiting = true;
    setEnabled(false);
    mCloseClicked = true;
    if (QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ShiftModifier)) {
        // The user shift-clicked on the X
        // This counts as us asking and having the user say "don't save"
        mExtendedWindow.ifExists([&] { mExtendedWindow.get()->sendMetricsOnShutDown(); });
        mAskedWhetherToSaveSnapshot = true;
        getConsoleAgents()->settings->avdParams()->flags |=
                AVDINFO_NO_SNAPSHOT_SAVE_ON_EXIT;
        mEmulatorWindow->requestClose();

        return;
    }

    if(shouldClose()) {
        mExtendedWindow.ifExists([&] { mExtendedWindow.get()->sendMetricsOnShutDown(); });
        mEmulatorWindow->requestClose();
    } else {
        mAskedWhetherToSaveSnapshot = false;
        setEnabled(true);
        mIsExiting = false;
        mCloseClicked = false;
    }
}

void ToolWindow::on_home_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::HOME, true);
}

void ToolWindow::on_home_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::HOME, false);
}

void ToolWindow::on_minimize_button_clicked() {
#ifdef __linux__
    this->hide();
#else
    this->showMinimized();
#endif
    mEmulatorWindow->showMinimized();
}

void ToolWindow::on_power_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::POWER, true);
}

void ToolWindow::on_power_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::POWER, false);
}

void ToolWindow::on_tablet_mode_button_clicked() {
    static bool tablet_mode;
    mEmulatorWindow->activateWindow();
    tablet_mode = !tablet_mode;
    ChangeIcon(mToolsUi->tablet_mode_button,
               tablet_mode ? "laptop_mode" : "tablet_mode",
               tablet_mode ? "Switch to laptop mode" : "Switch to tablet mode");
    handleUICommand(QtUICommand::TABLET_MODE, tablet_mode);
}

void ToolWindow::on_change_posture_button_clicked() {
    mPostureSelectionDialog->show();
    // Align pop-up posture selction dialog to the right of posture button
    QRect geoTool = this->geometry();
    mPostureSelectionDialog->move(
            geoTool.right(),
            geoTool.top() + mToolsUi->change_posture_button->geometry().top());
}

void ToolWindow::on_dismiss_posture_selection_dialog() {
    mToolsUi->change_posture_button->setChecked(false);
}

void ToolWindow::on_dismiss_resizable_dialog() {
    mToolsUi->resizable_button->setChecked(false);
}

void ToolWindow::on_resizable_button_clicked() {
    mResizableDialog->show();
    // Align pop-up resizableDialog to the right of resizable button
    QRect geoTool = this->geometry();
    mResizableDialog->move(
            geoTool.right(),
            geoTool.top() + mToolsUi->resizable_button->geometry().top());
}

void ToolWindow::on_new_resizable_requested(PresetEmulatorSizeType newSize) {
    mEmulatorWindow->activateWindow();
    presetSizeAdvance(newSize);
}

void ToolWindow::on_volume_up_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::VOLUME_UP, true);
}
void ToolWindow::on_volume_up_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::VOLUME_UP, false);
}
void ToolWindow::on_volume_down_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::VOLUME_DOWN, true);
}
void ToolWindow::on_volume_down_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::VOLUME_DOWN, false);
}

void ToolWindow::on_overview_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::OVERVIEW, true);
}

void ToolWindow::on_overview_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::OVERVIEW, false);
}

void ToolWindow::on_wear_button_1_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::WEAR_1, true);
}

void ToolWindow::on_wear_button_1_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::WEAR_1, false);
}

void ToolWindow::on_wear_button_2_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::WEAR_2, true);
}

void ToolWindow::on_wear_button_2_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::WEAR_2, false);
}

void ToolWindow::on_palm_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::PALM, true);
}

void ToolWindow::on_palm_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::PALM, false);
}

void ToolWindow::on_tilt_button_pressed() {
    mEmulatorWindow->raise();
    handleUICommand(QtUICommand::TILT, true);
}

void ToolWindow::on_tilt_button_released() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::TILT, false);
}

void ToolWindow::on_prev_layout_button_clicked() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::ROTATE_LEFT);
}

void ToolWindow::on_next_layout_button_clicked() {
    mEmulatorWindow->activateWindow();
    handleUICommand(QtUICommand::ROTATE_RIGHT);
}

void ToolWindow::on_scrShot_button_clicked() {
    handleUICommand(QtUICommand::TAKE_SCREENSHOT, true);
}
void ToolWindow::on_zoom_button_clicked() {
    handleUICommand(QtUICommand::ENTER_ZOOM, true);
}

void ToolWindow::onGuestClipboardChanged(QString text) {
    QSignalBlocker blockSignals(QApplication::clipboard());
    QApplication::clipboard()->setText(text);
}

void ToolWindow::onHostClipboardChanged() {
    QByteArray bytes = QApplication::clipboard()->text().toUtf8();
    sUiEmuAgent->clipboard->setGuestClipboardContents(
            (const uint8_t*)bytes.data(), bytes.size());
}

static bool isPaneEnabled(ExtendedWindowPane pane) {
    // Snapshot pane is disabled in embedded emulator.
    if (pane == PANE_IDX_SNAPSHOT &&
        getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->qt_hide_window) {
        return false;
    }

    return true;
}

void ToolWindow::showOrRaiseExtendedWindow(ExtendedWindowPane pane) {
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
        AVD_ANDROID_AUTO) {
        if (pane == PANE_IDX_DPAD || pane == PANE_IDX_BATTERY ||
            pane == PANE_IDX_FINGER || pane == PANE_IDX_CAMERA ||
            pane == PANE_IDX_MULTIDISPLAY) {
            return;
        }
    }
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_TV ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_WEAR) {
        if (pane == PANE_IDX_MULTIDISPLAY) {
            return;
        }
    }
    if (!androidHwConfig_hasVirtualSceneCamera(
                getConsoleAgents()->settings->hw()) &&
        pane == PANE_IDX_CAMERA) {
        return;
    }

    // Set to default help pane.
    if (!isPaneEnabled(pane)) {
        pane = PANE_IDX_HELP;
    }
    // Show the tabbed pane
    mExtendedWindow.get()->showPane(pane);
    mExtendedWindow.get()->raise();
    mExtendedWindow.get()->activateWindow();
}

void ToolWindow::on_more_button_clicked() {
    if (mAllowExtWindow && !mIsExiting) {
        mExtendedWindow.get()->show();
        mExtendedWindow.get()->raise();
        mExtendedWindow.get()->activateWindow();
    }
}

void ToolWindow::paintEvent(QPaintEvent*) {
    QPainter p;
    QPen pen(Qt::SolidLine);
    pen.setColor(Qt::black);
    pen.setWidth(1);
    p.begin(this);
    p.setPen(pen);

    double dpr = 1.0;
    auto newScreen = window()->windowHandle()
                             ? window()->windowHandle()->screen()
                             : nullptr;
    if (!newScreen) {
        newScreen = qGuiApp->primaryScreen();
    }
    dpr = newScreen->devicePixelRatio();

    if (dpr > 1.0) {
        // Normally you'd draw the border with a (0, 0, w-1, h-1) rectangle.
        // However, there's some weirdness going on with high-density displays
        // that makes a single-pixel "slack" appear at the left and bottom
        // of the border. This basically adds 1 to compensate for it.
        p.drawRect(contentsRect());
    } else {
        p.drawRect(QRect(0, 0, width() - 1, height() - 1));
    }
    p.end();
}

void ToolWindow::notifySwitchOnTop() {
#ifdef _WIN32
    mTopSwitched = true;
#endif
}

void ToolWindow::touchExtendedWindow() {
    mExtendedWindow.get();
}

void ToolWindow::enableCloseButton() {
    mToolsUi->close_button->setVisible(true);
}

void ToolWindow::hideRotationButton(bool hide) {
    if (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_TV ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_ANDROID_AUTO ||
        avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
                AVD_DESKTOP ||
        getConsoleAgents()->settings->hw()->hw_arc) {
        // already hide, do not bother its settings
        return;
    } else {
        mToolsUi->prev_layout_button->setHidden(hide);
        mToolsUi->next_layout_button->setHidden(hide);
    }
}

void ToolWindow::applyFoldableQuirk(int newPosture) {
    const bool is_pixel_fold = android_foldable_is_pixel_fold();
    if (is_pixel_fold) {
        if (newPosture > 1 && mLastRequestedFoldablePosture == 1 ||
                newPosture == 1 && mLastRequestedFoldablePosture > 1) {
            auto hw = (getConsoleAgents()->settings->avdInfo());
            auto apiLevel = avdInfo_getApiLevel(
                    getConsoleAgents()->settings->avdInfo());
            if (avdInfo_isVanillaIceCreamPreview(hw) || apiLevel >= 35) {
                mEmulatorWindow->getAdbInterface()->
                    enqueueCommand( {"shell", "input", "keyevent", "KEYCODE_SLEEP"});
                mSleepKeySent = true;
                startSleepTimer();
            }
        }
    }
    mLastRequestedFoldablePosture = newPosture;
}

void ToolWindow::on_new_posture_requested(int newPosture) {
    mEmulatorWindow->activateWindow();
    applyFoldableQuirk(newPosture);
    handleUICommand(QtUICommand::CHANGE_FOLDABLE_POSTURE, true);
}

WorkerProcessingResult ToolWindow::foldableSyncToAndroidItemFunction(
        const FoldableSyncToAndroidItem& item) {
    switch (item.op) {
        case SEND_AREA: {
            EmulatorQtWindow* emuQtWindow = EmulatorQtWindow::getInstance();
            if (emuQtWindow == nullptr) {
                break;
            }
            char foldedArea[64];
            sprintf(foldedArea, "folded-area %d,%d,%d,%d", item.x, item.y,
                    item.x + item.width, item.y + item.height);
            std::string sendArea(foldedArea);
            emuQtWindow->getAdbInterface()->enqueueCommand(
                    {"shell", "wm", foldedArea},
                    [sendArea](const OptionalAdbCommandResult& result) {
                        if (result && result->exit_code == 0) {
                            VLOG(foldable) << "foldable-page: 'send "
                                           << sendArea << "' command succeeded";
                        }
                    });
            break;
        }
        case CONFIRM_AREA: {
            EmulatorQtWindow* emuQtWindow = EmulatorQtWindow::getInstance();
            if (emuQtWindow) {
                mFoldableSyncToAndroidSuccess = false;
                mFoldableSyncToAndroidTimeout = false;
                int64_t timeOut = System::get()->getUnixTimeUs() +
                                  30000 * 1000;  // 30 second time out
                // Keep on querying folded area through adb,
                // until query returns the expected values
                while (!mFoldableSyncToAndroidSuccess &&
                       !mFoldableSyncToAndroidTimeout) {
                    android::base::AutoLock lock(mLock);
                    emuQtWindow->getAdbInterface()->enqueueCommand(
                            {"shell", "wm", "folded-area"},
                            [this, item,
                             timeOut](const OptionalAdbCommandResult& result) {
                                android::base::AutoLock lock(this->mLock);
                                if (System::get()->getUnixTimeUs() > timeOut) {
                                    LOG(ERROR) << "time out (30 sec) waiting "
                                                  "for window manager "
                                                  "configuring folded area";
                                    this->mFoldableSyncToAndroidTimeout = true;
                                    this->mCv.signalAndUnlock(&lock);
                                    return;
                                }
                                if (!result || !result->output) {
                                    VLOG(foldable) << "Invalid output for wm "
                                                      "adb-area, query again";
                                    this->mFoldableSyncToAndroidSuccess = false;
                                    this->mCv.signalAndUnlock(&lock);
                                    return;
                                }
                                std::string line;
                                std::string expectedAdbOutput =
                                        "Folded area: " +
                                        std::to_string(item.x) + "," +
                                        std::to_string(item.y) + "," +
                                        std::to_string(item.x + item.width) +
                                        "," +
                                        std::to_string(item.y + item.height);
                                while (getline(*(result->output), line)) {
                                    if (line.compare(expectedAdbOutput) == 0) {
                                        this->mFoldableSyncToAndroidSuccess =
                                                true;
                                        break;
                                    }
                                }
                                this->mCv.signalAndUnlock(&lock);
                            });
                    // block thread until adb query for folded-area return
                    if (!mCv.timedWait(&mLock, timeOut)) {
                        // Something unexpected along adb communication thread,
                        // call back function not been called after timeOut
                        LOG(ERROR) << "adb no response for more than 30 "
                                      "seconds, time out";
                        mFoldableSyncToAndroidTimeout = true;
                    }
                    // send LID_CLOSE when expected folded-area is configured in
                    // window manager
                    if (mFoldableSyncToAndroidSuccess) {
                        VLOG(foldable) << "confirm folded area configured by "
                                          "window manager, send LID close";
                        forwardGenericEventToEmulator(EV_SW, SW_LID, true);
                        forwardGenericEventToEmulator(EV_SYN, 0, 0);
                    }
                }
            }
            break;
        }
        case SEND_LID_CLOSE:
            forwardGenericEventToEmulator(EV_SW, SW_LID, true);
            forwardGenericEventToEmulator(EV_SYN, 0, 0);
            VLOG(foldable) << "send LID close";
            break;
        case SEND_LID_OPEN:
            forwardGenericEventToEmulator(EV_SW, SW_LID, false);
            forwardGenericEventToEmulator(EV_SYN, 0, 0);
            VLOG(foldable) << "send LID open";
            break;
        case SEND_EXIT:
            return WorkerProcessingResult::Stop;
    }
    return WorkerProcessingResult::Continue;
}

void ToolWindow::remaskButtons() {
    for (QPushButton* button : findChildren<QPushButton*>()) {
        const QString icon_name = button->property("themeIconName").toString();
        if (!icon_name.isNull()) {
            double dpr = 1.0;
#ifndef Q_OS_MAC
            QScreen* scr = this->screen();
            if (scr) {
                dpr = scr->logicalDotsPerInch() / SizeTweaker::BaselineDpi;
            }
#endif
            const QSize icon_size = button->property("iconSize").toSize();
            const QSize min_size = button->property("minimumSize").toSize();
            button->setMinimumSize(min_size * dpr);
            button->setIconSize(icon_size * dpr);
        }
    }
}

bool ToolWindow::eventFilter(QObject* o, QEvent* event) {
    if (event->type() == QEvent::ScreenChangeInternal) {
        // When moved across screens, masks on buttons need to
        // be adjusted according to screen density.
        remaskButtons();
    } else if (event->type() == QEvent::WindowStateChange) {
        // It seems on mac, repainting after getting QEvent::ScreenChangeInternal is too early.
        // From experiments, asking for repaint after WindowStateChange will resize the tool-window
        // to the correct size. Furthermore, if the window moves too fast to a different display,
        // QEvent::WindowStateChange may not trigger. So a workaround is to also trigger a repaint
        // when the user releases the mouse from moving the window.
        repaint();
    }
    return false;
}
