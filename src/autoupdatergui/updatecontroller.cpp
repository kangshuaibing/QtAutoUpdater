#include "updatecontroller.h"
#include "updatecontroller_p.h"
#include "adminauthorization_p.h"
#include "updatebutton.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <dialogmaster.h>

#include <QtAutoUpdaterCore/private/updater_p.h>

using namespace QtAutoUpdater;

UpdateController::UpdateController(QObject *parent) :
	QObject{parent},
	d{new UpdateControllerPrivate{this, nullptr}}
{}

UpdateController::UpdateController(QWidget *parentWidget, QObject *parent) :
	QObject{parent},
	d{new UpdateControllerPrivate{this, parentWidget}}
{}

UpdateController::UpdateController(const QString &maintenanceToolPath, QObject *parent) :
	QObject{parent},
	d{new UpdateControllerPrivate{this, maintenanceToolPath, nullptr}}
{}

UpdateController::UpdateController(const QString &maintenanceToolPath, QWidget *parentWidget, QObject *parent) :
	QObject{parent},
	d{new UpdateControllerPrivate{this, maintenanceToolPath, parentWidget}}
{}

UpdateController::~UpdateController() = default;

QAction *UpdateController::createUpdateAction(QObject *parent)
{
	auto updateAction = new QAction(UpdateControllerPrivate::getUpdatesIcon(),
									tr("Check for Updates"),
									parent);
	updateAction->setMenuRole(QAction::ApplicationSpecificRole);
	updateAction->setToolTip(tr("Checks if new updates are available. You will be prompted before updates are installed."));

	connect(updateAction, &QAction::triggered, this, [this](){
		start(UpdateController::ProgressLevel);
	});
	connect(this, &UpdateController::runningChanged,
			updateAction, &QAction::setDisabled);
	connect(this, &UpdateController::destroyed,
			updateAction, &QAction::deleteLater);

	return updateAction;
}

QString UpdateController::maintenanceToolPath() const
{
	return d->mainUpdater->maintenanceToolPath();
}

QWidget *UpdateController::parentWindow() const
{
	return d->window;
}

void UpdateController::setParentWindow(QWidget *parentWindow)
{
	d->window = parentWindow;
}

UpdateController::DisplayLevel UpdateController::currentDisplayLevel() const
{
	return d->displayLevel;
}

bool UpdateController::isRunning() const
{
	return d->running;
}

bool UpdateController::runAsAdmin() const
{
	return d->runAdmin;
}

void UpdateController::setRunAsAdmin(bool runAsAdmin, bool userEditable)
{
	if(d->runAdmin != runAsAdmin) {
		d->runAdmin = runAsAdmin;
		if(d->mainUpdater->willRunOnExit())
			d->mainUpdater->runUpdaterOnExit(d->runAdmin ? new AdminAuthorization() : nullptr);
		emit runAsAdminChanged(runAsAdmin);
	}
	d->adminUserEdit = userEditable;
}

QStringList UpdateController::updateRunArgs() const
{
	return d->runArgs;
}

void UpdateController::setUpdateRunArgs(QStringList updateRunArgs)
{
	d->runArgs = std::move(updateRunArgs);
}

void UpdateController::resetUpdateRunArgs()
{
	d->runArgs = QStringList(QStringLiteral("--updater"));
}

bool UpdateController::isDetailedUpdateInfo() const
{
	return d->detailedInfo;
}

QString UpdateController::desktopFileName() const
{
	return d->taskbar ?
				d->taskbar->attribute(QTaskbarControl::LinuxDesktopFile).toString() :
				QString{};
}

void UpdateController::setDetailedUpdateInfo(bool detailedUpdateInfo)
{
	d->detailedInfo = detailedUpdateInfo;
}

void UpdateController::setDesktopFileName(const QString &desktopFileName)
{
	if(d->taskbar)
		d->taskbar->setAttribute(QTaskbarControl::LinuxDesktopFile, desktopFileName);
}

Updater *UpdateController::updater() const
{
	return d->mainUpdater;
}

bool UpdateController::start(DisplayLevel displayLevel)
{
	if(d->running)
		return false;
	d->running = true;
	emit runningChanged(true);
	d->wasCanceled = false;
	d->displayLevel = displayLevel;

	if(d->displayLevel >= AskLevel) {
		if(DialogMaster::questionT(d->window,
								   tr("Check for Updates"),
								   tr("Do you want to check for updates now?"))
		   != QMessageBox::Yes) {
			d->running = false;
			emit runningChanged(false);
			return false;
		}
	}

	if(!d->mainUpdater->checkForUpdates()) {
		if(d->displayLevel >= ProgressLevel) {
			DialogMaster::warningT(d->window,
								   tr("Check for Updates"),
								   tr("The program is already checking for updates!"));
		}
		d->running = false;
		emit runningChanged(false);
		return false;
	} else {
		if(d->displayLevel >= ExtendedInfoLevel) {
			if(d->displayLevel >= ProgressLevel) {
				if(d->taskbar) {
					d->taskbar->setProgress(-1.0);
					d->taskbar->setProgressVisible(true);
				}
				d->checkUpdatesProgress = new ProgressDialog{d->window};
				connect(d->checkUpdatesProgress.data(), &ProgressDialog::canceled, this, [this](){
					d->wasCanceled = true;
				});
				d->checkUpdatesProgress->open(d->mainUpdater, &QtAutoUpdater::Updater::abortUpdateCheck);
			}
		}

		return true;
	}
}

bool UpdateController::cancelUpdate(int maxDelay)
{
	if(d->mainUpdater->isRunning()) {
		d->wasCanceled = true;
		if(d->checkUpdatesProgress)
			d->checkUpdatesProgress->setCanceled();
		d->mainUpdater->abortUpdateCheck(maxDelay, true);
		return true;
	} else
		return false;
}

int UpdateController::scheduleUpdate(int delaySeconds, bool repeated, UpdateController::DisplayLevel displayLevel)
{
	if((static_cast<qint64>(delaySeconds) * 1000ll) > static_cast<qint64>(std::numeric_limits<int>::max())) {
		qCWarning(logQtAutoUpdater) << "delaySeconds to big to be converted to msecs";
		return 0;
	}
	return d->scheduler->startSchedule(delaySeconds * 1000, repeated, QVariant::fromValue(displayLevel));
}

int UpdateController::scheduleUpdate(const QDateTime &when, UpdateController::DisplayLevel displayLevel)
{
	return d->scheduler->startSchedule(when, QVariant::fromValue(displayLevel));
}

void UpdateController::cancelScheduledUpdate(int taskId)
{
	d->scheduler->cancelSchedule(taskId);
}

void UpdateController::checkUpdatesDone(bool hasUpdates, bool hasError)
{
	if(d->displayLevel >= ExtendedInfoLevel) {
		if(d->checkUpdatesProgress) {
			d->checkUpdatesProgress->hide();
			d->checkUpdatesProgress->deleteLater();
			d->checkUpdatesProgress = nullptr;
		}
	}

	if(d->wasCanceled) {
		if(d->displayLevel >= ExtendedInfoLevel) {
			d->setTaskbarState(QTaskbarControl::Paused);
			DialogMaster::warningT(d->window,
								   tr("Check for Updates"),
								   tr("Checking for updates was canceled!"));
		}
	} else {
		if(hasUpdates) {
			if(d->displayLevel >= InfoLevel) {
				const auto updateInfos = d->mainUpdater->updateInfo();
				d->setTaskbarState(QTaskbarControl::Running);
				if(d->taskbar) {
					if(updateInfos.size() > 0) {
						d->taskbar->setCounter(updateInfos.size());
						d->taskbar->setCounterVisible(true);
					} else
						d->taskbar->setCounterVisible(false);
				}


				auto shouldShutDown = false;
				const auto oldRunAdmin = d->runAdmin;
				const auto res = UpdateInfoDialog::showUpdateInfo(updateInfos,
																  d->runAdmin,
																  d->adminUserEdit,
																  d->detailedInfo,
																  d->window);
				d->clearTaskbar();

				if(d->runAdmin != oldRunAdmin)
					emit runAsAdminChanged(d->runAdmin);

				switch(res) {
				case UpdateInfoDialog::InstallNow:
					shouldShutDown = true;
					Q_FALLTHROUGH();
				case UpdateInfoDialog::InstallLater:
					d->mainUpdater->runUpdaterOnExit(d->runAdmin ? new AdminAuthorization() : nullptr);
					if(shouldShutDown)
						qApp->quit();
					break;
				case UpdateInfoDialog::NoInstall:
					break;
				default:
					Q_UNREACHABLE();
				}

			} else {
				d->mainUpdater->runUpdaterOnExit(d->runAdmin ? new AdminAuthorization() : nullptr);
				if(d->displayLevel == ExitLevel) {
					d->setTaskbarState(QTaskbarControl::Running);
					DialogMaster::informationT(d->window,
											   tr("Install Updates"),
											   tr("New updates are available. The maintenance tool will be "
												  "started to install those as soon as you close the application!"));
				} else
					qApp->quit();
			}
		} else {
			if(hasError) {
				qCWarning(logQtAutoUpdater) << "maintenancetool process finished with exit code"
											<< d->mainUpdater->errorCode()
											<< "and error string:"
											<< d->mainUpdater->errorLog();
			}

			if(d->displayLevel >= ExtendedInfoLevel) {
				if(d->mainUpdater->exitedNormally()) {
					d->setTaskbarState(QTaskbarControl::Stopped);
					DialogMaster::criticalT(d->window,
											tr("Check for Updates"),
											tr("No new updates available!"));
				} else {
					d->setTaskbarState(QTaskbarControl::Paused);
					DialogMaster::warningT(d->window,
										   tr("Check for Updates"),
										   tr("The update process crashed!"));
				}
			}
		}
	}

	d->clearTaskbar();
	d->running = false;
	emit runningChanged(false);
}

void UpdateController::timerTriggered(const QVariant &parameter)
{
	if(parameter.canConvert<DisplayLevel>())
		start(parameter.value<DisplayLevel>());
}

//-----------------PRIVATE IMPLEMENTATION-----------------

QIcon UpdateControllerPrivate::getUpdatesIcon()
{
	return QIcon::fromTheme(QStringLiteral("system-software-update"), QIcon(QStringLiteral(":/QtAutoUpdater/icons/update.ico")));
}

UpdateControllerPrivate::UpdateControllerPrivate(UpdateController *q_ptr, QWidget *window) :
	UpdateControllerPrivate(q_ptr, QString(), window)
{}

UpdateControllerPrivate::UpdateControllerPrivate(UpdateController *q_ptr, const QString &toolPath, QWidget *window) :
	q{q_ptr},
	window{window},
	mainUpdater{toolPath.isEmpty() ? new Updater{q_ptr} : new Updater{toolPath, q_ptr}},
	taskbar{window ? new QTaskbarControl{window} : nullptr},
	scheduler{new SimpleScheduler{q_ptr}}
{
	QObject::connect(mainUpdater, &Updater::checkUpdatesDone,
					 q, &UpdateController::checkUpdatesDone,
					 Qt::QueuedConnection);
	QObject::connect(scheduler, &SimpleScheduler::scheduleTriggered,
					 q, &UpdateController::timerTriggered);

#ifdef Q_OS_UNIX
	QFileInfo maintenanceInfo(QCoreApplication::applicationDirPath(),
							  mainUpdater->maintenanceToolPath());
	runAdmin = (maintenanceInfo.ownerId() == 0);
#endif
}

UpdateControllerPrivate::~UpdateControllerPrivate()
{
	if(running)
		qCWarning(logQtAutoUpdater) << "UpdaterController destroyed while still running! This can crash your application!";

	if(checkUpdatesProgress)
		checkUpdatesProgress->deleteLater();

	clearTaskbar();
}

void UpdateControllerPrivate::setTaskbarState(QTaskbarControl::WinProgressState state)
{
	if(taskbar) {
		taskbar->setProgress(1.0);
		taskbar->setAttribute(QTaskbarControl::WindowsProgressState, state);
#ifdef Q_OS_WIN
		taskbar->setProgressVisible(true);
#else
		taskbar->setProgressVisible(false);
#endif
	}
}


void UpdateControllerPrivate::clearTaskbar()
{
	if(taskbar) {
		taskbar->setCounterVisible(false);
		taskbar->setProgressVisible(false);
		taskbar->deleteLater();
	}
}
