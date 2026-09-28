/*

  Copyright (c) 2026 Jayachandran Dharuman
  Copyright (c) 2026 Schildkroet

  This file is part of CANgaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/
#pragma once

#include "core/Backend.h"
#include "core/ConfigurableWidget.h"
#include "core/MeasurementSetup.h"
#include <chrono>

#include <QList>
#include <QTreeWidgetItem>


namespace Ui {
class TxGeneratorWindow;
}

#include "BitMatrixWidget.h"
#include "db/model/CanDbMessage.h"
#include "db/model/CanDbSignal.h"

class QDomDocument;
class QDomElement;

class TxGeneratorWindow : public ConfigurableWidget
{
    Q_OBJECT
public:
    explicit TxGeneratorWindow(QWidget *parent, Backend &backend);
    ~TxGeneratorWindow();

    virtual bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root);
    virtual bool loadXML(Backend &backend, QDomElement &el);
    virtual QSize sizeHint() const override;

protected:
    void retranslateUi() override;

signals:
    void loopbackFrame(const BusMessage &msg);
    void interfaceChanged(BusInterfaceId interfaceId);

public slots:
    void stopAll();

private slots:
    void on_lineEditSearchAvailable_textChanged(const QString &text);
    void on_sliderLayoutZoom_valueChanged(int value);
    void on_cbLayoutCompact_toggled(bool checked);
    void on_btnAddToList_released();
    void on_btnAddManual_released();
    void on_btnRemove_released();
    void on_btnSendOnce_released();
    void on_btnBulkRun_clicked();
    void on_btnBulkStop_clicked();
    void on_spinInterval_valueChanged(int i);
    void on_comboBoxInterface_currentIndexChanged(int index);
    void on_treeAvailable_itemDoubleClicked(QTreeWidgetItem *item, int column);
    void on_treeAvailable_itemSelectionChanged();
    void on_treeActive_itemSelectionChanged();
    void treeActiveItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onSendTimerTimeout();
    void onSetupChanged();
    void on_btnSelectAll_released();
    void on_btnClearAll_released();
    void on_treeActive_itemChanged(QTreeWidgetItem *item, int column);
    void onStatusButtonClicked();
    void updateMeasurementState();
    void refreshInterfaces();
    void onRandomPayloadReleased();

private:
    Ui::TxGeneratorWindow *ui;
    Backend &_backend;
    QTimer *_sendTimer = nullptr;
    BitMatrixWidget *_bitMatrixWidget = nullptr;
    class QPushButton *_btnRandomPayload;

    /// Monotonic clock for the TX schedule; unaffected by wall-clock (NTP) steps.
    using TxClock = std::chrono::steady_clock;

    enum ActiveColumn { SendColumn, CyclicColumn, IdColumn, NameColumn,
                        DataColumn, InterfaceColumn, LengthColumn, IntervalColumn };

    struct CyclicMessage {
        BusMessage msg;
        QString name;
        int interval;
        bool enabled;
        /// Absolute deadline of the next send. A default-constructed value means
        /// "not scheduled yet" and makes the message due immediately.
        TxClock::time_point nextDue;
        BusInterfaceId interfaceId;
        CanDbMessage *dbMsg;
        bool usesDbMessage = false;
        QString interfaceName; ///< Display name saved to XML; resolved to interfaceId by resolveInterfaceNames()
    };

    QList<CyclicMessage> _cyclicMessages;

    void updateAvailableList();
    void updateActiveList();
    void updateRowUI(int row);
    void populateDbcMessages();
    // Matches each CyclicMessage::interfaceName against the combo box and
    // sets interfaceId accordingly. Called after the combo box is repopulated.
    void resolveInterfaceNames();
    // Re-links DBC requests after setup changes. Manual requests keep their own
    // frame settings even when a DBC contains the same CAN ID.
    void resolveDbMessages();
    /// Arms the single-shot send timer for the earliest pending deadline, or
    /// stops it when nothing is due.
    void updateSendTimer();
    /// Applies a new cycle time to a row and pulls its pending deadline in if
    /// the old (longer) one is still armed.
    void setInterval(int row, int interval_ms);
    /// Enables/disables a row, restarting its schedule on the off->on edge.
    void setEnabled(int row, bool enabled);
    /// Sends exactly one frame without changing the cyclic schedule.
    void sendOnce(int row);
    void updateSendControls();
};

