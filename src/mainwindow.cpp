/******************************************************************************
*  Project: NextGIS GL Viewer
*  Purpose: GUI viewer for spatial data.
*  Author:  Dmitry Baryshnikov, bishop.dev@gmail.com
*******************************************************************************
*  Copyright (C) 2016-2017 NextGIS, <info@nextgis.com>
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 2 of the License, or
*   (at your option) any later version.
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
#include "mainwindow.h"

// Qt
#include <QApplication>
#include <QCloseEvent>
#include <QSettings>
#include <QStatusBar>
#include <QtWidgets>

// ngstore
#include "ngstore/api.h"
#include "ngstore/version.h"

#include "catalogdialog.h"
#include "version.h"

// progress function
int loadProgressFunction(enum ngsCode /*status*/,
                          double complete,
                          const char* /*message*/,
                          void* progressArguments)
{
    ProgressDialog* dlg = static_cast<ProgressDialog*>(progressArguments);
    if(nullptr != dlg) {
        dlg->setProgress(static_cast<int>(complete * 100));
        if(dlg->isCancel()) {
            return 0;
        }
    }
    return 1;
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    m_progressDlg(nullptr)
{
    setWindowIcon(QIcon(":/images/main_logo.svg"));

    char** options = nullptr;
    options = ngsAddNameValue(options, "DEBUG_MODE", "ON");
    options = ngsAddNameValue(options, "SETTINGS_DIR",
                              ngsFormFileName(ngsGetCurrentDirectory(), "tmp",
                                              nullptr));
    options = ngsAddNameValue(options, "GDAL_DATA",
                              qgetenv("GDAL_DATA").constData());
    options = ngsAddNameValue(options, "NUM_THREADS", "7");
    int result = ngsInit(options);

    ngsDestroyList(options);

    if(result == ngsCode::COD_SUCCESS && createDatastore()) {
        m_mapModel = new MapModel();
        // create empty map
        m_mapModel->create();

        // statusbar setup
        setStatusText(tr("Ready"), 30000); // time limit 30 sec.
        m_locationStatus = new LocationStatus;
        statusBar()->addPermanentWidget(m_locationStatus);

        m_eventsStatus = new EventsStatus;
        statusBar()->addPermanentWidget(m_eventsStatus);
        statusBar()->setStyleSheet("QStatusBar::item { border: none }"); // disable borders

        createActions ();
        createMenus();
        createDockWindows();
        readSettings();

        /*m_eventsStatus->addMessage ();
        m_eventsStatus->addWarning ();
        m_eventsStatus->addError ();*/


        connect(&m_watcher, SIGNAL(finished()), this, SLOT(loadFinished()));

    }
    else {
        QMessageBox::critical(this, tr("Error"), tr("Storage initialize failed"));
    }
}

void MainWindow::setStatusText(const QString &text, int timeout)
{
    statusBar()->showMessage(text, timeout);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    writeSettings();
    delete m_mapModel;
    ngsUnInit();
    event->accept();
}

void MainWindow::writeSettings()
{
    QSettings settings;

    settings.beginGroup("MainWindow");
    if(isMaximized()){
        settings.setValue("frame_maximized", true);
    }
    else{
        settings.setValue("frame_size", size());
        settings.setValue("frame_pos", pos());
    }
    settings.setValue("frame_state", saveState());
    settings.setValue("frame_statusbar_shown", statusBar()->isVisible());
    settings.setValue("splitter_sizes", m_splitter->saveState());
    settings.endGroup();
}

void MainWindow::readSettings()
{
    QSettings settings;

    settings.beginGroup("MainWindow");
    if(settings.value("frame_maximized", false).toBool()){
        showMaximized();
    }
    else{
        resize(settings.value("frame_size", QSize(400, 400)).toSize());
        move(settings.value("frame_pos", QPoint(200, 200)).toPoint());
    }
    restoreState(settings.value("frame_state").toByteArray());
    statusBar()->setVisible(settings.value("frame_statusbar_shown", true).toBool());
    m_splitter->restoreState(settings.value("splitter_sizes").toByteArray());
    settings.endGroup();
}

void MainWindow::newFile()
{
    m_mapModel->create();
}

void MainWindow::open()
{
    CatalogDialog dlg(CatalogDialog::OPEN, tr("Open map"),
                      ngsCatalogObjectType::CAT_FILE_NGMAPDOCUMENT, this);
    int result = dlg.exec();

    if(1 == result) {
        std::string openPath = dlg.getCatalogPath().c_str();
        if(!m_mapModel->open(openPath.c_str())) {
            QMessageBox::critical(this, tr("Error"), tr("Map load failed"));
        }
        else {
            statusBar ()->showMessage(tr("Map opened"), 10000); // time limit 10 sec.
        }
    }
}

void MainWindow::save()
{
    CatalogDialog dlg(CatalogDialog::SAVE, tr("Save map as..."),
                      ngsCatalogObjectType::CAT_FILE_NGMAPDOCUMENT, this);
    int result = dlg.exec();

    if(1 == result) {
        std::string savePath = dlg.getCatalogPath();

        if(ngsMapSave(m_mapModel->mapId(), savePath.c_str()) !=
                ngsCode::COD_SUCCESS) {
            QMessageBox::critical (this, tr("Error"), tr("Map save failed"));
        }
        else {
            statusBar ()->showMessage(tr("Map saved"), 10000); // time limit 10 sec.
        }
    }
}

void MainWindow::load()
{
    // 1. Choose file dialog
    CatalogDialog dlg(CatalogDialog::OPEN, tr("Select data to load"),
                      ngsCatalogObjectType::CAT_FC_ANY, this);
    int result = dlg.exec();

    if(1 == result) {
        std::string shapePath = dlg.getCatalogPath();

        // 2. Show progress dialog
        m_progressDlg = new ProgressDialog(tr("Loading ..."), this);

        const char *options[2] = {"FEATURES_SKIP=EMPTY_GEOMETRY", nullptr};
        char** popt = const_cast<char**>(options);
        ngsProgressFunc func = loadProgressFunction;

        QFuture<int> future = QtConcurrent::run(ngsCatalogObjectLoad,
                                                shapePath.c_str(),
                                                m_storePath.c_str(),
                                                popt,
                                                func,
                                                static_cast<void*>(m_progressDlg));

        m_watcher.setFuture(future);

        m_progressDlg->open();
    }
}

void MainWindow::addMapLayer()
{
    // 1. Choose file dialog
    CatalogDialog dlg(CatalogDialog::OPEN, tr("Select data to add to map"),
                      ngsCatalogObjectType::CAT_RASTER_FC_ANY, this);
    int result = dlg.exec();

    if(1 == result) {
        std::string path = dlg.getCatalogPath();
        std::string name = "Layer " + std::to_string(m_mapModel->rowCount());
        m_mapModel->createLayer(name.c_str(), path.c_str());
    }
}

void MainWindow::removeMapLayer()
{
    QModelIndexList selection = m_mapLayersView->selectionModel()->selectedRows();
    for(const QModelIndex& index : selection) {
        m_mapModel->deleteLayer(index);
    }
}

void MainWindow::loadFinished()
{
    int result = m_watcher.result();
    if(result != ngsCode::COD_SUCCESS &&
            result != ngsCode::COD_CANCELED) {
        QString message = QString(tr("Load to store failed.\nError: %1")).arg(
                    ngsGetLastErrorMessage());
        QMessageBox::critical(this, tr("Error"), message);
    }
    delete m_progressDlg;
    m_progressDlg = nullptr;
}

void MainWindow::about()
{
    QString appVersion(NGGLV_VERSION_STRING);
    QString libVersion(NGS_VERSION);
#ifdef Q_OS_MACOS
    QString format("OpenGL");
#else
    QString format("OpenGL ES");
#endif
    QString message =  QString(tr("The <b>GL View application</b> "
                                  "test %1 rendering (version %2).<p>"
                                  "Compiled with&nbsp;&nbsp;libngstore %3<p>"
                                  "Run with&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;"
                                  "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;libngstore %4")).arg (
                format, appVersion, libVersion, ngsGetVersionString("self"));
    QMessageBox::about(this, tr("About Menu"),
            message);
}

void MainWindow::createActions()
{
    m_pNewAct = new QAction(QIcon(":/images/doc_new.svg"), tr("&New"), this);
    m_pNewAct->setShortcuts(QKeySequence::New);
    m_pNewAct->setStatusTip(tr("Create a new map document"));
    connect(m_pNewAct, SIGNAL(triggered()), this, SLOT(newFile()));

    m_pOpenAct = new QAction(QIcon(":/images/doc_open.svg"), tr("&Open..."), this);
    m_pOpenAct->setShortcuts(QKeySequence::Open);
    m_pOpenAct->setStatusTip(tr("Open an existing map document"));
    connect(m_pOpenAct, SIGNAL(triggered()), this, SLOT(open()));

    m_pSaveAct = new QAction(QIcon(":/images/doc_save.svg"), tr("&Save"), this);
    m_pSaveAct->setShortcuts(QKeySequence::Save);
    m_pSaveAct->setStatusTip(tr("Save the map document to disk"));
    connect(m_pSaveAct, SIGNAL(triggered()), this, SLOT(save()));

    m_pLoadAct = new QAction(tr("&Load"), this);
    m_pLoadAct->setStatusTip(tr("Load spatial data to internal storage"));
    connect(m_pLoadAct, SIGNAL(triggered()), this, SLOT(load()));

    m_pAddLayerAct = new QAction(tr("Add layer"), this);
    m_pAddLayerAct->setStatusTip(tr("Add new layer to map"));
    connect(m_pAddLayerAct, SIGNAL(triggered()), this, SLOT(addMapLayer()));

    m_pExitAct = new QAction(tr("E&xit"), this);
    m_pExitAct->setShortcuts(QKeySequence::Quit);
    m_pExitAct->setStatusTip(tr("Exit the application"));
    connect(m_pExitAct, SIGNAL(triggered()), this, SLOT(close()));

    m_pAboutAct = new QAction(QIcon(":/images/main_logo.svg"), tr("&About"), this);
    m_pAboutAct->setStatusTip(tr("Show the application's About box"));
    m_pAboutAct->setMenuRole(QAction::AboutRole);
    connect(m_pAboutAct, SIGNAL(triggered()), this, SLOT(about()));

    m_pAboutQtAct = new QAction(tr("About &Qt"), this);
    m_pAboutQtAct->setStatusTip(tr("Show the Qt library's About box"));
    m_pAboutQtAct->setMenuRole(QAction::AboutQtRole);
    connect(m_pAboutQtAct, SIGNAL(triggered()), qApp, SLOT(aboutQt()));

}

bool MainWindow::createDatastore()
{
    // Check if datastore exists
    std::string path = ngsFormFileName(ngsGetCurrentDirectory(), "tmp", nullptr);
    std::string catalogPath = ngsCatalogPathFromSystem(path.c_str());
    std::string storeName("main.ngst");
    m_storePath = catalogPath + "/" + storeName;

    if(ngsCatalogObjectGet(m_storePath.c_str()) == nullptr) {
        char** options = nullptr;
        options = ngsAddNameValue(
                    options, "TYPE", std::to_string(
                        ngsCatalogObjectType::CAT_CONTAINER_NGS).c_str());
        options = ngsAddNameValue(options, "CREATE_UNIQUE", "ON");

        return ngsCatalogObjectCreate(catalogPath.c_str(), storeName.c_str(),
                                      options) == ngsCode::COD_SUCCESS;
    }
    return true;
}

void MainWindow::createMenus()
{
    QMenu *pFileMenu = menuBar()->addMenu(tr("&File"));
    pFileMenu->addAction(m_pNewAct);
    pFileMenu->addAction(m_pOpenAct);
    pFileMenu->addAction(m_pSaveAct);
    pFileMenu->addSeparator();
    pFileMenu->addAction(m_pExitAct);

    QMenu *pDataMenu = menuBar()->addMenu(tr("&Data"));
    pDataMenu->addAction(m_pLoadAct);

    QMenu *pMapMenu = menuBar()->addMenu(tr("&Map"));
    pMapMenu->addAction(m_pAddLayerAct);

    QMenu *pHelpMenu = menuBar()->addMenu(tr("&Help"));
    pHelpMenu->addAction(m_pAboutAct);
    pHelpMenu->addAction(m_pAboutQtAct);
}

void MainWindow::createDockWindows()
{
    m_splitter = new QSplitter(Qt::Horizontal);
    m_mapLayersView = new QListView();
    m_mapLayersView->setStyleSheet("QListView { border: none; }");
    m_mapLayersView->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_mapLayersView->setDragEnabled(true);
    m_mapLayersView->setModel(m_mapModel);
    m_mapLayersView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_mapLayersView->setDragDropMode(QAbstractItemView::InternalMove);
    m_mapLayersView->setDefaultDropAction(Qt::MoveAction);
    //m_mapLayersView->setDropIndicatorShown(true);
    m_mapLayersView->setMovement(QListView::Snap);
    m_mapLayersView->setDragDropOverwriteMode(false);
    connect(m_mapLayersView, SIGNAL(customContextMenuRequested(QPoint)), this,
            SLOT(showContextMenu(QPoint)));


    m_splitter->addWidget(m_mapLayersView);

    // mapview setup
    m_mapView = new GlMapView(m_locationStatus, this);
    m_mapView->setModel(m_mapModel);

    m_splitter->addWidget(m_mapView);
    m_splitter->setHandleWidth(1);
    m_splitter->setStretchFactor(1, 3);

    setCentralWidget(m_splitter);
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    // Handle global position
    QPoint globalPos = m_mapLayersView->mapToGlobal(pos);

    // Create menu and insert some actions
    QMenu myMenu;
    myMenu.addAction("Remove", this, SLOT(removeMapLayer()));

    // Show context menu at handling position
    myMenu.exec(globalPos);
}

