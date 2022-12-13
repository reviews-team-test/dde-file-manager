/*
 * Copyright (C) 2021 Uniontech Software Technology Co., Ltd.
 *
 * Author:     xushitong<xushitong@uniontech.com>
 *
 * Maintainer: max-lv<lvwujun@uniontech.com>
 *             lanxuesong<lanxuesong@uniontech.com>
 *             zhangsheng<zhangsheng@uniontech.com>
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
#include "computeritemwatcher.h"
#include "controller/computercontroller.h"
#include "utils/computerutils.h"
#include "utils/stashmountsutils.h"
#include "fileentity/appentryfileentity.h"
#include "fileentity/stashedprotocolentryfileentity.h"

#include "dfm-base/dfm_global_defines.h"
#include "dfm-base/base/configs/configsynchronizer.h"
#include "dfm-base/base/configs/dconfig/dconfigmanager.h"
#include "dfm-base/dbusservice/global_server_defines.h"
#include "dfm-base/dbusservice/dbus_interface/devicemanagerdbus_interface.h"
#include "dfm-base/utils/universalutils.h"
#include "dfm-base/utils/sysinfoutils.h"
#include "dfm-base/file/entry/entryfileinfo.h"
#include "dfm-base/file/local/localfilewatcher.h"
#include "dfm-base/base/device/deviceproxymanager.h"
#include "dfm-base/base/device/deviceutils.h"
#include "dfm-base/base/schemefactory.h"
#include "dfm-base/base/application/application.h"
#include "dfm-base/base/standardpaths.h"
#include "dfm-base/dbusservice/global_server_defines.h"

#include <dfm-framework/event/event.h>

#include <QDebug>
#include <QApplication>
#include <QWindow>

using ItemClickedActionCallback = std::function<void(quint64 windowId, const QUrl &url)>;
using ContextMenuCallback = std::function<void(quint64 windowId, const QUrl &url, const QPoint &globalPos)>;
using RenameCallback = std::function<void(quint64 windowId, const QUrl &url, const QString &name)>;
using FindMeCallback = std::function<bool(const QUrl &itemUrl, const QUrl &targetUrl)>;
Q_DECLARE_METATYPE(ItemClickedActionCallback);
Q_DECLARE_METATYPE(ContextMenuCallback);
Q_DECLARE_METATYPE(RenameCallback);
Q_DECLARE_METATYPE(FindMeCallback);
Q_DECLARE_METATYPE(QList<QUrl> *);

DFMBASE_USE_NAMESPACE

namespace dfmplugin_computer {
using namespace GlobalServerDefines;

/*!
 * \class ComputerItemWatcher
 * \brief watches the change of computer item
 */
ComputerItemWatcher *ComputerItemWatcher::instance()
{
    static ComputerItemWatcher watcher;
    return &watcher;
}

ComputerItemWatcher::ComputerItemWatcher(QObject *parent)
    : QObject(parent)
{
    initAppWatcher();
    initConn();
}

ComputerItemWatcher::~ComputerItemWatcher()
{
}

ComputerDataList ComputerItemWatcher::items()
{
    ComputerDataList ret;

    ret.append(getUserDirItems());

    // these are all in Disk group
    bool hasInsertNewDisk = false;
    ret.push_back(getGroup(kGroupDisks));
    int diskStartPos = ret.count();

    ret.append(getBlockDeviceItems(hasInsertNewDisk));
    ComputerDataList protocolDevices = getProtocolDeviceItems(hasInsertNewDisk);
    ret.append(protocolDevices);
    ret.append(getStashedProtocolItems(hasInsertNewDisk, protocolDevices));
    ret.append(getAppEntryItems(hasInsertNewDisk));

    std::sort(ret.begin() + diskStartPos, ret.end(), ComputerItemWatcher::typeCompare);

    if (!hasInsertNewDisk)
        ret.pop_back();

    QList<QUrl> computerItems;
    for (const auto &item : ret)
        computerItems << item.url;

    qDebug() << "computer: [LIST] filter items BEFORE add them: " << computerItems;
    dpfHookSequence->run("dfmplugin_computer", "hook_ComputerView_ItemListFilter", &computerItems);
    qDebug() << "computer: [LIST] items are filtered by external plugins: " << computerItems;
    for (int i = ret.count() - 1; i >= 0; --i) {
        if (!computerItems.contains(ret[i].url))
            ret.removeAt(i);
    }

    return ret;
}

ComputerDataList ComputerItemWatcher::getInitedItems()
{
    return initedDatas;
}

bool ComputerItemWatcher::typeCompare(const ComputerItemData &a, const ComputerItemData &b)
{
    return ComputerUtils::sortItem(a.info, b.info);
}

void ComputerItemWatcher::initConn()
{
    connect(appEntryWatcher.data(), &LocalFileWatcher::subfileCreated, this, [this](const QUrl &url) {
        auto appUrl = ComputerUtils::makeAppEntryUrl(url.path());
        if (!appUrl.isValid())
            return;
        this->onDeviceAdded(appUrl, getGroupId(diskGroup()), ComputerItemData::kLargeItem, false);
    });
    connect(appEntryWatcher.data(), &LocalFileWatcher::fileDeleted, this, [this](const QUrl &url) {
        auto appUrl = ComputerUtils::makeAppEntryUrl(url.path());
        if (!appUrl.isValid())
            return;
        removeDevice(appUrl);
    });

    connect(Application::instance(), &Application::genericAttributeChanged, this, &ComputerItemWatcher::onGenAttributeChanged);
    connect(DConfigManager::instance(), &DConfigManager::valueChanged, this, &ComputerItemWatcher::onDConfigChanged);

    initDeviceConn();
    connect(DevProxyMng, &DeviceProxyManager::devMngDBusRegistered, this, [this]() { startQueryItems(); });
}

void ComputerItemWatcher::initDeviceConn()
{
    connect(DevProxyMng, &DeviceProxyManager::blockDevAdded, this, &ComputerItemWatcher::onBlockDeviceAdded);
    connect(DevProxyMng, &DeviceProxyManager::blockDevRemoved, this, &ComputerItemWatcher::onBlockDeviceRemoved);
    connect(DevProxyMng, &DeviceProxyManager::blockDevMounted, this, &ComputerItemWatcher::onBlockDeviceMounted);
    connect(DevProxyMng, &DeviceProxyManager::blockDevUnmounted, this, &ComputerItemWatcher::onBlockDeviceUnmounted);
    connect(DevProxyMng, &DeviceProxyManager::blockDevLocked, this, &ComputerItemWatcher::onBlockDeviceLocked);
    connect(DevProxyMng, &DeviceProxyManager::blockDevUnlocked, this, &ComputerItemWatcher::onUpdateBlockItem);
    connect(DevProxyMng, &DeviceProxyManager::blockDevPropertyChanged, this, &ComputerItemWatcher::onDevicePropertyChangedQVar);
    connect(DevProxyMng, &DeviceProxyManager::protocolDevMounted, this, &ComputerItemWatcher::onProtocolDeviceMounted);
    connect(DevProxyMng, &DeviceProxyManager::protocolDevUnmounted, this, &ComputerItemWatcher::onProtocolDeviceUnmounted);
    connect(DevProxyMng, &DeviceProxyManager::devSizeChanged, this, &ComputerItemWatcher::onDeviceSizeChanged);
    //    connect(&DeviceManagerInstance, &DeviceManager::protocolDevAdded, this, &ComputerItemWatcher::);
    connect(DevProxyMng, &DeviceProxyManager::protocolDevRemoved, this, &ComputerItemWatcher::onProtocolDeviceRemoved);
}

void ComputerItemWatcher::initAppWatcher()
{
    QUrl extensionUrl;
    extensionUrl.setScheme(Global::Scheme::kFile);
    extensionUrl.setPath(StandardPaths::location(StandardPaths::kExtensionsAppEntryPath));
    appEntryWatcher.reset(new LocalFileWatcher(extensionUrl, this));
    appEntryWatcher->startWatcher();
}

ComputerDataList ComputerItemWatcher::getUserDirItems()
{
    ComputerDataList ret;
    bool userDirAdded = false;
    ret.push_back(getGroup(kGroupDirs));

    static const QStringList udirs = { "desktop", "videos", "music", "pictures", "documents", "downloads" };
    for (auto dir : udirs) {
        QUrl url;
        url.setScheme(DFMBASE_NAMESPACE::Global::Scheme::kEntry);
        url.setPath(QString("%1.%2").arg(dir).arg(SuffixInfo::kUserDir));
        //        auto info = InfoFactory::create<EntryFileInfo>(url);
        DFMEntryFileInfoPointer info(new EntryFileInfo(url));
        if (!info->exists()) continue;

        ComputerItemData data;
        data.url = url;
        data.shape = ComputerItemData::kSmallItem;
        data.info = info;
        data.groupId = getGroupId(userDirGroup());
        ret.push_back(data);
        userDirAdded = true;
    }
    if (!userDirAdded)
        ret.pop_back();
    return ret;
}

ComputerDataList ComputerItemWatcher::getBlockDeviceItems(bool &hasNewItem)
{
    ComputerDataList ret;
    QStringList devs;
    devs = DevProxyMng->getAllBlockIds();

    for (const auto &dev : devs) {
        auto devUrl = ComputerUtils::makeBlockDevUrl(dev);
        //        auto info = InfoFactory::create<EntryFileInfo>(devUrl);
        DFMEntryFileInfoPointer info(new EntryFileInfo(devUrl));
        if (!info->exists())
            continue;

        ComputerItemData data;
        data.url = devUrl;
        data.shape = ComputerItemData::kLargeItem;
        data.info = info;
        data.groupId = getGroupId(diskGroup());
        ret.push_back(data);
        hasNewItem = true;

        if (info->targetUrl().isValid())
            insertUrlMapper(dev, info->targetUrl());

        addSidebarItem(info);
    }

    return ret;
}

ComputerDataList ComputerItemWatcher::getProtocolDeviceItems(bool &hasNewItem)
{
    ComputerDataList ret;
    QStringList devs;
    devs = DevProxyMng->getAllProtocolIds();

    for (const auto &dev : devs) {
        auto devUrl = ComputerUtils::makeProtocolDevUrl(dev);
        //        auto info = InfoFactory::create<EntryFileInfo>(devUrl);
        DFMEntryFileInfoPointer info(new EntryFileInfo(devUrl));
        if (!info->exists())
            continue;

        ComputerItemData data;
        data.url = devUrl;
        data.shape = ComputerItemData::kLargeItem;
        data.info = info;
        data.groupId = getGroupId(diskGroup());
        ret.push_back(data);
        hasNewItem = true;

        addSidebarItem(info);
    }

    return ret;
}

ComputerDataList ComputerItemWatcher::getStashedProtocolItems(bool &hasNewItem, const ComputerDataList &protocolDevs)
{
    auto hasProtocolDev = [](const QUrl &url, const ComputerDataList &container) {
        for (auto dev : container) {
            if (dev.url == url)
                return true;
        }
        return false;
    };
    ComputerDataList ret;

    const QMap<QString, QString> &&stashedMounts = StashMountsUtils::stashedMounts();

    auto isStashedSmbBeMounted = [](const QUrl &smbUrl) {   //TOOD(zhuangshu):do it out of computer plugin
        QUrl url(smbUrl);
        if (url.scheme() != Global::Scheme::kSmb)
            return false;

        QString temPath = url.path();
        QStringList devs = DevProxyMng->getAllProtocolIds();
        for (const QString &dev : devs) {
            if (dev.startsWith(Global::Scheme::kSmb)) {   //mounted by gvfs
                if (UniversalUtils::urlEquals(url, QUrl(dev)))
                    return true;
            }
            if (DeviceUtils::isSamba(dev)) {   // mounted by cifs
                if (temPath.endsWith("/")) {
                    temPath.chop(1);
                    url.setPath(temPath);
                }
                const QUrl &temUrl = QUrl::fromPercentEncoding(dev.toUtf8());
                const QString &path = temUrl.path();
                int pos = path.lastIndexOf("/");
                const QString &displayName = path.mid(pos + 1);
                if (QString("%1 on %2").arg(url.fileName()).arg(url.host()) == displayName)
                    return true;
            }
        }

        return false;
    };

    for (auto iter = stashedMounts.cbegin(); iter != stashedMounts.cend(); ++iter) {
        QUrl protocolUrl;
        if (iter.key().startsWith(Global::Scheme::kSmb)) {
            QUrl temUrl(iter.key());
            if (isStashedSmbBeMounted(temUrl))
                continue;
        } else {
            protocolUrl = ComputerUtils::makeProtocolDevUrl(iter.key());
        }

        if (hasProtocolDev(protocolUrl, ret) || hasProtocolDev(protocolUrl, protocolDevs))
            continue;

        const QUrl &stashedUrl = ComputerUtils::makeStashedProtocolDevUrl(iter.key());

        DFMEntryFileInfoPointer info(new EntryFileInfo(stashedUrl));
        if (!info->exists())
            continue;

        ComputerItemData data;
        data.url = stashedUrl;
        data.shape = ComputerItemData::kLargeItem;
        data.info = info;
        data.groupId = getGroupId(diskGroup());
        ret.push_back(data);

        addSidebarItem(info);

        hasNewItem = true;
    }

    return ret;
}

ComputerDataList ComputerItemWatcher::getAppEntryItems(bool &hasNewItem)
{
    static const QString appEntryPath = StandardPaths::location(StandardPaths::kExtensionsAppEntryPath);
    QDir appEntryDir(appEntryPath);
    if (!appEntryDir.exists())
        return {};

    ComputerDataList ret;

    auto entries = appEntryDir.entryList(QDir::Files);
    QStringList cmds;   // for de-duplication
    for (auto entry : entries) {
        auto entryUrl = ComputerUtils::makeAppEntryUrl(QString("%1/%2").arg(appEntryPath).arg(entry));
        if (!entryUrl.isValid())
            continue;

        DFMEntryFileInfoPointer info(new EntryFileInfo(entryUrl));
        if (!info->exists()) {
            qInfo() << "the appentry is in extension folder but not exist: " << info->urlOf(UrlInfoType::kUrl);
            continue;
        }
        QString cmd = info->extraProperty(ExtraPropertyName::kExecuteCommand).toString();
        if (cmds.contains(cmd))
            continue;
        cmds.append(cmd);

        ComputerItemData data;
        data.url = entryUrl;
        data.shape = ComputerItemData::kLargeItem;
        data.info = info;
        data.groupId = getGroupId(diskGroup());
        ret.push_back(data);
        hasNewItem = true;
    }

    return ret;
}

/*!
 * \brief ComputerItemWatcher::getGroup, create a group item
 * \param type
 * \return
 */
ComputerItemData ComputerItemWatcher::getGroup(ComputerItemWatcher::GroupType type)
{
    ComputerItemData splitter;
    splitter.shape = ComputerItemData::kSplitterItem;
    switch (type) {
    case kGroupDirs:
        splitter.itemName = userDirGroup();
        break;
    case kGroupDisks:
        splitter.itemName = diskGroup();
        break;
    }

    splitter.groupId = getGroupId(splitter.itemName);

    return splitter;
}

QString ComputerItemWatcher::userDirGroup()
{
    return tr("My Directories");
}

QString ComputerItemWatcher::diskGroup()
{
    return tr("Disks");
}

int ComputerItemWatcher::getGroupId(const QString &groupName)
{
    if (groupIds.contains(groupName))
        return groupIds.value(groupName);

    int id = ComputerUtils::getUniqueInteger();
    groupIds.insert(groupName, id);
    return id;
}

QList<QUrl> ComputerItemWatcher::disksHiddenByDConf()
{
    const auto &&currHiddenDisks = DConfigManager::instance()->value(kDefaultCfgPath, kKeyHideDisk).toStringList().toSet();
    const auto &&allSystemUUIDs = ComputerUtils::allSystemUUIDs().toSet();
    const auto &&needToBeHidden = currHiddenDisks - (currHiddenDisks - allSystemUUIDs);   // setA ∩ setB
    const auto &&devUrls = ComputerUtils::systemBlkDevUrlByUUIDs(needToBeHidden.toList());
    return devUrls;
}

void ComputerItemWatcher::cacheItem(const ComputerItemData &in)
{
    int insertAt = 0;
    bool foundGroup = false;
    for (; insertAt < initedDatas.count(); insertAt++) {
        const auto &item = initedDatas.at(insertAt);
        if (item.groupId != in.groupId) {
            if (foundGroup)
                break;
            continue;
        }
        foundGroup = true;
        if (ComputerItemWatcher::typeCompare(in, item))
            break;
    }
    initedDatas.insert(insertAt, in);
}

QString ComputerItemWatcher::reportName(const QUrl &url)
{
    //    if (url.scheme() == DFMGLOBAL_NAMESPACE::Scheme::kSmb) {
    //        return "Sharing Folders";
    //    } else if (url.scheme() == DFMROOT_SCHEME) {
    //        QString strPath = url.path();
    //        if (strPath.endsWith(SUFFIX_UDISKS)) {
    //            // 截获盘符名
    //            int startIndex = strPath.indexOf("/");
    //            int endIndex = strPath.indexOf(".");
    //            int count = endIndex - startIndex - 1;
    //            QString result = strPath.mid(startIndex + 1, count);
    //            // 组装盘符绝对路径
    //            QString localPath = "/dev/" + result;
    //            // 获得块设备路径
    //            QStringList devicePaths = DDiskManager::resolveDeviceNode(localPath, {});
    //            if (!devicePaths.isEmpty()) {
    //                QString devicePath = devicePaths.first();
    //                // 获得块设备对象
    //                DBlockDevice *blDev = DDiskManager::createBlockDevice(devicePath);
    //                // 获得块设备挂载点
    //                QByteArrayList mounts = blDev->mountPoints();
    //                if (!mounts.isEmpty()) {
    //                    QString mountPath = mounts.first();
    //                    // 如果挂载点为"/"，则为系统盘
    //                    if (mountPath == "/") {
    //                        return "System Disk";
    //                    } else {   // 数据盘
    //                        return "Data Disk";
    //                    }
    //                }
    //            }
    //        } else if (strPath.endsWith(SUFFIX_GVFSMP)) {
    //            return REPORT_SHARE_DIR;
    //        }
    //    }
    return "unknow disk";
}

void ComputerItemWatcher::addSidebarItem(DFMEntryFileInfoPointer info)
{
    if (!info)
        return;

    // additem to sidebar
    bool removable = info->extraProperty(DeviceProperty::kRemovable).toBool()
            || info->nameOf(NameInfoType::kSuffix) == SuffixInfo::kProtocol;
    if (ComputerUtils::shouldSystemPartitionHide()
        && info->nameOf(NameInfoType::kSuffix) == SuffixInfo::kBlock && !removable)
        return;

    ItemClickedActionCallback cdCb = [](quint64 winId, const QUrl &url) { ComputerControllerInstance->onOpenItem(winId, url); };
    ContextMenuCallback contextMenuCb = [](quint64 winId, const QUrl &url, const QPoint &) { ComputerControllerInstance->onMenuRequest(winId, url, true); };
    RenameCallback renameCb = [](quint64 winId, const QUrl &url, const QString &name) { ComputerControllerInstance->doRename(winId, url, name); };
    FindMeCallback findMeCb = [this](const QUrl &itemUrl, const QUrl &targetUrl) {
        if (this->routeMapper.contains(itemUrl))
            return DFMBASE_NAMESPACE::UniversalUtils::urlEquals(this->routeMapper.value(itemUrl), targetUrl);

        DFMEntryFileInfoPointer info(new EntryFileInfo(itemUrl));
        auto mntUrl = info->targetUrl();
        return dfmbase::UniversalUtils::urlEquals(mntUrl, targetUrl);
    };

    static const QStringList kItemVisiableControlKeys { "builtin_disks", "loop_dev", "other_disks", "mounted_share_dirs" };
    QString key;
    QString reportName = "Unknown Disk";
    QString subGroup = Global::Scheme::kComputer;
    if (info->extraProperty(DeviceProperty::kIsLoopDevice).toBool()) {
        key = kItemVisiableControlKeys[1];
    } else if (info->extraProperty(DeviceProperty::kHintSystem).toBool()) {
        key = kItemVisiableControlKeys[0];
        reportName = info->targetUrl().path() == "/" ? "System Disk" : "Data Disk";
    } else if (info->order() == EntryFileInfo::kOrderSmb || info->order() == EntryFileInfo::kOrderFtp) {
        key = kItemVisiableControlKeys[3];
        reportName = "Sharing Folders";
        if (info->order() == EntryFileInfo::kOrderSmb)
            subGroup = Global::Scheme::kSmb;
        else if (info->order() == EntryFileInfo::kOrderFtp)
            subGroup = Global::Scheme::kFtp;
    } else {
        key = kItemVisiableControlKeys[2];
    }

    Qt::ItemFlags flags { Qt::ItemIsEnabled | Qt::ItemIsSelectable };
    if (info->renamable())
        flags |= Qt::ItemIsEditable;
    QString iconName { info->fileIcon().name() };
    if (info->fileIcon().name().startsWith("media"))
        iconName = "media-optical-symbolic";
    else if (info->order() == EntryFileInfo::kOrderRemovableDisks)   // always display as USB icon for removable disks.
        iconName = "drive-removable-media-symbolic";
    else
        iconName += "-symbolic";

    QVariantMap map {
        { "Property_Key_Group", key == kItemVisiableControlKeys[3] ? "Group_Network" : "Group_Device" },
        { "Property_Key_SubGroup", subGroup },
        { "Property_Key_DisplayName", info->displayName() },
        { "Property_Key_Icon", QIcon::fromTheme(iconName) },
        { "Property_Key_FinalUrl", info->targetUrl().isValid() ? info->targetUrl() : QUrl() },
        { "Property_Key_QtItemFlags", QVariant::fromValue(flags) },
        { "Property_Key_Ejectable", removable },
        { "Property_Key_CallbackItemClicked", QVariant::fromValue(cdCb) },
        { "Property_Key_CallbackContextMenu", QVariant::fromValue(contextMenuCb) },
        { "Property_Key_CallbackRename", QVariant::fromValue(renameCb) },
        { "Property_Key_CallbackFindMe", QVariant::fromValue(findMeCb) },
        { "Property_Key_VisiableControl", key },
        { "Property_Key_ReportName", reportName }
    };

    dpfSlotChannel->push("dfmplugin_sidebar", "slot_Item_Add", info->urlOf(UrlInfoType::kUrl), map);
}

void ComputerItemWatcher::removeSidebarItem(const QUrl &url)
{
    dpfSlotChannel->push("dfmplugin_sidebar", "slot_Item_Remove", url);
}

void ComputerItemWatcher::insertUrlMapper(const QString &devId, const QUrl &mntUrl)
{
    QUrl devUrl;
    if (devId.startsWith(DeviceId::kBlockDeviceIdPrefix))
        devUrl = ComputerUtils::makeBlockDevUrl(devId);
    else
        devUrl = ComputerUtils::makeProtocolDevUrl(devId);
    routeMapper.insert(devUrl, mntUrl);

    if (devId.contains(QRegularExpression("sr[0-9]*$")))
        routeMapper.insert(devUrl, ComputerUtils::makeBurnUrl(devId));
}

void ComputerItemWatcher::updateSidebarItem(const QUrl &url, const QString &newName, bool editable)
{
    QVariantMap map {
        { "Property_Key_DisplayName", newName },
        { "Property_Key_Editable", editable }
    };
    dpfSlotChannel->push("dfmplugin_sidebar", "slot_Item_Update", url, map);
}

void ComputerItemWatcher::addDevice(const QString &groupName, const QUrl &url, int shape)
{
    int groupId = addGroup(groupName);
    onDeviceAdded(url, groupId, static_cast<ComputerItemData::ShapeType>(shape), false);
}

void ComputerItemWatcher::removeDevice(const QUrl &url)
{
    if (dpfHookSequence->run("dfmplugin_computer", "hook_ComputerView_ItemFilterOnRemove", url)) {
        qDebug() << "computer: [REMOVE] device is filtered by external plugin: " << url;
        return;
    }

    Q_EMIT itemRemoved(url);
    removeSidebarItem(url);
    auto ret = std::find_if(initedDatas.cbegin(), initedDatas.cend(), [url](const ComputerItemData &item) { return UniversalUtils::urlEquals(url, item.url); });
    if (ret != initedDatas.cend())
        initedDatas.removeAt(ret - initedDatas.cbegin());
}

void ComputerItemWatcher::startQueryItems()
{
    // if computer view is not init view, no receiver to receive the signal, cause when cd to computer view, shows empty.
    // on initialize computer view/model, get the cached items in construction.
    initedDatas = items();
    Q_EMIT itemQueryFinished(initedDatas);
}

/*!
 * \brief ComputerItemWatcher::addGroup, add and emit itemAdded signal
 * \param name
 * \return a unique group id
 */
int ComputerItemWatcher::addGroup(const QString &name)
{
    auto ret = std::find_if(initedDatas.cbegin(), initedDatas.cend(), [name](const ComputerItemData &item) {
        return item.shape == ComputerItemData::kSplitterItem && item.itemName == name;
    });
    ComputerItemData data;
    if (ret != initedDatas.cend()) {
        const auto &inited = initedDatas[ret - initedDatas.cbegin()];
        data.shape = inited.shape;
        data.itemName = inited.itemName;
        data.groupId = inited.groupId;
    } else {
        data.shape = ComputerItemData::kSplitterItem;
        data.itemName = name;
        data.groupId = getGroupId(name);
        cacheItem(data);
    }

    Q_EMIT itemAdded(data);
    return data.groupId;
}

void ComputerItemWatcher::onDeviceAdded(const QUrl &devUrl, int groupId, ComputerItemData::ShapeType shape, bool needSidebarItem)
{
    DFMEntryFileInfoPointer info(new EntryFileInfo(devUrl));
    if (!info->exists()) return;

    if (info->nameOf(NameInfoType::kSuffix) == SuffixInfo::kProtocol) {
        QString id = ComputerUtils::getProtocolDevIdByUrl(info->urlOf(UrlInfoType::kUrl));
        if (id.startsWith(Global::Scheme::kSmb)) {
            StashMountsUtils::stashMount(info->urlOf(UrlInfoType::kUrl), info->displayName());
            removeDevice(ComputerUtils::makeStashedProtocolDevUrl(id));
        }
    }

    if (dpfHookSequence->run("dfmplugin_computer", "hook_ComputerView_ItemFilterOnAdd", devUrl)) {
        qDebug() << "computer: [ADD] device is filtered by external plugin: " << devUrl;
        return;
    }

    ComputerItemData data;
    data.url = devUrl;
    data.shape = shape;
    data.info = info;
    data.groupId = groupId;
    data.itemName = info->displayName();
    Q_EMIT itemAdded(data);

    cacheItem(data);

    if (needSidebarItem)
        addSidebarItem(info);
}

void ComputerItemWatcher::onDevicePropertyChangedQVar(const QString &id, const QString &propertyName, const QVariant &var)
{
    onDevicePropertyChangedQDBusVar(id, propertyName, QDBusVariant(var));
}

void ComputerItemWatcher::onDevicePropertyChangedQDBusVar(const QString &id, const QString &propertyName, const QDBusVariant &var)
{
    if (id.startsWith(DeviceId::kBlockDeviceIdPrefix)) {
        auto url = ComputerUtils::makeBlockDevUrl(id);
        // if `hintIgnore` changed to TRUE, then remove the display in view, else add it.
        if (propertyName == DeviceProperty::kHintIgnore) {
            if (var.variant().toBool())
                removeDevice(url);
            else
                addDevice(diskGroup(), url, ComputerItemData::kLargeItem);
        } else {
            auto &&devUrl = ComputerUtils::makeBlockDevUrl(id);
            if (propertyName == DeviceProperty::kOptical)
                onUpdateBlockItem(id);
            Q_EMIT itemPropertyChanged(devUrl, propertyName, var.variant());
        }

        // by default if loop device do not have filesystem interface in udisks, it will not be shown in computer,
        // and for loop devices, no blockAdded signal will be emited cause it's already existed there, so
        // watch the filesystemAdded/Removed signal to decide whether to show or hide it.
        if (propertyName == DeviceProperty::kHasFileSystem) {
            auto blkInfo = DevProxyMng->queryBlockInfo(id);
            if (blkInfo.value(DeviceProperty::kIsLoopDevice).toBool()) {
                if (var.variant().toBool())
                    onDeviceAdded(url, getGroupId(diskGroup()));
                else
                    removeDevice(url);
            }
        }
    }
}

void ComputerItemWatcher::onGenAttributeChanged(Application::GenericAttribute ga, const QVariant &value)
{
    if (ga == Application::GenericAttribute::kShowFileSystemTagOnDiskIcon) {
        Q_EMIT hideFileSystemTag(!value.toBool());
    } else if (ga == Application::GenericAttribute::kHiddenSystemPartition) {
        Q_EMIT hideNativeDisks(value.toBool());
    } else if (ga == Application::GenericAttribute::kAlwaysShowOfflineRemoteConnections) {
        if (!value.toBool()) {
            QStringList mounts = StashMountsUtils::stashedMounts().keys();
            for (const auto &mountUrl : mounts) {
                QUrl stashedUrl = ComputerUtils::makeStashedProtocolDevUrl(mountUrl);
                removeDevice(stashedUrl);
                removeSidebarItem(stashedUrl);
            }
            StashMountsUtils::clearStashedMounts();
        } else {
            StashMountsUtils::stashMountedMounts();
        }
    } else if (ga == Application::GenericAttribute::kHideLoopPartitions) {
        bool hide = value.toBool();
        Q_EMIT hideLoopPartitions(hide);
    }
}

void ComputerItemWatcher::onDConfigChanged(const QString &cfg, const QString &cfgKey)
{
    if (cfgKey == kKeyHideDisk && cfg == kDefaultCfgPath)
        Q_EMIT hideDisks(disksHiddenByDConf());
}

void ComputerItemWatcher::onBlockDeviceAdded(const QString &id)
{
    QUrl url = ComputerUtils::makeBlockDevUrl(id);
    onDeviceAdded(url, getGroupId(diskGroup()));
}

void ComputerItemWatcher::onBlockDeviceRemoved(const QString &id)
{
    auto &&devUrl = ComputerUtils::makeBlockDevUrl(id);
    removeDevice(devUrl);
    routeMapper.remove(ComputerUtils::makeBlockDevUrl(id));
}

void ComputerItemWatcher::onUpdateBlockItem(const QString &id)
{
    QUrl &&devUrl = ComputerUtils::makeBlockDevUrl(id);
    Q_EMIT this->itemUpdated(devUrl);
    auto ret = std::find_if(initedDatas.cbegin(), initedDatas.cend(), [devUrl](const ComputerItemData &data) { return data.url == devUrl; });
    if (ret != initedDatas.cend()) {
        auto item = initedDatas.at(ret - initedDatas.cbegin());
        if (item.info) {
            item.info->refresh();
            updateSidebarItem(devUrl, item.info->displayName(), item.info->renamable());
        }
    }
}

void ComputerItemWatcher::onProtocolDeviceMounted(const QString &id, const QString &mntPath)
{
    Q_UNUSED(mntPath)
    auto url = ComputerUtils::makeProtocolDevUrl(id);

    if (DeviceUtils::isSamba(QUrl(id))) {
        const QVariantHash &newMount = StashMountsUtils::makeStashedSmbDataById(id);
        const QUrl &stashedUrl = StashMountsUtils::makeStashedSmbMountUrl(newMount);
        removeDevice(stashedUrl);   // Before adding mounted smb item, removing its stashed item firstly.
    }

    this->onDeviceAdded(url, getGroupId(diskGroup()));
}

void ComputerItemWatcher::onProtocolDeviceUnmounted(const QString &id)
{
    auto &&devUrl = ComputerUtils::makeProtocolDevUrl(id);
    removeDevice(devUrl);

    if (StashMountsUtils::isStashMountsEnabled()) {   // After removing smb device, adding stashed smb item to sidebar and computer view
        QUrl stashedUrl;
        if (id.startsWith(Global::Scheme::kSmb)) {
            onDeviceAdded(ComputerUtils::makeStashedProtocolDevUrl(id), getGroupId(diskGroup()));
        } else if (DeviceUtils::isSamba(QUrl(id))) {
            const QVariantHash &newMount = StashMountsUtils::makeStashedSmbDataById(id);
            StashMountsUtils::stashSmbMount(newMount);
            onDeviceAdded(StashMountsUtils::makeStashedSmbMountUrl(newMount), getGroupId(diskGroup()));
        }
    }

    routeMapper.remove(ComputerUtils::makeProtocolDevUrl(id));
}

void ComputerItemWatcher::onDeviceSizeChanged(const QString &id, qlonglong total, qlonglong free)
{
    QUrl devUrl = id.startsWith(DeviceId::kBlockDeviceIdPrefix) ? ComputerUtils::makeBlockDevUrl(id) : ComputerUtils::makeProtocolDevUrl(id);
    Q_EMIT this->itemSizeChanged(devUrl, total, free);
}

void ComputerItemWatcher::onProtocolDeviceRemoved(const QString &id)
{
    auto &&devUrl = ComputerUtils::makeProtocolDevUrl(id);
    removeDevice(devUrl);
}

void ComputerItemWatcher::onBlockDeviceMounted(const QString &id, const QString &mntPath)
{
    Q_UNUSED(mntPath);
    auto &&datas = DevProxyMng->queryBlockInfo(id);
    auto shellDevId = datas.value(GlobalServerDefines::DeviceProperty::kCryptoBackingDevice).toString();
    onUpdateBlockItem(shellDevId.length() > 1 ? shellDevId : id);
}

void ComputerItemWatcher::onBlockDeviceUnmounted(const QString &id)
{
    routeMapper.remove(ComputerUtils::makeBlockDevUrl(id));
    onUpdateBlockItem(id);
}

void ComputerItemWatcher::onBlockDeviceLocked(const QString &id)
{
    routeMapper.remove(ComputerUtils::makeBlockDevUrl(id));
    onUpdateBlockItem(id);
}

}
