/*
 * OMEMO Plugin for Psi
 * Copyright (C) 2018 Vyacheslav Karpukhin
 * Copyright (C) 2020 Boris Pek
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "configwidget.h"
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>

namespace psiomemo {
ConfigWidget::ConfigWidget(OMEMO *omemo, AccountInfoAccessingHost *accountInfo) : QWidget(), m_accountInfo(accountInfo)
{
    auto mainLayout = new QVBoxLayout(this);

    int  curIndex   = 0;
    auto accountBox = new QComboBox(this);
    while (m_accountInfo->getId(curIndex) != "-1") {
        accountBox->addItem(m_accountInfo->getName(curIndex), curIndex);
        curIndex++;
    }
    mainLayout->addWidget(accountBox);

    int account = accountBox->itemData(accountBox->currentIndex()).toInt();

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->addTab(new KnownFingerprints(account, omemo, this), tr("Fingerprints"));
    m_tabWidget->addTab(new ManageDevices(account, omemo, this), tr("Manage Devices"));
    mainLayout->addWidget(m_tabWidget);
    setLayout(mainLayout);

    // TODO: update after stopping support of Ubuntu Xenial:
    connect(accountBox, SIGNAL(currentIndexChanged(int)), SLOT(currentAccountChanged(int)));
}

ConfigWidgetTabWithTable::ConfigWidgetTabWithTable(int account, OMEMO *omemo, QWidget *parent) :
    ConfigWidgetTab(account, omemo, parent)
{
    m_table = new QTableView(this);
    m_table->setShowGrid(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);

    m_tableModel = new QStandardItemModel(this);
    m_table->setModel(m_tableModel);
}

void ConfigWidgetTabWithTable::filterContacts(const QString &jid)
{
    m_jid = jid;
    updateData();
}

void ConfigWidgetTabWithTable::updateData()
{
    int           sortSection = m_table->horizontalHeader()->sortIndicatorSection();
    Qt::SortOrder sortOrder   = m_table->horizontalHeader()->sortIndicatorOrder();
    m_tableModel->clear();

    doUpdateData();

    m_table->sortByColumn(sortSection, sortOrder);
    m_table->resizeColumnsToContents();
}

void ConfigWidget::currentAccountChanged(int index)
{
    int account = dynamic_cast<QComboBox *>(sender())->itemData(index).toInt();
    for (int i = 0; i < m_tabWidget->count(); i++) {
        dynamic_cast<ConfigWidgetTab *>(m_tabWidget->widget(i))->setAccount(account);
    }
}

KnownFingerprints::KnownFingerprints(int account, OMEMO *omemo, QWidget *parent) :
    ConfigWidgetTabWithTable(account, omemo, parent)
{
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_table);

    auto buttonsLayout = new QHBoxLayout(this);
    auto trustButton   = new QPushButton(tr("Trust"), this);
    auto revokeButton  = new QPushButton(tr("Do not trust"), this);
    auto removeButton  = new QPushButton(tr("Delete"), this);

    connect(trustButton, &QPushButton::clicked, this, &KnownFingerprints::trustFingerprint);
    connect(revokeButton, &QPushButton::clicked, this, &KnownFingerprints::revokeFingerprint);
    connect(removeButton, &QPushButton::clicked, this, &KnownFingerprints::removeFingerprint);

    buttonsLayout->addWidget(trustButton);
    buttonsLayout->addWidget(revokeButton);
    buttonsLayout->addWidget(new QLabel(this));
    buttonsLayout->addWidget(removeButton);
    mainLayout->addLayout(buttonsLayout);

    setLayout(mainLayout);
    updateData();
}

void KnownFingerprints::doUpdateData()
{
    m_tableModel->setColumnCount(3);
    m_tableModel->setHorizontalHeaderLabels({ tr("Contact"), tr("Trust"), tr("Fingerprint") });
    for (auto fingerprint : m_omemo->getKnownFingerprints(m_account)) {
        if (!m_jid.isEmpty()) {
            if (fingerprint.contact != m_jid) {
                continue;
            }
        }

        QList<QStandardItem *> row;
        auto                   contact = new QStandardItem(fingerprint.contact);
        contact->setData(QVariant(fingerprint.deviceId));
        row.append(contact);
        TRUST_STATE state = fingerprint.trust;
        row.append(
            new QStandardItem(state == TRUSTED ? tr("trusted") : state == UNTRUSTED ? tr("untrusted") : QString()));
        auto fpItem = new QStandardItem(fingerprint.fingerprint);
        fpItem->setData(QColor(state == TRUSTED ? Qt::darkGreen : state == UNTRUSTED ? Qt::darkRed : Qt::darkYellow),
                        Qt::ForegroundRole);
        fpItem->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        row.append(fpItem);
        m_tableModel->appendRow(row);
    }
}

void KnownFingerprints::removeFingerprint()
{
    if (!m_table->selectionModel()->hasSelection())
        return;

    QStandardItem *item = m_tableModel->item(m_table->selectionModel()->selectedRows(0).at(0).row(), 0);
    m_omemo->removeDevice(m_account, item->text(), item->data().toUInt());

    updateData();
}

void KnownFingerprints::trustFingerprint()
{
    if (!m_table->selectionModel()->hasSelection())
        return;

    QStandardItem *item = m_tableModel->item(m_table->selectionModel()->selectedRows(0).at(0).row(), 0);
    m_omemo->confirmDeviceTrust(m_account, item->text(), item->data().toUInt());

    const int index    = item->row();
    const int rowCount = m_tableModel->rowCount();
    updateData();

    if (rowCount == m_tableModel->rowCount())
        m_table->selectRow(index);
}

void KnownFingerprints::revokeFingerprint()
{
    if (!m_table->selectionModel()->hasSelection())
        return;

    QStandardItem *item = m_tableModel->item(m_table->selectionModel()->selectedRows(0).at(0).row(), 0);
    m_omemo->revokeDeviceTrust(m_account, item->text(), item->data().toUInt());

    const int index    = item->row();
    const int rowCount = m_tableModel->rowCount();
    updateData();

    if (rowCount == m_tableModel->rowCount())
        m_table->selectRow(index);
}

ManageDevices::ManageDevices(int account, OMEMO *omemo, QWidget *parent) :
    ConfigWidgetTabWithTable(account, omemo, parent)
{
    m_ourDeviceId = m_omemo->getDeviceId(account);

    auto currentDevice = new QGroupBox(tr("Current device"), this);
    auto currentDeviceLayout = new QHBoxLayout(currentDevice);
    auto infoLabel = new QLabel(tr("Fingerprint: "), currentDevice);
    infoLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_fingerprintLabel = new QLabel(currentDevice);
    m_fingerprintLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fingerprintLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fingerprintLabel->setWordWrap(true);
    currentDeviceLayout->addWidget(infoLabel);
    currentDeviceLayout->addWidget(m_fingerprintLabel);
    currentDevice->setLayout(currentDeviceLayout);
    currentDevice->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    auto otherDevices = new QGroupBox(tr("Other devices"), this);
    auto buttonsLayout = new QHBoxLayout();
    m_deleteButton = new QPushButton(tr("Delete"), this);
    m_deleteButton->setEnabled(false);
    connect(m_deleteButton, &QPushButton::clicked, this, &ManageDevices::deleteDevice);
    buttonsLayout->addWidget(m_deleteButton);
    buttonsLayout->addWidget(new QLabel(this));
    buttonsLayout->addWidget(new QLabel(this));

    auto otherDevicesLayout = new QVBoxLayout(otherDevices);
    otherDevicesLayout->addWidget(m_table);
    otherDevicesLayout->addLayout(buttonsLayout);
    otherDevices->setLayout(otherDevicesLayout);

    auto mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(currentDevice);
    mainLayout->addWidget(otherDevices);
    setLayout(mainLayout);

    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ManageDevices::selectionChanged);
    connect(m_omemo, &OMEMO::deviceListUpdated, this, &ManageDevices::deviceListUpdated);

    updateData();
}

void ManageDevices::updateData()
{
    m_fingerprintLabel->setText(QString("<code>%1</code>").arg(m_omemo->getOwnFingerprint(m_account)));

    ConfigWidgetTabWithTable::updateData();
}

void ManageDevices::selectionChanged(const QItemSelection &selected, const QItemSelection &)
{
    QModelIndexList selection = selected.indexes();
    if (!selection.isEmpty()) {
        m_deleteButton->setEnabled(selectedDeviceId(selection) != m_ourDeviceId);
    }
}

uint32_t ManageDevices::selectedDeviceId(const QModelIndexList &selection) const
{
    return m_tableModel->itemFromIndex(selection.first())->data().toUInt();
}

void ManageDevices::doUpdateData()
{
    m_tableModel->setColumnCount(1);
    m_tableModel->setHorizontalHeaderLabels({ tr("Device ID"), tr("Fingerprint") });
    auto fingerprintsMap = m_omemo->getOwnFingerprintsMap(m_account);
    for (auto deviceId : m_omemo->getOwnDevicesList(m_account)) {
        if (deviceId == m_ourDeviceId)
            continue;

        QList<QStandardItem *> row;
        auto item = new QStandardItem(QString::number(deviceId));
        item->setData(deviceId);
        row.append(item);
        if (fingerprintsMap.contains(deviceId)) {
            row.append(new QStandardItem(fingerprintsMap[deviceId]));
        } else {
            row.append(new QStandardItem());
        }
        m_tableModel->appendRow(row);
    }
}

void ManageDevices::deleteDevice()
{
    QModelIndexList selection = m_table->selectionModel()->selectedIndexes();
    if (!selection.isEmpty()) {
        m_omemo->unpublishDevice(m_account, selectedDeviceId(selection));
    }
}

void ManageDevices::deviceListUpdated(int account)
{
    if (account == m_account) {
        updateData();
    }
}
}
