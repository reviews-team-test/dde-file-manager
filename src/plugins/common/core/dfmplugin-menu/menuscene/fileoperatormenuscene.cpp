/*
 * Copyright (C) 2022 Uniontech Software Technology Co., Ltd.
 *
 * Author:     liqiang<liqianga@uniontech.com>
 *
 * Maintainer: liqiang<liqianga@uniontech.com>
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
#include "private/fileoperatormenuscene_p.h"
#include "action_defines.h"
#include "menuutils.h"

#include "dfm-base/dfm_menu_defines.h"
#include "dfm-base/base/schemefactory.h"
#include "dfm-base/interfaces/abstractjobhandler.h"
#include "dfm-base/dfm_event_defines.h"
#include "dfm-base/mimetype/mimesappsmanager.h"
#include "dfm-base/utils/properties.h"
#include "dfm-base/utils/fileutils.h"
#include "dfm-base/base/standardpaths.h"
#include "dfm-base/base/application/application.h"

#include <dfm-framework/dpf.h>

#include <QMenu>
#include <QVariant>
#include <QSettings>
#include <QFileDialog>
#include <QGuiApplication>

using namespace dfmplugin_menu;
DFMBASE_USE_NAMESPACE

AbstractMenuScene *FileOperatorMenuCreator::create()
{
    return new FileOperatorMenuScene();
}

FileOperatorMenuScenePrivate::FileOperatorMenuScenePrivate(FileOperatorMenuScene *qq)
    : AbstractMenuScenePrivate(qq)
{
    predicateName[ActionID::kOpen] = tr("Open");
    predicateName[ActionID::kRename] = tr("Rename");
    predicateName[ActionID::kDelete] = tr("Delete");
    predicateName[ActionID::kEmptyTrash] = tr("Empty Trash");
    predicateName[ActionID::kSetAsWallpaper] = tr("Set as wallpaper");
}

FileOperatorMenuScene::FileOperatorMenuScene(QObject *parent)
    : AbstractMenuScene(parent),
      d(new FileOperatorMenuScenePrivate(this))
{
}

QString FileOperatorMenuScene::name() const
{
    return FileOperatorMenuCreator::name();
}

bool FileOperatorMenuScene::initialize(const QVariantHash &params)
{
    d->currentDir = params.value(MenuParamKey::kCurrentDir).toUrl();
    d->selectFiles = params.value(MenuParamKey::kSelectFiles).value<QList<QUrl>>();
    if (!d->selectFiles.isEmpty())
        d->focusFile = d->selectFiles.first();
    d->onDesktop = params.value(MenuParamKey::kOnDesktop).toBool();
    d->isEmptyArea = params.value(MenuParamKey::kIsEmptyArea).toBool();
    d->indexFlags = params.value(MenuParamKey::kIndexFlags).value<Qt::ItemFlags>();
    d->windowId = params.value(MenuParamKey::kWindowId).toULongLong();

    const auto &tmpParams = dfmplugin_menu::MenuUtils::perfectMenuParams(params);
    d->isFocusOnDDEDesktopFile = tmpParams.value(MenuParamKey::kIsFocusOnDDEDesktopFile, false).toBool();
    d->isSystemPathIncluded = tmpParams.value(MenuParamKey::kIsSystemPathIncluded, false).toBool();

    if (!d->initializeParamsIsValid()) {
        qWarning() << "menu scene:" << name() << " init failed." << d->selectFiles.isEmpty() << d->focusFile << d->currentDir;
        return false;
    }

    if (!d->isEmptyArea) {
        QString errString;
        d->focusFileInfo = DFMBASE_NAMESPACE::InfoFactory::create<AbstractFileInfo>(d->focusFile, true, &errString);
        if (d->focusFileInfo.isNull()) {
            qDebug() << errString;
            return false;
        }
    }

    return AbstractMenuScene::initialize(params);
}

AbstractMenuScene *FileOperatorMenuScene::scene(QAction *action) const
{
    if (action == nullptr)
        return nullptr;

    if (d->predicateAction.values().contains(action))
        return const_cast<FileOperatorMenuScene *>(this);

    return AbstractMenuScene::scene(action);
}

bool FileOperatorMenuScene::create(QMenu *parent)
{
    if (d->isEmptyArea)
        return true;

    QAction *tempAction = parent->addAction(d->predicateName.value(ActionID::kOpen));
    d->predicateAction[ActionID::kOpen] = tempAction;
    tempAction->setProperty(ActionPropertyKey::kActionID, QString(ActionID::kOpen));

    if (d->selectFiles.count() == 1) {
        auto focusFileInfo = d->focusFileInfo;
        if (d->focusFileInfo->isSymLink()) {
            const auto &targetFile = d->focusFileInfo->pathInfo(AbstractFileInfo::FilePathInfoType::kSymLinkTarget);
            auto targetFileInfo = InfoFactory::create<AbstractFileInfo>(QUrl::fromLocalFile(targetFile));
            if (targetFileInfo && targetFileInfo->exists())
                focusFileInfo = targetFileInfo;
        }

        const auto mimeType = focusFileInfo->nameInfo(AbstractFileInfo::FileNameInfoType::kMimeTypeName);
        if (mimeType.startsWith("image") && focusFileInfo->isReadable()
            && !mimeType.endsWith("svg+xml") && !mimeType.endsWith("raf")
            && !mimeType.endsWith("crw")) {
            tempAction = parent->addAction(d->predicateName.value(ActionID::kSetAsWallpaper));
            d->predicateAction[ActionID::kSetAsWallpaper] = tempAction;
            tempAction->setProperty(ActionPropertyKey::kActionID, QString(ActionID::kSetAsWallpaper));
        }
    }

    if (FileUtils::isTrashDesktopFile(d->focusFile)) {
        tempAction = parent->addAction(d->predicateName.value(ActionID::kEmptyTrash));
        d->predicateAction[ActionID::kEmptyTrash] = tempAction;
        tempAction->setProperty(ActionPropertyKey::kActionID, QString(ActionID::kEmptyTrash));
    } else if (!d->isFocusOnDDEDesktopFile && !d->isSystemPathIncluded) {
        tempAction = parent->addAction(d->predicateName.value(ActionID::kRename));
        d->predicateAction[ActionID::kRename] = tempAction;
        tempAction->setProperty(ActionPropertyKey::kActionID, QString(ActionID::kRename));

        tempAction = parent->addAction(d->predicateName.value(ActionID::kDelete));
        d->predicateAction[ActionID::kDelete] = tempAction;
        tempAction->setProperty(ActionPropertyKey::kActionID, QString(ActionID::kDelete));
    }

    return AbstractMenuScene::create(parent);
}

void FileOperatorMenuScene::updateState(QMenu *parent)
{
    if (!parent)
        return;

    if (d->isEmptyArea)
        return;

    if (FileUtils::isTrashDesktopFile(d->focusFile)) {
        if (auto clearTrash = d->predicateAction.value(ActionID::kEmptyTrash)) {
            auto info = InfoFactory::create<AbstractFileInfo>(FileUtils::trashRootUrl());
            if (info->countChildFile() <= 0)
                clearTrash->setDisabled(true);
        }
    }

    d->focusFileInfo->refresh();
    // delete
    if (auto delAction = d->predicateAction.value(ActionID::kDelete)) {
        if (!d->focusFileInfo->canDelete() || (!d->focusFileInfo->isWritable() && !d->focusFileInfo->isFile() && !d->focusFileInfo->isSymLink()))
            delAction->setDisabled(true);
    }

    // rename
    if (auto rename = d->predicateAction.value(ActionID::kRename)) {
        if (!d->focusFileInfo->canRename() || !d->indexFlags.testFlag(Qt::ItemIsEditable))
            rename->setDisabled(true);
    }
    if (d->selectFiles.count() > 1) {
        // open
        if (auto open = d->predicateAction.value(ActionID::kOpen)) {

            // app support mime types
            QStringList supportedMimeTypes;
            QMimeType fileMimeType = d->focusFileInfo->fileMimeType();
            QString defaultAppDesktopFile = MimesAppsManager::getDefaultAppDesktopFileByMimeType(fileMimeType.name());
            QSettings desktopFile(defaultAppDesktopFile, QSettings::IniFormat);
            desktopFile.setIniCodec("UTF-8");
            Properties mimeTypeProperties(defaultAppDesktopFile, "Desktop Entry");
            supportedMimeTypes = mimeTypeProperties.value("MimeType").toString().split(';');
            supportedMimeTypes.removeAll("");

            QString errString;
            QList<QUrl> redirectedUrls;

            for (auto url : d->selectFiles) {
                auto info = DFMBASE_NAMESPACE::InfoFactory::create<AbstractFileInfo>(url, true, &errString);
                if (Q_UNLIKELY(info.isNull())) {
                    qDebug() << errString;
                    break;
                }

                // if the suffix is the same, it can be opened with the same application
                if (info->nameInfo(AbstractFileInfo::FileNameInfoType::kSuffix) != d->focusFileInfo->nameInfo(AbstractFileInfo::FileNameInfoType::kSuffix)) {

                    QStringList mimeTypeList { info->nameInfo(AbstractFileInfo::FileNameInfoType::kMimeTypeName) };
                    QUrl parentUrl = info->urlInfo(AbstractFileInfo::FileUrlInfoType::kParentUrl);
                    auto parentInfo = DFMBASE_NAMESPACE::InfoFactory::create<AbstractFileInfo>(url, true, &errString);
                    if (!info.isNull()) {
                        mimeTypeList << parentInfo->nameInfo(AbstractFileInfo::FileNameInfoType::kMimeTypeName);
                    }

                    bool matched = false;
                    // or,the application suooprt mime type contains the type of the url file mime type
                    for (const QString &oneMimeType : mimeTypeList) {
                        if (supportedMimeTypes.contains(oneMimeType)) {
                            matched = true;
                            break;
                        }
                    }

                    // disable open action when there are different opening methods
                    if (!matched) {
                        open->setDisabled(true);
                        break;
                    }
                }
            }
        }
    }

    AbstractMenuScene::updateState(parent);
}

bool FileOperatorMenuScene::triggered(QAction *action)
{
    if (!d->predicateAction.values().contains(action))
        return AbstractMenuScene::triggered(action);

    auto actionId = action->property(ActionPropertyKey::kActionID).toString();

    // open
    if (actionId == ActionID::kOpen) {
        if (!d->onDesktop && 1 == d->selectFiles.count() && d->focusFileInfo->isDir()) {
            if (Application::instance()->appAttribute(Application::kAllwayOpenOnNewWindow).toBool()) {
                dpfSignalDispatcher->publish(GlobalEventType::kOpenNewWindow, d->focusFile);
            } else {
                dpfSignalDispatcher->publish(GlobalEventType::kChangeCurrentUrl, d->windowId, d->focusFile);
            }
        } else {
            dpfSignalDispatcher->publish(GlobalEventType::kOpenFiles, d->windowId, d->selectFiles);
        }

        return true;
    }

    // rename
    if (actionId == ActionID::kRename) {
    }

    // delete
    if (actionId == ActionID::kDelete) {
        if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
            dpfSignalDispatcher->publish(GlobalEventType::kDeleteFiles, d->windowId, d->selectFiles, AbstractJobHandler::JobFlag::kNoHint, nullptr);
        } else {
            dpfSignalDispatcher->publish(GlobalEventType::kMoveToTrash, d->windowId, d->selectFiles, AbstractJobHandler::JobFlag::kNoHint, nullptr);
        }
        return true;
    }

    // clear trash
    if (actionId == ActionID::kEmptyTrash) {
        dpfSignalDispatcher->publish(GlobalEventType::kCleanTrash,
                                     d->windowId,
                                     QList<QUrl>(),
                                     AbstractJobHandler::DeleteDialogNoticeType::kEmptyTrash, nullptr);
        return true;
    }

    // set as wallpaper
    if (actionId == ActionID::kSetAsWallpaper) {
        if (UrlRoute::isVirtual(d->focusFile)) {
            const auto &localFile = d->focusFileInfo->pathInfo(AbstractFileInfo::FilePathInfoType::kAbsoluteFilePath);
            FileUtils::setBackGround(localFile);
        } else {
            FileUtils::setBackGround(d->focusFile.toLocalFile());
        }
        return true;
    }

    // TODO(Lee or others):
    return AbstractMenuScene::triggered(action);
}
