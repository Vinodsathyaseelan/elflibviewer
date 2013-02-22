/* Copyright © 2007, 2009 Michael Pyne <michael.pyne@kdemail.net>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include <QtCore>
#include <QtGui>

#include "MainWindowImpl.h"

const char *versionString = "0.9";

class MainWindowImpl::Private
{
    friend class MainWindowImpl;

    QStringList basePaths;
    QStringList envPaths;
    QTimer highlightLibrariesTimer;
};

MainWindowImpl::MainWindowImpl(QWidget *parent) :
  QMainWindow(parent), m_ui(new Ui::MainWindow), m_model(0), d(new Private)
{
    m_ui->setupUi(this);

    m_model = new QStandardItemModel(this);
    m_ui->libView->setModel(m_model);

    d->basePaths = getBasePaths();
    d->envPaths = getSystemEnvPaths();

    // Used to automatically highlight libraries if the user stops typing but hasn't
    // hit enter or return.
    d->highlightLibrariesTimer.setInterval(1000);
    d->highlightLibrariesTimer.setSingleShot(true);
    connect(&d->highlightLibrariesTimer, SIGNAL(timeout()), SLOT(highlightMatchingLibraries()));
}

void MainWindowImpl::openFile(const QString &fileName)
{
    if(fileName.isEmpty())
        return;

    m_model->clear();
    QStringList list;
    list << "Shared Object" << "Resolved Path";
    m_model->setHorizontalHeaderLabels(list);

    QFileInfo fi(fileName);

    QList<QStandardItem *> items;
    items << new QStandardItem(fi.fileName());
    items << new QStandardItem(fileName);

    m_model->invisibleRootItem()->appendRow(items);
    addFile(fileName, m_model->item(0));
    m_ui->libView->expand(m_model->indexFromItem(items[0]));
    m_ui->libView->resizeColumnToContents(0);

    setWindowFilePath(fileName);
}

void MainWindowImpl::on_actionOpen_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this, "Open executable/library");
    openFile(fileName);
}

void MainWindowImpl::on_actionQuit_triggered()
{
    emit quit();
}

void MainWindowImpl::addFile(const QString &fileName, QStandardItem *root)
{
    QList<QStandardItem *> items;

    QProcess readelf;

    readelf.start("readelf", QStringList() << "-d" << QFile::encodeName(fileName));
    if(!readelf.waitForFinished())
        return;

    QTextStream ts(&readelf);
    QRegExp soPat("\\[(.*)\\]$");
    LibSearchInfo info;

    // List of libraries to search for.  We cannot search as soon as its
    // encountered because we must wait until we've seen rPath and runPath.
    QStringList libs;

    while(!ts.atEnd()) {
        QString str = ts.readLine();

        if(str.contains("(RPATH)") && soPat.indexIn(str) != -1)
            info.rPath = soPat.cap(1);
        else if(str.contains("(RUNPATH)") && soPat.indexIn(str) != -1)
            info.runPath = soPat.cap(1);
        else if(str.contains("(NEEDED)") && soPat.indexIn(str) != -1)
            libs << soPat.cap(1);
    }

    foreach(QString soname, libs) {
        bool hadLib = m_libs.contains(soname);
        QString lib = resolveLibrary(soname, info);
        items << new QStandardItem(soname);

        if(lib.isEmpty()) {
            items << new QStandardItem("not found");

            QFont f = items[0]->font();
            f.setItalic(true);

            items[0]->setFont(f);
            items[1]->setFont(f);
        }
        else
            items << new QStandardItem(resolveLibrary(soname, info));

        root->appendRow(items);

        if(!lib.isEmpty() && !hadLib)
            addFile(lib, root->child(root->rowCount() - 1, 0));

        items.clear();
    }
}

QString MainWindowImpl::resolveLibrary(const QString &library, const LibSearchInfo &info)
{
    if(m_libs.contains(library))
        return m_libs[library];

    // Create search path
    QStringList paths;

    // RPATH was set, RUNPATH was not, look in RPATH first.
    if(info.runPath.isEmpty() && !info.rPath.isEmpty())
        paths << info.rPath.split(":");

    paths << d->envPaths;

    if(!info.runPath.isEmpty())
        paths << info.runPath.split(":");

    paths << d->basePaths;
    paths << "/lib" << "/usr/lib";

    foreach(QString path, paths) {
        if(QFile::exists(path + "/" + library)) {
            // Resolve symlink.
            QFileInfo qfi(path + "/" + library);
            m_libs[library] = qfi.canonicalFilePath();

            return qfi.canonicalFilePath();
        }
    }

    return QString();
}

void MainWindowImpl::resetItems(QStandardItem *root)
{
    for(int i = 0; i < root->rowCount(); ++i)
        resetItems(root->child(i, 0));

    root->setForeground(Qt::black);
}

void MainWindowImpl::on_actionAbout_triggered()
{
    QMessageBox::about(this, "About elflibviewer",
        QString::fromUtf8("elflibviewer is a program to display the required library dependencies of"
        " a program or shared library (in ELF format).  It requires the readelf"
        " tool.\n\nCopyright © 2007, 2009 Michael Pyne.  This program may be distributed"
        " under the terms of the GNU GPL v2 (or any later version).\n\n"
        "Icons are from the KDE 4 Oxygen icon set, distributed under the "
        "LGPL, version 2."
        "\n\nVersion: %1").arg(versionString));
}

void MainWindowImpl::highlightMatchingLibraries()
{
    QString findText = m_ui->libSearchName->text();

    resetItems(m_model->invisibleRootItem());
    if(findText.isEmpty())
        return;

    QList<QStandardItem *> results = m_model->findItems(findText, Qt::MatchContains | Qt::MatchRecursive);
    foreach(QStandardItem *i, results) {
        i->setForeground(Qt::red);
        QStandardItem *item = i->parent();
        while(item) {
            item->setForeground(Qt::red);
            item = item->parent();
        }
    }
}

void MainWindowImpl::restartTimer()
{
    d->highlightLibrariesTimer.start();
}

QStringList MainWindowImpl::getBasePaths() const
{
    // Open system file defining standard library paths.
    QFile ldConf("/etc/ld.so.conf");

    if(!ldConf.open(QIODevice::ReadOnly))
        return QStringList();

    QTextStream in(&ldConf);
    QHash<QString, bool> foundMap;
    QStringList paths;

    // Loop through each path.  Resolve it to the actual file, and if not
    // already in the path, add it at the end.
    while(!in.atEnd()) {
        QString lib = in.readLine();
        QFileInfo libInfo(lib);
        QString realPath = libInfo.canonicalFilePath();

        if(foundMap.contains(realPath))
            continue;

        foundMap[realPath] = true;
        paths << realPath;
    }

    return paths;
}

QStringList MainWindowImpl::getSystemEnvPaths() const
{
    QStringList envPath = QProcess::systemEnvironment();
    foreach(QString s, envPath) {
        if(s.startsWith("LD_LIBRARY_PATH=")) {
            s.remove("LD_LIBRARY_PATH=");
            return s.split(":");
        }
    }

    return QStringList();
}

// vim: set ts=8 sw=4 et encoding=utf8: