#include "mainwindow.h"

#include <QMessageBox>
#include <QBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QDebug>
#include <QThread>
#include <QListView>
#include <QLineEdit>
#include <QAbstractButton>
#include <QPushButton>
#include <QLabel>
#include <QClipboard>
#include <QDir>
#include <QMenuBar>
#include <QFile>
#include <QFileDialog>
#include <QMenu>
#include <QRadioButton>
#include <QProgressBar>
#include <QStackedWidget>
#include <QJsonObject>
#include <QJsonDocument>

#include "loggedinwidget.h"
#include "account.h"
#include "esdbmodel.h"
#include "newaccount.h"
#include "resetdevice.h"
#include "editaccount.h"
#include "buttonwaitdialog.h"
#include "loginwindow.h"
#include "common.h"
#include "signetapplication.h"

extern "C" {
#include "signetdev.h"
};

#include "changemasterpassword.h"

MainWindow::MainWindow(QWidget *parent) :
	QMainWindow(parent),
	m_wipeProgress(NULL),
	m_wipingWidget(NULL),
	m_connected(false),
	m_quitting(false),
	m_fileMenu(NULL),
	m_deviceMenu(NULL),
	m_loggedInStack(NULL),
	m_connectingLabel(NULL),
	m_loggedIn(false),
	m_wasConnected(false),
	m_deviceState(STATE_INVALID),
	m_backupWidget(NULL),
	m_backupProgress(NULL),
	m_backupFile(NULL),
	m_backupPrevState(STATE_INVALID),
	m_restoreWidget(NULL),
	m_restoreBlock(0),
	m_restoreFile(NULL),
	m_uninitPrompt(NULL),
	m_backupAction(NULL),
	m_restoreAction(NULL),
	m_logoutAction(NULL),
	m_wipeDeviceAction(NULL),
	m_eraseDeviceAction(NULL),
	m_changePasswordAction(NULL),
	m_buttonWaitDialog(NULL),
	m_signetdevCmdToken(-1)
{
	QObject::connect(&m_resetTimer, SIGNAL(timeout()), this, SLOT(resetTimer()));

	QObject::connect(&m_connectingTimer, SIGNAL(timeout()), this, SLOT(connectingTimer()));
	QMenuBar *bar = new QMenuBar();
	this->setMenuBar(bar);
	m_fileMenu = bar->addMenu("File");
	QAction *quit_action = m_fileMenu->addAction("Quit");
	QObject::connect(quit_action, SIGNAL(triggered(bool)), this, SLOT(quit()));

	m_deviceMenu = bar->addMenu("Device");

	m_backupAction = m_deviceMenu->addAction("Backup device");
	QObject::connect(m_backupAction, SIGNAL(triggered(bool)),
			 this, SLOT(backupDeviceUi()));

	m_restoreAction = m_deviceMenu->addAction("Restore device");
	QObject::connect(m_restoreAction, SIGNAL(triggered(bool)),
			 this, SLOT(restoreDeviceUi()));

	m_logoutAction = m_deviceMenu->addAction("Logout");
	QObject::connect(m_logoutAction, SIGNAL(triggered(bool)),
			 this, SLOT(logoutUi()));

	m_changePasswordAction = m_deviceMenu->addAction("Change master password");
	QObject::connect(m_changePasswordAction, SIGNAL(triggered(bool)),
			 this, SLOT(changePasswordUi()));

	m_eraseDeviceAction = m_deviceMenu->addAction("Reset");
	QObject::connect(m_eraseDeviceAction, SIGNAL(triggered(bool)),
			 this, SLOT(eraseDeviceUi()));

	m_wipeDeviceAction = m_deviceMenu->addAction("Wipe");
	QObject::connect(m_wipeDeviceAction, SIGNAL(triggered(bool)),
			 this, SLOT(wipeDeviceUi()));

	m_updateFirmwareAction = m_deviceMenu->addAction("Update firmware");
	QObject::connect(m_updateFirmwareAction, SIGNAL(triggered(bool)),
			 this, SLOT(updateFirmwareUi()));

	m_logoutAction->setVisible(false);
	m_eraseDeviceAction->setVisible(false);
	m_wipeDeviceAction->setVisible(false);
	m_changePasswordAction->setVisible(false);
	m_backupAction->setVisible(false);
	m_restoreAction->setVisible(false);
	enterDeviceState(STATE_NEVER_SHOWN);
}

void MainWindow::connectionError()
{
	if (!m_resetTimer.isActive()) {
		m_wasConnected = true;
		enterDeviceState(STATE_CONNECTING);
		int rc = signetdev_open_connection();
		if (rc == 0) {
			deviceOpened();
		}
	}
}

#ifdef _WIN32
bool MainWindow::nativeEvent(const QByteArray & eventType, void *message, long *result)
{
	Q_UNUSED(result);
	if (eventType == QByteArray("windows_generic_MSG")) {
		MSG *msg = (MSG *) message;
		signetdev_filter_window_messasage(msg->message, msg->wParam, msg->lParam);
	}
	return false;
}
#endif

void MainWindow::deviceOpened()
{
	::signetdev_startup_async(NULL, &m_signetdevCmdToken);
}

void MainWindow::deviceClosed()
{
	emit connectionError();
}

void MainWindow::signetdevGetProgressResp(signetdevCmdRespInfo info, signetdev_get_progress_resp_data data)
{
	if (info.token != m_signetdevCmdToken) {
		return;
	}
	m_signetdevCmdToken = -1;
	int resp_code = info.resp_code;

	if (m_deviceState == STATE_WIPING) {
		switch (resp_code) {
		case OKAY:
			m_wipeProgress->setRange(0, data.total_progress_maximum);
			m_wipeProgress->setValue(data.total_progress);
			m_wipeProgress->update();
			::signetdev_get_progress_async(NULL, &m_signetdevCmdToken, data.total_progress, WIPING);
			break;
		case INVALID_STATE:
			enterDeviceState(STATE_UNINITIALIZED);
			break;
		case SIGNET_ERROR_DISCONNECT:
		case SIGNET_ERROR_QUIT:
			return;
		default:
			abort();
			return;
		}
	} else if (m_deviceState == STATE_UPDATING_FIRMWARE) {
		switch (resp_code) {
		case OKAY:
			m_firmwareUpdateProgress->setRange(0, data.total_progress_maximum);
			m_firmwareUpdateProgress->setValue(data.total_progress);
			m_firmwareUpdateProgress->update();
			::signetdev_get_progress_async(NULL, &m_signetdevCmdToken, data.total_progress, ERASING_PAGES);
			break;
		case INVALID_STATE: {
			m_firmwareUpdateStage->setText("Writing firmware data...");
			m_totalWritten += m_writingSize;

			m_firmwareUpdateProgress->setValue(m_totalWritten);

			int total_bytes = 0;

			for (auto a = m_fwSections.begin(); a != m_fwSections.end(); a++) {
				total_bytes += a->size;
			}
			m_firmwareUpdateProgress->setRange(0, total_bytes);
			m_firmwareUpdateProgress->setValue(0);

			m_writingSectionIter = m_fwSections.begin();
			m_writingAddr = m_writingSectionIter->lma;
			m_totalWritten = 0;
			sendFirmwareWriteCmd();
		}
		break;
		case SIGNET_ERROR_DISCONNECT:
		case SIGNET_ERROR_QUIT:
			return;
		default:
			abort();
			return;
		}
	}
}

void MainWindow::signetdevReadBlockResp(signetdevCmdRespInfo info, QByteArray block)
{
	if (info.token != m_signetdevCmdToken) {
		return;
	}
	m_signetdevCmdToken = -1;

	int code = info.resp_code;
	switch (code) {
	case OKAY: {
		int rc;
		rc = m_backupFile->write(block);
		if (rc == -1) {
			QMessageBox * box = SignetApplication::messageBoxError(QMessageBox::Critical, "Backup device", "Failed to write to backup file", this);
			connect(box, SIGNAL(finished(int)), this, SLOT(backupError()));
			return;
		}
		m_backupBlock++;
		m_backupProgress->setMinimum(0);
		m_backupProgress->setMaximum(MAX_ID);
		m_backupProgress->setValue(m_backupBlock);
		if (m_backupBlock > MAX_ID) {
			::signetdev_end_device_backup_async(NULL, &m_signetdevCmdToken);
		} else {
			::signetdev_read_block_async(NULL, &m_signetdevCmdToken, m_backupBlock);
		}
	}
	break;
	case BUTTON_PRESS_CANCELED:
	case BUTTON_PRESS_TIMEOUT:
	case SIGNET_ERROR_DISCONNECT:
	case SIGNET_ERROR_QUIT:
		break;
	default:
		abort();
		return;
	}
}

void MainWindow::signetdevCmdResp(signetdevCmdRespInfo info)
{
	if (info.token != m_signetdevCmdToken) {
		return;
	}
	m_signetdevCmdToken = -1;

	bool do_abort = false;
	int code = info.resp_code;

	switch (code) {
	case OKAY:
	case BUTTON_PRESS_CANCELED:
	case BUTTON_PRESS_TIMEOUT:
	case SIGNET_ERROR_DISCONNECT:
	case SIGNET_ERROR_QUIT:
		break;
	default:
		do_abort = true;
		return;
	}

	switch (info.cmd) {
	case SIGNETDEV_CMD_ERASE_PAGES:
		if (code == OKAY) {
			::signetdev_get_progress_async(NULL, &m_signetdevCmdToken, 0, ERASING_PAGES);
		}
		break;
	case SIGNETDEV_CMD_WRITE_FLASH:
		if (code == OKAY) {
			m_totalWritten += m_writingSize;
			m_firmwareUpdateProgress->setValue(m_totalWritten);
			m_firmwareUpdateProgress->update();
			if (m_writingSectionIter == m_fwSections.end()) {
				::signetdev_reset_device_async(NULL, &m_signetdevCmdToken);
			} else {
				sendFirmwareWriteCmd();
			}
		}
		break;
	case SIGNETDEV_CMD_WRITE_BLOCK:
		if (code == OKAY) {
			m_restoreBlock++;
			m_restoreProgress->setMinimum(0);
			m_restoreProgress->setMaximum(MAX_ID);
			m_restoreProgress->setValue(m_restoreBlock);
			if (m_restoreBlock < MAX_ID) {
				QByteArray block(BLK_SIZE, 0);
				int sz = m_restoreFile->read(block.data(), block.length());
				if (sz == BLK_SIZE) {
					::signetdev_write_block_async(NULL, &m_signetdevCmdToken, m_restoreBlock, block.data());
				} else {
					QMessageBox *box = SignetApplication::messageBoxError(QMessageBox::Critical, "Restore device", "Failed to read from source file", this);
					connect(box, SIGNAL(finished(int)), this, SLOT(restoreError()));
				}
			} else {
				::signetdev_end_device_restore_async(NULL, &m_signetdevCmdToken);
			}
		}
		break;
	case SIGNETDEV_CMD_BEGIN_DEVICE_BACKUP:
		if (m_buttonWaitDialog)
			m_buttonWaitDialog->done(QMessageBox::Ok);
		if (code == OKAY) {
			m_backupPrevState = m_deviceState;
			enterDeviceState(STATE_BACKING_UP);
			m_backupBlock = 0;
			::signetdev_read_block_async(NULL, &m_signetdevCmdToken, m_backupBlock);
		} else {
			do_abort = do_abort && (code != BUTTON_PRESS_CANCELED && code != BUTTON_PRESS_TIMEOUT);
		}
		break;
	case SIGNETDEV_CMD_END_DEVICE_BACKUP:
		m_backupFile->close();
		delete m_backupFile;
		m_backupFile = NULL;
		if (code == OKAY) {
			enterDeviceState(m_backupPrevState);
		}
		break;
	case SIGNETDEV_CMD_BEGIN_DEVICE_RESTORE:
		if (m_buttonWaitDialog)
			m_buttonWaitDialog->done(QMessageBox::Ok);
		if (code == OKAY) {
			enterDeviceState(STATE_RESTORING);
			m_restoreBlock = 0;
			m_restoreProgress->setMinimum(0);
			m_restoreProgress->setMaximum(MAX_ID);
			m_restoreProgress->setValue(m_restoreBlock);
			QByteArray block(BLK_SIZE, 0);
			int sz = m_restoreFile->read(block.data(), block.length());
			if (sz == BLK_SIZE) {
				::signetdev_write_block_async(NULL, &m_signetdevCmdToken, m_restoreBlock, block.data());
			} else {
				QMessageBox *box = SignetApplication::messageBoxError(QMessageBox::Critical, "Restore device", "Failed to read from source file", this);
				connect(box, SIGNAL(finished(int)), this, SLOT(restoreError()));
			}
		} else {
			do_abort = do_abort && (code != BUTTON_PRESS_CANCELED && code != BUTTON_PRESS_TIMEOUT);
		}
		break;
	case SIGNETDEV_CMD_END_DEVICE_RESTORE:
		if (code == OKAY) {
			::signetdev_startup_async(NULL, &m_signetdevCmdToken);
		}
		break;
	case SIGNETDEV_CMD_LOGOUT:
		if (code == OKAY) {
			enterDeviceState(STATE_LOGGED_OUT);
		}
		break;
	case SIGNETDEV_CMD_WIPE:
		if (m_buttonWaitDialog)
			m_buttonWaitDialog->done(QMessageBox::Ok);
		if (code == OKAY) {
			enterDeviceState(STATE_WIPING);
		}
		break;
	case SIGNETDEV_CMD_BEGIN_UPDATE_FIRMWARE: {
		if (m_buttonWaitDialog)
			m_buttonWaitDialog->done(QMessageBox::Ok);
		if (code == OKAY) {
			enterDeviceState(STATE_UPDATING_FIRMWARE);
		}
	}
	break;
	case SIGNETDEV_CMD_RESET_DEVICE: {
		if (code == OKAY) {
			m_firmwareUpdateStage->setText("Resetting device...");
			m_firmwareUpdateProgress->hide();
			::signetdev_close_connection();
			m_resetTimer.setSingleShot(true);
			m_resetTimer.setInterval(500);
			m_resetTimer.start();
		}
	}
	break;
	}
	if (do_abort) {
		abort();
	}
}

void MainWindow::restoreError()
{
	::signetdev_end_device_restore_async(NULL, &m_signetdevCmdToken);
}

void MainWindow::backupError()
{
	::signetdev_end_device_backup_async(NULL, &m_signetdevCmdToken);
}

void MainWindow::signetdevStartupResp(signetdevCmdRespInfo info, int device_state, QByteArray hashfn, QByteArray salt)
{
	if (info.token != m_signetdevCmdToken) {
		return;
	}
	m_signetdevCmdToken = -1;
	int code = info.resp_code;
	SignetApplication *app = SignetApplication::get();
	app->setSalt(salt);
	app->setHashfn(hashfn);

	if (m_restoreFile) {
		m_restoreFile->close();
		delete m_restoreFile;
		m_restoreFile = NULL;
	}

	switch (code) {
	case OKAY:
		switch (device_state) {
		case LOGGED_OUT:
			enterDeviceState(STATE_LOGGED_OUT);
			break;
		case UNINITIALIZED:
			enterDeviceState(STATE_UNINITIALIZED);
			break;
		}
		break;
	case BUTTON_PRESS_CANCELED:
	case BUTTON_PRESS_TIMEOUT:
	case SIGNET_ERROR_DISCONNECT:
	case SIGNET_ERROR_QUIT:
		return;
	default:
		abort();
		return;
	}
}

void MainWindow::showEvent(QShowEvent *event)
{
	if (!event->spontaneous()) {
		if (m_deviceState == STATE_NEVER_SHOWN) {
			enterDeviceState(STATE_CONNECTING);
#ifdef _WIN32
			signetdev_win32_set_window_handle((HANDLE)winId());
#endif
			int rc = signetdev_open_connection();
			if (rc == 0) {
				deviceOpened();
			}
		}
	}
}

void MainWindow::quit()
{
	m_quitting = true;
	emit close();
}

void MainWindow::changePasswordUi()
{
	ChangeMasterPassword *cmp = new ChangeMasterPassword(this);
	connect(cmp, SIGNAL(abort()), this, SLOT(abort()));
	connect(cmp, SIGNAL(finished(int)), cmp, SLOT(deleteLater()));
	cmp->show();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (m_quitting) {
		event->accept();
	} else {
		emit hide();
		event->ignore();
	}
}

MainWindow::~MainWindow()
{
}

void MainWindow::logoutUi()
{
	if (m_deviceState == STATE_LOGGED_IN) {
		::signetdev_logout_async(NULL, &m_signetdevCmdToken);
	}
}

void MainWindow::open()
{
	show();
	setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
	raise();
}

void MainWindow::signetDevEvent(int code)
{
	Q_UNUSED(code);
	open();
}

void MainWindow::background()
{
	showMinimized();
}

void MainWindow::enterDeviceState(int state)
{
	if (state == m_deviceState && (state != STATE_LOGGED_OUT))
		return;
	switch (m_deviceState) {
	case STATE_LOGGED_IN:
		break;
	case STATE_BACKING_UP:
		m_loggedInStack->setCurrentIndex(0);
		m_loggedInStack->removeWidget(m_backupWidget);
		m_backupWidget->deleteLater();
		m_backupWidget = NULL;
		break;
	case STATE_LOGGED_IN_LOADING_ACCOUNTS: {
		QWidget *w = m_loggedInStack->currentWidget();
		m_loggedInStack->setCurrentIndex(0);
		m_loggedInStack->removeWidget(w);
		w->deleteLater();
	}
	break;
	case STATE_CONNECTING:
		m_connectingTimer.stop();
		m_connectingLabel = NULL;
		break;
	default:
		break;
	}

	m_deviceState = (enum device_state) state;

	switch (m_deviceState) {
	case STATE_CONNECTING: {
		m_loggedIn = false;
		QWidget *connecting_widget = new QWidget();
		QBoxLayout *layout = new QBoxLayout(QBoxLayout::TopToBottom);
		layout->setAlignment(Qt::AlignTop);
		if (m_wasConnected) {
			m_connectingLabel = new QLabel("No Signet device detected.\n\nPlease insert device.");
			m_wasConnected = false;
		} else {
			m_connectingLabel = new QLabel("Connecting to device...");
			m_connectingTimer.setSingleShot(true);
			m_connectingTimer.setInterval(500);
			m_connectingTimer.start();
		}

		layout->addWidget(m_connectingLabel);

		m_deviceMenu->setDisabled(true);
		m_fileMenu->setDisabled(false);
		connecting_widget->setLayout(layout);
		setCentralWidget(connecting_widget);
	}
	break;
	case STATE_RESET:
		m_loggedIn = false;
		break;
	case STATE_WIPING: {
		m_loggedIn = false;
		m_wipingWidget = new QWidget(this);
		QBoxLayout *layout = new QBoxLayout(QBoxLayout::TopToBottom);
		layout->setAlignment(Qt::AlignTop);
		m_wipeProgress = new QProgressBar();
		layout->addWidget(new QLabel("Wiping device..."));
		layout->addWidget(m_wipeProgress);
		m_wipingWidget->setLayout(layout);
		m_deviceMenu->setDisabled(true);
		m_fileMenu->setDisabled(false);
		setCentralWidget(m_wipingWidget);
		::signetdev_get_progress_async(NULL, &m_signetdevCmdToken, 0, WIPING);
	}
	break;
	case STATE_LOGGED_IN_LOADING_ACCOUNTS: {
		m_loggedIn = true;
		m_deviceMenu->setDisabled(true);
		m_fileMenu->setDisabled(false);
		QLabel *loading_label = new QLabel("Loading...");
		loading_label->setStyleSheet("font-weight: bold");

		QProgressBar *loading_progress = new QProgressBar();

		QVBoxLayout *layout = new QVBoxLayout();
		layout->setAlignment(Qt::AlignTop);
		layout->addWidget(loading_label);
		layout->addWidget(loading_progress);

		QWidget *loadingWidget = new QWidget();
		loadingWidget->setLayout(layout);

		LoggedInWidget *loggedInWidget = new LoggedInWidget(this, loading_progress);
		connect(loggedInWidget, SIGNAL(abort()), this, SLOT(abort()));
		connect(loggedInWidget, SIGNAL(enterDeviceState(int)),
			this, SLOT(enterDeviceState(int)));
		connect(SignetApplication::get(), SIGNAL(signetdevEvent(int)), loggedInWidget, SLOT(signetDevEvent(int)));
		connect(loggedInWidget, SIGNAL(background()), this, SLOT(background()));

		m_loggedInStack = new QStackedWidget();
		m_loggedInStack->addWidget(loggedInWidget);
		m_loggedInStack->addWidget(loadingWidget);
		m_loggedInStack->setCurrentIndex(1);
		setCentralWidget(m_loggedInStack);
	}
	break;
	case STATE_BACKING_UP: {
		m_loggedIn = true;
		m_backupWidget = new QWidget();
		QBoxLayout *layout = new QBoxLayout(QBoxLayout::TopToBottom);
		layout->setAlignment(Qt::AlignTop);
		m_backupProgress = new QProgressBar();
		layout->addWidget(new QLabel("Backing up device..."));
		layout->addWidget(m_backupProgress);
		m_backupWidget->setLayout(layout);
		m_deviceMenu->setDisabled(true);
		m_fileMenu->setDisabled(false);
		m_loggedInStack->addWidget(m_backupWidget);
		m_loggedInStack->setCurrentWidget(m_backupWidget);
	}
	break;
	case STATE_RESTORING: {
		m_loggedIn = false;
		m_restoreWidget = new QWidget();
		QBoxLayout *layout = new QBoxLayout(QBoxLayout::TopToBottom);
		layout->setAlignment(Qt::AlignTop);
		m_restoreProgress = new QProgressBar();
		layout->addWidget(new QLabel("Restoring device..."));
		layout->addWidget(m_restoreProgress);
		m_restoreWidget->setLayout(layout);
		m_deviceMenu->setDisabled(true);
		m_fileMenu->setDisabled(false);
		setCentralWidget(m_restoreWidget);
	}
	break;
	case STATE_UPDATING_FIRMWARE: {
		m_firmwareUpdateWidget = new QWidget();
		QBoxLayout *layout = new QBoxLayout(QBoxLayout::TopToBottom);
		layout->setAlignment(Qt::AlignTop);
		m_firmwareUpdateProgress = new QProgressBar();
		m_firmwareUpdateStage = new QLabel("Erasing firmware pages...");
		layout->addWidget(m_firmwareUpdateStage);
		layout->addWidget(m_firmwareUpdateProgress);
		m_firmwareUpdateWidget->setLayout(layout);
		m_deviceMenu->setDisabled(true);
		m_fileMenu->setDisabled(false);
		QByteArray erase_pages_;
		QByteArray page_mask(512, 0);

		for (auto iter = m_fwSections.begin(); iter != m_fwSections.end(); iter++) {
			const fwSection &section = (*iter);
			unsigned int lma = section.lma;
			unsigned int lma_end = lma + section.size;
			int page_begin = (lma - 0x8000000)/2048;
			int page_end = (lma_end - 1 - 0x8000000)/2048;
			for (int i  = page_begin; i <= page_end; i++) {
				if (i < 0)
					continue;
				if (i >= 511)
					continue;
				page_mask[i] = 1;
			}
		}

		for (int i = 0; i < 512; i++) {
			if (page_mask[i]) {
				erase_pages_.push_back(i);
			}
		}
		::signetdev_erase_pages_async(NULL, &m_signetdevCmdToken,
					      erase_pages_.size(),
					      (u8 *)erase_pages_.data());
		setCentralWidget(m_firmwareUpdateWidget);
	}
	break;
	case STATE_UNINITIALIZED: {
		m_loggedIn = false;
		m_deviceMenu->setDisabled(false);
		m_fileMenu->setDisabled(false);
		m_logoutAction->setVisible(false);

		m_backupAction->setVisible(true);
		m_backupAction->setDisabled(true);

		m_restoreAction->setVisible(true);
		m_restoreAction->setDisabled(false);

		m_wipeDeviceAction->setVisible(false);

		m_changePasswordAction->setVisible(false);

		m_eraseDeviceAction->setVisible(true);
		m_eraseDeviceAction->setDisabled(false);
		m_eraseDeviceAction->setText("Initialize");

		m_updateFirmwareAction->setVisible(false);

		m_uninitPrompt = new QWidget();
		QVBoxLayout *layout = new QVBoxLayout();
		layout->setAlignment(Qt::AlignTop);
		QPushButton *init_button = new QPushButton("Initialize");
		QPushButton *restore_button = new QPushButton("Restore from file");
		layout->addWidget(new QLabel("This device is uninitialized.\n\nSelect an option below enable the device\n"));
		layout->addWidget(init_button);
		layout->addWidget(restore_button);
		m_uninitPrompt->setLayout(layout);
		connect(init_button, SIGNAL(pressed()), this, SLOT(eraseDeviceUi()));
		connect(restore_button, SIGNAL(pressed()), this, SLOT(restoreDeviceUi()));
		setCentralWidget(m_uninitPrompt);
	}
	break;
	case STATE_LOGGED_OUT: {
		m_loggedIn = false;
		m_deviceMenu->setDisabled(false);
		m_fileMenu->setDisabled(false);
		m_logoutAction->setVisible(false);

		m_backupAction->setVisible(true);
		m_backupAction->setDisabled(true);

		m_restoreAction->setVisible(true);
		m_restoreAction->setDisabled(false);

		m_wipeDeviceAction->setVisible(true);
		m_wipeDeviceAction->setDisabled(false);

		m_changePasswordAction->setVisible(true);
		m_changePasswordAction->setDisabled(false);

		m_eraseDeviceAction->setVisible(true);
		m_eraseDeviceAction->setDisabled(false);
		m_eraseDeviceAction->setText("Reset device");

		m_updateFirmwareAction->setVisible(false);

		LoginWindow *login_window = new LoginWindow(this);
		connect(login_window, SIGNAL(enterDeviceState(int)),
			this, SLOT(enterDeviceState(int)));
		connect(login_window, SIGNAL(abort()), this, SLOT(abort()));
		setCentralWidget(login_window);
	}
	break;
	case STATE_LOGGED_IN: {
		m_loggedIn = true;
		resize(QSize(200, 300));
		m_deviceMenu->setDisabled(false);
		m_fileMenu->setDisabled(false);
		m_logoutAction->setVisible(true);
		m_logoutAction->setDisabled(false);

		m_backupAction->setVisible(true);
		m_backupAction->setDisabled(false);

		m_restoreAction->setVisible(true);
		m_restoreAction->setDisabled(true);

		m_wipeDeviceAction->setVisible(true);
		m_wipeDeviceAction->setDisabled(true);

		m_changePasswordAction->setVisible(true);
		m_changePasswordAction->setDisabled(false);

		m_eraseDeviceAction->setVisible(true);
		m_eraseDeviceAction->setDisabled(true);
		m_eraseDeviceAction->setText("Reinitialize");

		m_updateFirmwareAction->setVisible(true);

		m_loggedInStack->setCurrentIndex(0);
	}
	break;
	default:
		break;
	}
}

void MainWindow::eraseDeviceUi()
{
	ResetDevice *rd = new ResetDevice(m_deviceState != STATE_UNINITIALIZED,this);
	connect(rd, SIGNAL(enterDeviceState(int)),
		this, SLOT(enterDeviceState(int)));
	connect(rd, SIGNAL(abort()), this, SLOT(abort()));
	connect(rd, SIGNAL(finished(int)), rd, SLOT(deleteLater()));
	rd->show();
}

void MainWindow::abort()
{
	QMessageBox *box = SignetApplication::messageBoxError(QMessageBox::Critical, "Signet", "Unexpected device error. Press okay to exit.", this);
	connect(box, SIGNAL(finished(int)), this, SLOT(quit()));
}

void MainWindow::connectingTimer()
{
	m_connectingLabel->setText("No Signet device detected.\n\nPlease insert device.");
}

void MainWindow::resetTimer()
{
	m_resetTimer.stop();
	enterDeviceState(STATE_CONNECTING);
	m_wasConnected = false;
	int rc = ::signetdev_open_connection();
	if (rc == 0) {
		deviceOpened();
	}
}

void MainWindow::sendFirmwareWriteCmd()
{
	bool advance = false;
	unsigned int section_lma = m_writingSectionIter->lma;
	unsigned int section_size = m_writingSectionIter->size;
	unsigned int section_end = section_lma + section_size;
	unsigned int write_size = 1024;
	if ((m_writingAddr + write_size) >= section_end) {
		write_size = section_end - m_writingAddr;
		advance = true;
	}
	void *data = m_writingSectionIter->contents.data() + (m_writingAddr - section_lma);

	::signetdev_write_flash_async(NULL, &m_signetdevCmdToken, m_writingAddr, data, write_size);
	if (advance) {
		m_writingSectionIter++;
		if (m_writingSectionIter != m_fwSections.end()) {
			m_writingAddr = m_writingSectionIter->lma;
		}
	} else {
		m_writingAddr += write_size;
	}
	m_writingSize = write_size;
}

void MainWindow::updateFirmwareUi()
{
	QFileDialog fd(this);
	QStringList filters;
	filters.append("*.signet_fw");
	filters.append("*");
	fd.setNameFilters(filters);
	fd.setFileMode(QFileDialog::AnyFile);
	fd.setAcceptMode(QFileDialog::AcceptOpen);
	fd.setWindowModality(Qt::WindowModal);
	fd.exec();
	QStringList sl = fd.selectedFiles();
	if (sl.empty()) {
		return;
	}

	QFile firmware_update_file(sl.first());
	bool result = firmware_update_file.open(QFile::ReadWrite);
	if (!result) {
		firmware_update_file.close();
		SignetApplication::messageBoxError(QMessageBox::Warning, "Update firmware", "Failed to open firmware file", this);
		return;
	}

	QByteArray datum = firmware_update_file.readAll();
	QJsonDocument doc = QJsonDocument::fromJson(datum);

	firmware_update_file.close();

	bool valid_fw = !doc.isNull() && doc.isObject();

	QJsonObject doc_obj;
	QJsonObject sections_obj;
	m_fwSections.clear();

	if (valid_fw) {
		doc_obj = doc.object();
		QJsonValue temp_val = doc_obj.value("sections");
		valid_fw = (temp_val != QJsonValue::Undefined) && temp_val.isObject();
		if (valid_fw) {
			sections_obj = temp_val.toObject();
		}
	}

	if (valid_fw) {
		for (auto iter = sections_obj.constBegin(); iter != sections_obj.constEnd() && valid_fw; iter++) {
			fwSection section;
			section.name = iter.key();
			QJsonValue temp = iter.value();
			if (!temp.isObject()) {
				valid_fw = false;
				break;
			}

			QJsonObject section_obj = temp.toObject();
			QJsonValue lma_val = section_obj.value("lma");
			QJsonValue size_val = section_obj.value("size");
			QJsonValue contents_val = section_obj.value("contents");

			if (lma_val == QJsonValue::Undefined ||
			    size_val == QJsonValue::Undefined ||
			    contents_val == QJsonValue::Undefined ||
			    !lma_val.isDouble() ||
			    !size_val.isDouble() ||
			    !contents_val.isString()) {
				valid_fw = false;
				break;
			}
			section.lma = (unsigned int)(lma_val.toDouble());
			section.size = (unsigned int)(size_val.toDouble());
			section.contents = QByteArray::fromBase64(contents_val.toString().toLatin1());
			if (section.contents.size() != section.size) {
				valid_fw = false;
				break;
			}
			m_fwSections.append(section);
		}
	}
	if (valid_fw) {
		m_buttonWaitDialog = new ButtonWaitDialog("Update firmware", "update firmware", this);
		connect(m_buttonWaitDialog, SIGNAL(finished(int)), this, SLOT(operationFinished(int)));
		m_buttonWaitDialog->show();
		::signetdev_begin_update_firmware_async(NULL, &m_signetdevCmdToken);
	} else {
		SignetApplication::messageBoxError(QMessageBox::Warning, "Update firmware", "Firmware file not valid", this);
	}
}

void MainWindow::wipeDeviceUi()
{
	m_wipeDeviceDialog = new QMessageBox(QMessageBox::Warning,
					     "Wipe device",
					     "This will permanently erase the contents of the device. Continue?",
					     QMessageBox::Ok | QMessageBox::Cancel,
					     this);
	connect(m_wipeDeviceDialog, SIGNAL(finished(int)), this, SLOT(wipeDeviceDialogFinished(int)));
	connect(m_wipeDeviceDialog, SIGNAL(finished(int)), m_wipeDeviceDialog, SLOT(deleteLater()));
	m_wipeDeviceDialog->setWindowModality(Qt::WindowModal);
	m_wipeDeviceDialog->show();
}

void MainWindow::wipeDeviceDialogFinished(int result)
{
	if (result != QMessageBox::Ok) {
		return;
	}
	m_buttonWaitDialog = new ButtonWaitDialog("Wipe device", "wipe device", this);
	connect(m_buttonWaitDialog, SIGNAL(finished(int)), this, SLOT(operationFinished(int)));
	m_buttonWaitDialog->show();
	::signetdev_wipe_async(NULL, &m_signetdevCmdToken);
}

void MainWindow::operationFinished(int code)
{
	m_buttonWaitDialog->deleteLater();
	if (code != QMessageBox::Ok) {
		::signetdev_cancel_button_wait();
	}
	m_buttonWaitDialog = NULL;
}

void MainWindow::backupDeviceUi()
{
	QFileDialog fd(this);
	QStringList filters;
	filters.append("*.pm_backup");
	filters.append("*");
	fd.setNameFilters(filters);
	fd.setFileMode(QFileDialog::AnyFile);
	fd.setAcceptMode(QFileDialog::AcceptSave);
	fd.setDefaultSuffix(QString("pm_backup"));
	fd.setWindowModality(Qt::WindowModal);
	fd.exec();
	QStringList sl = fd.selectedFiles();
	if (sl.empty()) {
		return;
	}
	m_backupFile = new QFile(sl.first());
	bool result = m_backupFile->open(QFile::ReadWrite);
	if (!result) {
		SignetApplication::messageBoxError(QMessageBox::Warning, "Backup device", "Failed to create destination file", this);
		return;
	}
	m_buttonWaitDialog = new ButtonWaitDialog("Backup device", "start backing up device", this);
	connect(m_buttonWaitDialog, SIGNAL(finished(int)), this, SLOT(operationFinished(int)));
	m_buttonWaitDialog->show();
	::signetdev_begin_device_backup_async(NULL, &m_signetdevCmdToken);
}

void MainWindow::restoreDeviceUi()
{
	QFileDialog fd(this);
	QStringList filters;
	filters.append("*.pm_backup");
	filters.append("*");
	fd.setNameFilters(filters);
	fd.setFileMode(QFileDialog::AnyFile);
	fd.setAcceptMode(QFileDialog::AcceptOpen);
	fd.setDefaultSuffix(QString("pm_backup"));
	fd.setWindowModality(Qt::WindowModal);
	fd.exec();
	QStringList sl = fd.selectedFiles();
	if (sl.empty()) {
		return;
	}
	m_restoreFile = new QFile(sl.first());
	bool result = m_restoreFile->open(QFile::ReadWrite);
	if (!result) {
		delete m_restoreFile;
		SignetApplication::messageBoxError(QMessageBox::Warning, "Restore device", "Failed to open backup file", this);
		return;
	}
	if (m_restoreFile->size() != BLK_SIZE * (MAX_ID + 1)) {
		delete m_restoreFile;
		SignetApplication::messageBoxError(QMessageBox::Warning, "Restore device", "Backup file has wrong size", this);
		return;
	}

	m_buttonWaitDialog = new ButtonWaitDialog("Restore device", "start restoring device", this);
	connect(m_buttonWaitDialog, SIGNAL(finished(int)), this, SLOT(operationFinished(int)));
	m_buttonWaitDialog->show();
	::signetdev_begin_device_restore_async(NULL, &m_signetdevCmdToken);
}
