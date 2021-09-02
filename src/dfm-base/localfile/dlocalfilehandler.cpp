/*
 * Copyright (C) 2017 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
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
#include "localfile/dlocalfilehandler.h"
#include "private/dfilehandler_p.h"
#include "base/standardpaths.h"

#include <QObject>
#include <QDir>
#include <QDateTime>
#include <QTemporaryFile>

#include <unistd.h>
#include <utime.h>
#include <cstdio>

DLocalFileHandler::DLocalFileHandler()
{

}

bool DLocalFileHandler::exists(const QUrl &url)
{
    Q_ASSERT(url.isLocalFile());

    QFileInfo info(url.toLocalFile());

    return info.exists() || info.isSymLink();
}

bool DLocalFileHandler::touch(const QUrl &url)
{
    Q_ASSERT(url.isLocalFile());

    QFile file(url.toLocalFile());

    if (file.exists())
        return false;

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.close();
        return true;
    }

    Q_D(DFileHandler);

    d->setErrorString(file.errorString());

    return false;
}

bool DLocalFileHandler::mkdir(const QUrl &url)
{
    Q_ASSERT(url.isLocalFile());

    Q_D(DFileHandler);

    if (QDir::current().mkdir(url.toLocalFile())) {
        return true;
    }

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}

bool DLocalFileHandler::mkpath(const QUrl &url)
{
    Q_ASSERT(url.isLocalFile());

    if (QDir::current().mkpath(url.toLocalFile())) {
        return true;
    }

    Q_D(DFileHandler);

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}

bool DLocalFileHandler::link(const QString &path, const QUrl &linkUrl)
{
    Q_ASSERT(linkUrl.isLocalFile());

    if (::symlink(path.toLocal8Bit().constData(), linkUrl.toLocalFile().toLocal8Bit().constData()) == 0)
        return true;

    Q_D(DFileHandler);

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}

bool DLocalFileHandler::remove(const QUrl &url)
{
    Q_ASSERT(url.isLocalFile());

    if (::remove(url.toLocalFile().toLocal8Bit()) == 0)
        return true;

    Q_D(DFileHandler);

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}

bool DLocalFileHandler::rmdir(const QUrl &url)
{
    Q_ASSERT(url.isLocalFile());

    if (::rmdir(url.toLocalFile().toLocal8Bit()) == 0)
        return true;

    Q_D(DFileHandler);

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}

bool DLocalFileHandler::rename(const QUrl &url, const QUrl &newUrl)
{
    Q_ASSERT(url.isLocalFile());
    Q_ASSERT(newUrl.isLocalFile());
    Q_D(DFileHandler);

    const QByteArray &source_file = url.toLocalFile().toLocal8Bit();
    const QByteArray &target_file = newUrl.toLocalFile().toLocal8Bit();

    if (::rename(source_file.constData(), target_file.constData()) == 0)
        return true;

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}

bool DLocalFileHandler::setPermissions(const QUrl &url, QFileDevice::Permissions permissions)
{
    Q_ASSERT(url.isLocalFile());

    QFile file(url.toLocalFile());

    if (file.setPermissions(permissions))
        return true;

    Q_D(DFileHandler);

    d->setErrorString(file.errorString());

    return false;
}

bool DLocalFileHandler::setFileTime(const QUrl &url, const QDateTime &accessDateTime, const QDateTime &lastModifiedTime)
{
    Q_ASSERT(url.isLocalFile());

    utimbuf buf = {
        .actime = accessDateTime.toTime_t(),
        .modtime = lastModifiedTime.toTime_t()
    };

    if (::utime(url.toLocalFile().toLocal8Bit(), &buf) == 0) {
        return true;
    }

    Q_D(DFileHandler);

    d->setErrorString(QString::fromLocal8Bit(strerror(errno)));

    return false;
}
