/*
 * Copyright (C) 2021 ~ 2022 Uniontech Software Technology Co., Ltd.
 *
 * Author:     zhangsheng<zhangsheng@uniontech.com>
 *
 * Maintainer: max-lv<lvwujun@uniontech.com>
 *             lanxuesong<lanxuesong@uniontech.com>
 *             xushitong<xushitong@uniontech.com>
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
#include "workspacewidget.h"
#include "fileview.h"
#include "tabbar.h"
#include "tab.h"
#include "events/workspaceeventcaller.h"
#include "utils/workspacehelper.h"

#include "services/filemanager/windows/windowsservice.h"

#include "dfm-base/interfaces/abstractbaseview.h"
#include "dfm-base/base/schemefactory.h"

#include <DIconButton>
#include <DHorizontalLine>

#include <QVBoxLayout>
#include <QStackedLayout>
#include <QKeyEvent>
#include <QPushButton>

DSB_FM_USE_NAMESPACE
DFMBASE_USE_NAMESPACE
DPWORKSPACE_USE_NAMESPACE

WorkspaceWidget::WorkspaceWidget(QFrame *parent)
    : AbstractFrame(parent)
{
    initializeUi();
    initConnect();
}

// NOTE(zhangs): please ref to : DFileManagerWindowPrivate::cdForTab (old filemanager)
void WorkspaceWidget::setCurrentUrl(const QUrl &url)
{
    workspaceUrl = url;
    // NOTE(zhangs): follw is temp code, need tab!

    if (!tabBar->currentTab())
        tabBar->createTab(nullptr);

    QString scheme { url.scheme() };
    if (!views.contains(scheme)) {
        QString error;
        ViewPtr fileView = ViewFactory::create<AbstractBaseView>(url, &error);
        if (!fileView) {
            qWarning() << "Cannot create view for " << url << "Reason: " << error;
            return;
        }
        viewStackLayout->addWidget(dynamic_cast<QWidget *>(fileView.get()));
        viewStackLayout->setCurrentWidget(fileView->widget());
        views.insert(url.scheme(), fileView);
        tabBar->setCurrentView(fileView.get());
        tabBar->setCurrentUrl(url);
        return;
    }
    views[scheme]->setRootUrl(url);
    viewStackLayout->setCurrentWidget(views[scheme]->widget());
    tabBar->setCurrentView(views[scheme].get());
    tabBar->setCurrentUrl(url);
}

QUrl WorkspaceWidget::currentUrl() const
{
    return workspaceUrl;
}

void WorkspaceWidget::openNewTab(const QUrl &url)
{
    if (!tabBar->tabAddable())
        return;

    if (url.isEmpty()) {
        // turn to home path
    }

    tabBar->createTab(nullptr);

    auto windowID = WorkspaceHelper::instance()->windowId(this);
    WorkspaceEventCaller::sendChangeCurrentUrl(windowID, url);
}

void WorkspaceWidget::onOpenUrlInNewTab(quint64 windowId, const QUrl &url)
{
    quint64 thisWindowID = WorkspaceHelper::instance()->windowId(this);
    if (thisWindowID == windowId)
        openNewTab(url);
}

void WorkspaceWidget::onCurrentTabChanged(int tabIndex)
{
    Tab *tab = tabBar->tabAt(tabIndex);
    if (tab) {
        auto windowID = WorkspaceHelper::instance()->windowId(this);
        WorkspaceEventCaller::sendChangeCurrentUrl(windowID, tab->getCurrentUrl());
    }
}

void WorkspaceWidget::onRequestCloseTab(const int index, const bool &remainState)
{
    tabBar->removeTab(index, remainState);
}

void WorkspaceWidget::onTabAddableChanged(bool addable)
{
    newTabButton->setEnabled(addable);
}

void WorkspaceWidget::showNewTabButton()
{
    newTabButton->show();
    tabTopLine->show();
    tabBottomLine->show();
}

void WorkspaceWidget::hideNewTabButton()
{
    newTabButton->hide();
    tabTopLine->hide();
    tabBottomLine->hide();
}

void WorkspaceWidget::onNewTabButtonClicked()
{
    QUrl url = currentUrl();
    openNewTab(url);
}

void WorkspaceWidget::keyPressEvent(QKeyEvent *event)
{
    switch (event->modifiers()) {
    case Qt::ControlModifier:
        switch (event->key()) {
        case Qt::Key_N:
            handleCtrlN();
            return;
        case Qt::Key_T:
            openNewTab(tabBar->currentTab()->getCurrentUrl());
            return;
        default:
            break;
        }
        break;
    }
    AbstractFrame::keyPressEvent(event);
}

void WorkspaceWidget::showEvent(QShowEvent *event)
{
    AbstractFrame::showEvent(event);

    setFocus();
}

// NOTE(zhangs): please ref to: DFileManagerWindow::initRightView (old filemanager)
void WorkspaceWidget::initializeUi()
{
    initTabBar();
    initViewLayout();
    // TODO(zhangs): initRenameBarState
}

void WorkspaceWidget::initConnect()
{
    connect(WorkspaceHelper::instance(), &WorkspaceHelper::openUrlInNewTab, this, &WorkspaceWidget::onOpenUrlInNewTab);

    QObject::connect(tabBar, &TabBar::currentChanged, this, &WorkspaceWidget::onCurrentTabChanged);
    QObject::connect(tabBar, &TabBar::tabCloseRequested, this, &WorkspaceWidget::onRequestCloseTab);
    QObject::connect(tabBar, &TabBar::tabAddableChanged, this, &WorkspaceWidget::onTabAddableChanged);
    QObject::connect(tabBar, &TabBar::tabBarShown, this, &WorkspaceWidget::showNewTabButton);
    QObject::connect(tabBar, &TabBar::tabBarHidden, this, &WorkspaceWidget::hideNewTabButton);
    QObject::connect(newTabButton, &QPushButton::clicked, this, &WorkspaceWidget::onNewTabButtonClicked);
}

void WorkspaceWidget::initTabBar()
{
    tabBar = new TabBar(this);
    tabBar->setFixedHeight(36);

    newTabButton = new DIconButton(this);
    newTabButton->setObjectName("NewTabButton");
    newTabButton->setFixedSize(36, 36);
    newTabButton->setIconSize({ 24, 24 });
    newTabButton->setIcon(QIcon::fromTheme("dfm_tab_new"));
    newTabButton->setFlat(true);
    newTabButton->hide();

    tabTopLine = new DHorizontalLine(this);
    tabBottomLine = new DHorizontalLine(this);
    tabTopLine->setFixedHeight(1);
    tabBottomLine->setFixedHeight(1);
    tabTopLine->hide();
    tabBottomLine->hide();

    tabBarLayout = new QHBoxLayout;
    tabBarLayout->setMargin(0);
    tabBarLayout->setSpacing(0);
    tabBarLayout->addWidget(tabBar);
    tabBarLayout->addWidget(newTabButton);
}

void WorkspaceWidget::initViewLayout()
{
    viewStackLayout = new QStackedLayout;
    viewStackLayout->setSpacing(0);
    viewStackLayout->setContentsMargins(0, 0, 0, 0);

    widgetLayout = new QVBoxLayout;
    widgetLayout->addWidget(tabTopLine);
    widgetLayout->addLayout(tabBarLayout);
    widgetLayout->addWidget(tabBottomLine);
    widgetLayout->addLayout(viewStackLayout, 1);
    widgetLayout->setSpacing(0);
    widgetLayout->setContentsMargins(0, 0, 0, 0);
    setLayout(widgetLayout);
}

void WorkspaceWidget::handleCtrlN()
{
    ViewPtr fileView = views[workspaceUrl.scheme()];
    if (!fileView) {
        qWarning() << "Cannot find view by url: " << workspaceUrl;
        return;
    }
    WorkspaceEventCaller::sendOpenWindow(fileView->selectedUrlList());
}
