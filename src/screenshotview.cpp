/*
 * Copyright (C) 2021 CutefishOS Team.
 *
 * Author:     Reion Wong <reionwong@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "screenshotview.h"

#include <QClipboard>
#include <QEventLoop>
#include <QTimer>

#include <QGuiApplication>
#include <QQmlContext>
#include <QScreen>
#include <QPixmap>
#include <QStandardPaths>
#include <QDateTime>

#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QProcess>

#include <QMimeData>
#include <QBuffer>

static void setClipboardImageData(const QImage &image)
{
    QClipboard *clipboard = qGuiApp->clipboard();

    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QString tmpPath = "/tmp/cutefish-latest-screenshot.png";
    QString cachePath = QString("%1/latest-screenshot.png").arg(cacheDir);

    image.save(tmpPath, "PNG");
    image.save(cachePath, "PNG");

    if (clipboard) {
        QMimeData *mimeData = new QMimeData();
        mimeData->setImageData(image);

        QByteArray byteArray;
        QBuffer buffer(&byteArray);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        mimeData->setData("image/png", byteArray);

        QList<QUrl> urls;
        urls.append(QUrl::fromLocalFile(tmpPath));
        urls.append(QUrl::fromLocalFile(cachePath));
        mimeData->setUrls(urls);
        mimeData->setText(tmpPath);

        clipboard->setMimeData(mimeData, QClipboard::Clipboard);
        clipboard->setMimeData(mimeData, QClipboard::Selection);
    }

    // Persist clipboard via xclip & wl-copy shell command execution
    QString cmd = QString("xclip -selection clipboard -t image/png -i '%1' 2>/dev/null || wl-copy --type image/png < '%1' 2>/dev/null").arg(tmpPath);
    QProcess::startDetached("sh", QStringList() << "-c" << cmd);
}

ScreenshotView::ScreenshotView(QQuickView *parent)
    : QQuickView(parent)
{
    rootContext()->setContextProperty("view", this);

    setFlags(Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint);
    setScreen(qGuiApp->primaryScreen());
    setResizeMode(QQuickView::SizeRootObjectToView);
    setSource(QUrl("qrc:/qml/main.qml"));
    setGeometry(screen()->geometry());
}

void ScreenshotView::start()
{
    QFile::remove(QStringLiteral("/tmp/cutefish-screenshot.png"));

    // 1. Try grim (standard Wayland compositor screenshot tool)
    if (QProcess::execute(QStringLiteral("grim"), QStringList() << QStringLiteral("/tmp/cutefish-screenshot.png")) != 0 || !QFile::exists(QStringLiteral("/tmp/cutefish-screenshot.png"))) {
        // 2. Try spectacle
        QProcess pr;
        pr.start(QStringLiteral("spectacle"), QStringList() << QStringLiteral("-b") << QStringLiteral("-n") << QStringLiteral("-o") << QStringLiteral("/tmp/cutefish-screenshot.png"));
        if (!pr.waitForFinished(1500) || !QFile::exists(QStringLiteral("/tmp/cutefish-screenshot.png"))) {
            // 3. Fallback to QScreen::grabWindow
            QPixmap p = qGuiApp->primaryScreen()->grabWindow(0);
            p.save(QStringLiteral("/tmp/cutefish-screenshot.png"));
        }
    }

    setVisible(true);
    setKeyboardGrabEnabled(true);

    emit refresh();
}

void ScreenshotView::delay(int value)
{
    QEventLoop waitLoop;
    QTimer::singleShot(value, &waitLoop, SLOT(quit()));
    waitLoop.exec();

    start();
}

void ScreenshotView::quit()
{
    qGuiApp->quit();
}

void ScreenshotView::saveFile(QRect rect)
{
    setVisible(false);

    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString fileName = QString("%1/Screenshot_%2.png")
                              .arg(desktopPath)
                              .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QImage image("/tmp/cutefish-screenshot.png");
    if (image.isNull()) {
        QPixmap p = qGuiApp->primaryScreen()->grabWindow(0);
        image = p.toImage();
    }

    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    QRect cropRect(rect.x() * dpr, rect.y() * dpr, rect.width() * dpr, rect.height() * dpr);
    if (cropRect.isEmpty() || cropRect.width() <= 0 || cropRect.height() <= 0) {
        cropRect = image.rect();
    } else {
        cropRect = cropRect.intersected(image.rect());
    }

    QImage cropped = image.copy(cropRect);
    bool saved = cropped.save(fileName);

    // Always keep last taken screenshot in clipboard
    setClipboardImageData(cropped);

    if (saved) {
        QDBusInterface iface("org.freedesktop.Notifications",
                             "/org/freedesktop/Notifications",
                             "org.freedesktop.Notifications",
                             QDBusConnection::sessionBus());
        if (iface.isValid()) {
            QList<QVariant> args;
            args << "cutefish-screenshot";
            args << ((unsigned int) 0);
            args << "cutefish-screenshot";
            args << "";
            args << tr("The picture has been saved to %1 and copied to clipboard").arg(fileName);
            args << QStringList();
            args << QVariantMap();
            args << (int) 10;
            iface.asyncCallWithArgumentList("Notify", args);
        }
    }

    removeTmpFile();
    QTimer::singleShot(600, qGuiApp, &QGuiApplication::quit);
}

void ScreenshotView::copyToClipboard(QRect rect)
{
    setVisible(false);

    QImage image("/tmp/cutefish-screenshot.png");
    if (image.isNull()) {
        QPixmap p = qGuiApp->primaryScreen()->grabWindow(0);
        image = p.toImage();
    }

    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    QRect cropRect(rect.x() * dpr, rect.y() * dpr, rect.width() * dpr, rect.height() * dpr);
    if (cropRect.isEmpty() || cropRect.width() <= 0 || cropRect.height() <= 0) {
        cropRect = image.rect();
    } else {
        cropRect = cropRect.intersected(image.rect());
    }

    QImage cropped = image.copy(cropRect);
    setClipboardImageData(cropped);

    QDBusInterface iface("org.freedesktop.Notifications",
                         "/org/freedesktop/Notifications",
                         "org.freedesktop.Notifications",
                         QDBusConnection::sessionBus());
    if (iface.isValid()) {
        QList<QVariant> args;
        args << "cutefish-screenshot";
        args << ((unsigned int) 0);
        args << "cutefish-screenshot";
        args << "";
        args << tr("The picture has been saved to the clipboard");
        args << QStringList();
        args << QVariantMap();
        args << (int) 10;
        iface.asyncCallWithArgumentList("Notify", args);
    }

    removeTmpFile();

    QTimer::singleShot(200, qGuiApp, &QGuiApplication::quit);
}

void ScreenshotView::removeTmpFile()
{
    QFile("/tmp/cutefish-screenshot.png").remove();
}
