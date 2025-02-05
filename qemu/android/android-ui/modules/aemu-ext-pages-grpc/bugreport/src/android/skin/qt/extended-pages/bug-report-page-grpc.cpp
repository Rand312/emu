// Copyright (C) 2017 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/extended-pages/bug-report-page-grpc.h"

#include <QtConcurrent/qtconcurrentrun.h>
#include <time.h>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QFuture>
#include <QLabel>
#include <QMovie>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtCore>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "aemu/base/StringFormat.h"
#include "aemu/base/Uri.h"
#include "aemu/base/Uuid.h"
#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/logging/Log.h"
#include "android/base/system/System.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/metrics/UiEventTracker.h"
#include "android/skin/qt/error-dialog.h"
#include "android/skin/qt/extended-pages/common.h"
#include "android/skin/qt/function-runner.h"
#include "android/skin/qt/raised-material-button.h"
#include "android/skin/qt/stylesheet.h"
#include "android/utils/path.h"
#include "emulator_controller.pb.h"
#include "studio_stats.pb.h"
#include "ui_bug-report-page-grpc.h"

using android::base::PathUtils;
using android::base::RecurrentTask;
using android::base::StringAppendFormat;
using android::base::StringFormat;
using android::base::System;
using android::base::ThreadLooper;
using android::base::Uri;
using android::base::Uuid;
using android::emulation::AdbInterface;
using android::emulation::OptionalAdbCommandResult;
using android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::EmulatorStatus;
using android::emulation::control::Image;
using android::emulation::control::ImageFormat;

static const int kReproStepsCharacterLimit = 2000;
static const System::Duration kAdbCommandTimeoutMs = System::kInfinite;
static const System::Duration kTaskInterval = 1000;  // ms

static const char FILE_BUG_URL[] =
        "https://issuetracker.google.com/issues/new"
        "?component=192708&description=%s&template=840533";
// In reference to
// https://developer.android.com/studio/report-bugs.html#emulator-bugs
static const char BUG_REPORT_TEMPLATE[] =
        R"(Steps to Reproduce Bug:%s
Expected Behavior:

Observed Behavior:)";

class DeviceDetailPage : public QFrame {
public:
    explicit DeviceDetailPage(const QString& text, QWidget* parent = nullptr)
        : QFrame(parent) {
        setWindowFlags(Qt::Tool | Qt::WindowCloseButtonHint);
        auto layout = new QVBoxLayout(this);
        auto label = new QLabel(text);
        layout->addWidget(label);
    }
    ~DeviceDetailPage() {}
    void closeEvent(QCloseEvent* event) override {
        hide();
        event->ignore();
    }
};

BugreportPageGrpc::BugreportPageGrpc(QWidget* parent)
    : ThemedWidget(parent),
      mUi(new Ui::BugreportPageGrpc),
      mBugTracker(new UiEventTracker(
              android_studio::EmulatorUiEvent::BUTTON_PRESS,
              android_studio::EmulatorUiEvent::EXTENDED_BUG_TAB)),
      mEmulatorControl(EmulatorGrpcClient::me()),
      // A recurrent task to refresh and load data only after device boots up.
      mTask(
              ThreadLooper::get(),
              [this]() {
                  getBootStatus();
                  if (mBooted) {
                      refreshContents();
                      return false;
                  } else {
                      getBootStatus();
                      return true;
                  }
              },
              kTaskInterval),
      mAdb(android::emulation::AdbInterface::createGlobalOwnThread()) {
    mUi->setupUi(this);
    mUi->bug_deviceLabel->installEventFilter(this);
    mUi->bug_emulatorVersionLabel->setText(QString::fromStdString(
            StringFormat("%s (%s)", mReportingFields.emulatorVer,
                         mReportingFields.hypervisorVer)));
    mUi->bug_androidVersionLabel->setText(
            QString::fromStdString(mReportingFields.androidVer));
    QFontMetrics metrics(mUi->bug_deviceLabel->font());
    QString elidedText = metrics.elidedText(
            QString::fromStdString(mReportingFields.deviceName), Qt::ElideRight,
            mUi->bug_deviceLabel->width());
    mUi->bug_deviceLabel->setText(elidedText);
    mUi->bug_hostMachineLabel->setText(
            QString::fromStdString(mReportingFields.hostOsName));
    mDeviceDetail.reset(new DeviceDetailPage(
            QString::fromStdString(mReportingFields.avdDetails), this));
    mUi->bug_sendToGoogle->setIcon(getIconForCurrentTheme("open_in_browser"));
    connect(this, SIGNAL(imageLoaded()), this, SLOT(on_imageLoaded()));
}

BugreportPageGrpc::~BugreportPageGrpc() {
    mTask.stopAsync();

    if (System::get()->pathIsFile(mSavingStates.adbBugreportFilePath)) {
        System::get()->deleteFile(mSavingStates.adbBugreportFilePath);
    }
    if (System::get()->pathIsFile(mSavingStates.screenshotFilePath)) {
        System::get()->deleteFile(mSavingStates.screenshotFilePath);
    }

    // We need to cancel these running adb commands, as their result callbacks
    // may call runOnUiThread on an already destroyed EmulatorQtWindow instance.
    // Cancelling will ensure the result callback is not called.
    if (mAdbLogcat) {
        mAdbLogcat->cancel();
    }
    if (mAdbBugreport) {
        mAdbBugreport->cancel();
    }
}

void BugreportPageGrpc::setAdbInterface(AdbInterface* adb) {
    mAdb = adb;
}

void BugreportPageGrpc::getBootStatus() {
    mEmulatorControl.getStatusAsync([this](absl::StatusOr<EmulatorStatus*> status) {
        if (!status.ok()) {
            return;
        }

        auto info = status.value();
        mBooted = info->booted();
        for (const auto& entry : info->hardwareconfig().entry()) {
            if (entry.key() == "avd.api_level") {
                mApiLevel = std::stoi(entry.value());
            }
            if (entry.key() == "AvdId") {
                mDeviceName = entry.value();
            }
        }
    });
}

void BugreportPageGrpc::updateTheme() {
    SettingsTheme theme = getSelectedTheme();
    QMovie* movie = new QMovie(this);
    movie->setFileName(":/" + Ui::stylesheetValues(theme)[Ui::THEME_PATH_VAR] +
                       "/circular_spinner");
    if (movie->isValid()) {
        movie->start();
        mUi->bug_circularSpinner->setMovie(movie);
    }

    mUi->bug_bugReportTextEdit->setStyleSheet(
            "background: transparent; border: 1px solid " +
            Ui::stylesheetValues(theme)["EDIT_COLOR"]);
    mUi->bug_sendToGoogle->setIcon(getIconForCurrentTheme("open_in_browser"));
}

void BugreportPageGrpc::showEvent(QShowEvent* event) {
    // Override stylesheet for QPlainTextEdit[readOnly=true]
    SettingsTheme theme = getSelectedTheme();
    mUi->bug_bugReportTextEdit->setStyleSheet(
            "background: transparent; border: 1px solid " +
            Ui::stylesheetValues(theme)["EDIT_COLOR"]);
    QWidget::showEvent(event);
    // Refresh contents for screenshot, logcat and adb bugreport
    // every time the page is shown unless its underlying command is still
    // ongoing.
    if (!mTask.inFlight())
        mTask.start();
}

void BugreportPageGrpc::on_imageLoaded() {
    if (System::get()->pathIsFile(mSavingStates.screenshotFilePath) &&
        System::get()->pathCanRead(mSavingStates.screenshotFilePath)) {
        QPixmap image(mSavingStates.screenshotFilePath.c_str());
        int height = mUi->bug_screenshotImage->height();
        int width = mUi->bug_screenshotImage->width();
        mUi->bug_screenshotImage->setPixmap(
                image.scaled(width, height, Qt::KeepAspectRatio));
    } else {
        // TODO(wdu) Better error handling for failed screen capture
        // operation
        mUi->bug_screenshotImage->setText(
                tr("There was an error while capturing the "
                   "screenshot."));
    }
}

void BugreportPageGrpc::loadScreenshotImage() {
    // Delete previously taken screenshot if it exists
    if (System::get()->pathIsFile(mSavingStates.screenshotFilePath)) {
        System::get()->deleteFile(mSavingStates.screenshotFilePath);
        mSavingStates.screenshotFilePath.clear();
    }

    ImageFormat fmt;
    fmt.set_format(ImageFormat::PNG);
    mEmulatorControl.getScreenshotAsync(fmt, [this](absl::StatusOr<Image*> status) {
        if (!status.ok()) {
            dwarning("Failed to obtain screenshot: %s",
                     status.status().message().data());
            return;
        }
        mSavingStates.screenshotFilePath =
                PathUtils::join(System::get()->getTempDir(), "screenshot.png");
        std::ofstream screenShotFile(
                PathUtils::asUnicodePath(
                        mSavingStates.screenshotFilePath.c_str())
                        .c_str(),
                std::ios_base::out | std::ios_base::binary);

        if (!screenShotFile.is_open() || !screenShotFile.good()) {
            dwarning("Failed to save screenshot to %s. ",
                     mSavingStates.screenshotFilePath.c_str());
            return;
        }

        screenShotFile << status.value()->image();
        emit(imageLoaded());
    });
}

void BugreportPageGrpc::refreshContents() {
    // clean up the content loaded from last time.
    mUi->bug_bugReportTextEdit->clear();
    mUi->bug_reproStepsTextEdit->clear();
    mUi->bug_screenshotImage->clear();
    // check if save location is set
    if (mSavingStates.saveLocation.empty()) {
        mSavingStates.saveLocation = getScreenshotSaveDirectory().toStdString();
    }

    if (!mSavingStates.saveLocation.empty() &&
        System::get()->pathIsDir(mSavingStates.saveLocation) &&
        System::get()->pathCanWrite(mSavingStates.saveLocation)) {
        // Reset the bugreportSavedSucceed to avoid using stale bugreport
        // folder.
        mSavingStates.bugreportSavedSucceed = false;
        loadAdbLogcat();
        loadAdbBugreport();
        loadScreenshotImage();
    } else {
        showErrorDialog(tr("The bugreport save location is not set.<br/>"
                           "Check the settings page and ensure the directory "
                           "exists and is writeable."),
                        tr("Bugreport"));
    }
}

void BugreportPageGrpc::enableInput(bool enabled) {
    SettingsTheme theme = getSelectedTheme();
    setButtonEnabled(mUi->bug_sendToGoogle, theme, enabled);
    setButtonEnabled(mUi->bug_saveButton, theme, enabled);
}

void BugreportPageGrpc::showSpinningWheelAnimation(bool enabled) {
    if (!mUi->bug_circularSpinner->movie()) {
        SettingsTheme theme = getSelectedTheme();
        QMovie* movie = new QMovie(this);
        movie->setFileName(":/" +
                           Ui::stylesheetValues(theme)[Ui::THEME_PATH_VAR] +
                           "/circular_spinner");
        if (movie->isValid()) {
            movie->start();
            mUi->bug_circularSpinner->setMovie(movie);
        }
    }

    if (enabled) {
        mUi->bug_circularSpinner->show();
        mUi->bug_collectingLabel->show();
    } else {
        mUi->bug_circularSpinner->hide();
        mUi->bug_collectingLabel->hide();
    }
}

void BugreportPageGrpc::on_bug_bugReportCheckBox_clicked() {
    if (mUi->bug_bugReportCheckBox->isChecked()) {
        if (mAdbBugreport && mAdbBugreport->inFlight()) {
            showSpinningWheelAnimation(true);
            enableInput(false);
        }
    } else {
        showSpinningWheelAnimation(false);
        enableInput(true);
    }
}

void BugreportPageGrpc::on_bug_saveButton_clicked() {
    mBugTracker->increment("ON_SAVE");
    QString dirName = QString::fromStdString(mSavingStates.saveLocation);
    dirName = QFileDialog::getExistingDirectory(
            this, tr("Report Saving Location"), dirName);
    if (dirName.isNull())
        return;
    auto savingPath = PathUtils::join(dirName.toStdString(),
                                      generateUniqueBugreportName());

    enableInput(false);
    QFuture<bool> future = QtConcurrent::run(
            &BugreportPageGrpc::saveBugReportTo, this, savingPath);
    mSavingStates.bugreportSavedSucceed = future.result();
    enableInput(true);
    if (!mSavingStates.bugreportSavedSucceed) {
        showErrorDialog(tr("The bugreport save location is invalid.<br/>"
                           "Check the settings page and ensure the directory "
                           "exists and is writeable."),
                        tr("Bug Report"));
    }
}

void BugreportPageGrpc::on_bug_sendToGoogle_clicked() {
    mBugTracker->increment("ON_SEND");
    if (!mSavingStates.bugreportSavedSucceed) {
        on_bug_saveButton_clicked();
    }
    // launch the issue tracker in a separate thread
    QFuture<bool> future =
            QtConcurrent::run(&BugreportPageGrpc::launchIssueTracker, this);
    bool success = future.result();
    if (!success) {
        QString errMsg =
                tr("There was an error while opening the Google issue "
                   "tracker.<br/>");
        showErrorDialog(errMsg, tr("File a Bug for Google"));
    }
}

bool BugreportPageGrpc::saveBugReportTo(std::string_view savingPath) {
    if (savingPath.empty()) {
        return false;
    }

    if (path_mkdir_if_needed(savingPath.data(), 0755)) {
        return false;
    }

    if (mUi->bug_bugReportCheckBox->isChecked() &&
        mSavingStates.adbBugreportSucceed) {
        std::string_view bugreportBaseName;
        if (PathUtils::extension(mSavingStates.adbBugreportFilePath) ==
            ".zip") {
            bugreportBaseName = "bugreport.zip";
        } else {
            bugreportBaseName = "bugreport.txt";
        }
        path_copy_file(
                PathUtils::join(savingPath.data(), bugreportBaseName.data())
                        .c_str(),
                mSavingStates.adbBugreportFilePath.c_str());
    }

    if (mUi->bug_screenshotCheckBox->isChecked() &&
        mSavingStates.screenshotSucceed) {
        std::string_view screenshotBaseName = "screenshot.png";
        path_copy_file(
                PathUtils::join(savingPath.data(), screenshotBaseName.data())
                        .c_str(),
                mSavingStates.screenshotFilePath.c_str());
    }

    if (!mReportingFields.avdDetails.empty()) {
        auto avdDetailsFilePath =
                PathUtils::join(savingPath.data(), "avd_details.txt");
        saveToFile(avdDetailsFilePath, mReportingFields.avdDetails.c_str(),
                   mReportingFields.avdDetails.length());
    }
    QString reproSteps = mUi->bug_reproStepsTextEdit->toPlainText();
    reproSteps.truncate(kReproStepsCharacterLimit);
    mReproSteps = reproSteps.toStdString();
    if (!mReproSteps.empty()) {
        auto reproStepsFilePath =
                PathUtils::join(savingPath.data(), "repro_steps.txt");
        saveToFile(reproStepsFilePath, mReproSteps.c_str(),
                   mReproSteps.length());
    }
    return true;
}

bool BugreportPageGrpc::launchIssueTracker() {
    QString reproSteps = mUi->bug_reproStepsTextEdit->toPlainText();
    reproSteps.truncate(kReproStepsCharacterLimit);
    mReproSteps = reproSteps.toStdString();
    // launch the issue tracker in a separate thread
    std::string bugTemplate = mReportingFields.dump();

    StringAppendFormat(&bugTemplate, BUG_REPORT_TEMPLATE, mReproSteps);
    std::string encodedArgs =
            Uri::FormatEncodeArguments(FILE_BUG_URL, bugTemplate);
    QUrl url(QString::fromStdString(encodedArgs));
    return url.isValid() && QDesktopServices::openUrl(url);
}

void BugreportPageGrpc::loadAdbBugreport() {
    // Avoid generating adb bugreport multiple times while in execution.
    if (mAdbBugreport && mAdbBugreport->inFlight())
        return;

    // No-op if no adb interface
    if (!mAdb)
        return;

    mSavingStates.adbBugreportSucceed = false;
    enableInput(false);
    if (mUi->bug_bugReportCheckBox->isChecked()) {
        showSpinningWheelAnimation(true);
    }

    auto filePath = PathUtils::join(System::get()->getTempDir(),
                                    generateUniqueBugreportName());
    // In reference to
    // platform/frameworks/native/cmds/dumpstate/bugreport-format.md
    // Issue different command args given the API level

    bool isNougatOrHigher =
            (mApiLevel != kDefaultUnknownAPILevel && mApiLevel > 23);

    // Only 0.7% is below N.
    if (isNougatOrHigher)
        filePath.append(".zip");
    else
        filePath.append(".txt");
    bool wantOutput = !isNougatOrHigher;

    mAdbBugreport = mAdb->runAdbCommand(
            (isNougatOrHigher) ? std::vector<std::string>{"bugreport", filePath}
                               : std::vector<std::string>{"bugreport"},
            [this, filePath,
             wantOutput](const OptionalAdbCommandResult& result) {
                mAdbBugreport.reset();
                bool success = (result && result->exit_code == 0);
                if (success && wantOutput) {
                    std::string s(
                            std::istreambuf_iterator<char>(*result->output),
                            {});
                    success = saveToFile(filePath, s.c_str(), s.length());
                }

                runOnEmuUiThread([this, success, filePath] {
                    enableInput(true);
                    showSpinningWheelAnimation(false);
                    if (System::get()->pathIsFile(
                                mSavingStates.adbBugreportFilePath)) {
                        System::get()->deleteFile(
                                mSavingStates.adbBugreportFilePath);
                        mSavingStates.adbBugreportFilePath.clear();
                    }
                    if (success) {
                        mSavingStates.adbBugreportSucceed = true;
                        mSavingStates.adbBugreportFilePath = filePath;
                    } else {
                        // TODO(wdu) Better error handling for failed adb
                        // bugreport generation
                        showErrorDialog(
                                tr("Bug report interrupted by snapshot "
                                   "load? "
                                   "There was an error while generating "
                                   "adb bugreport"),
                                tr("Bug Report"));
                    }
                });
            },
            kAdbCommandTimeoutMs, wantOutput);
}

void BugreportPageGrpc::loadAdbLogcat() {
    // Avoid generating adb logcat multiple times while in execution.
    if (mAdbLogcat && mAdbLogcat->inFlight()) {
        return;
    }

    if (!mAdb)
        return;
    // After apiLevel 19, buffer "all" become available
    mAdbLogcat = mAdb->runAdbCommand(
            (mApiLevel != kDefaultUnknownAPILevel && mApiLevel > 19)
                    ? std::vector<std::string>{"logcat", "-b", "all", "-d"}
                    : std::vector<std::string>{"logcat", "-b", "events", "-b",
                                               "main", "-b", "radio", "-b",
                                               "system", "-d"},
            [this](const OptionalAdbCommandResult& result) {
                mAdbLogcat.reset();
                if (!result || result->exit_code || !result->output) {
                    runOnEmuUiThread([this] {
                        mUi->bug_bugReportTextEdit->setPlainText(
                                tr("There was an error while loading adb "
                                   "logcat"));
                    });
                } else {
                    std::string s(
                            std::istreambuf_iterator<char>(*result->output),
                            {});
                    runOnEmuUiThread([this, s] {
                        mUi->bug_bugReportTextEdit->setPlainText(
                                QString::fromStdString(s));
                    });
                }
            },
            kAdbCommandTimeoutMs, true);
}

bool BugreportPageGrpc::saveToFile(std::string_view filePath,
                                   const char* content,
                                   size_t length) {
    std::ofstream outFile(PathUtils::asUnicodePath(filePath.data()).c_str(),
                          std::ios_base::out | std::ios_base::binary);
    if (!outFile.is_open() || !outFile.good())
        return false;
    outFile.write(content, length);
    outFile.close();
    return true;
}

bool BugreportPageGrpc::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (mDeviceDetail.get()) {
            mDeviceDetail->show();
            setFrameOnTop(mDeviceDetail.get(), true);
        }
        return true;
    } else {
        return false;
    }
}

std::string BugreportPageGrpc::generateUniqueBugreportName() {
    time_t now = System::get()->getUnixTime();
    char date[80];
    strftime(date, sizeof(date), "%Y-%m-%d-%H-%M-%S", localtime(&now));
    return StringFormat("bugreport-%s-%s-%s", mDeviceName, date,
                        Uuid::generate().toString());
}
