/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

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

#include "BusListener.h"
#include "BusInterface.h"

#include <QThread>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/BusTrace.h"
#include "decoders/IsotpFlowControl.h"

BusListener::BusListener(QObject *parent, Backend &backend, BusInterface &intf)
  : QObject(parent),
    _backend(backend),
    _intf(intf),
    _shouldBeRunning(true),
    _openComplete(false)
{
    _thread = new QThread();
}

BusListener::~BusListener()
{
    delete _thread;
}

BusInterfaceId BusListener::getInterfaceId()
{
    return _intf.getId();
}

BusInterface &BusListener::getInterface()
{
    return _intf;
}

void BusListener::run()
{
    // Note: open and close done from run() so all operations take place in the same thread
    //BusMessage msg;
    QList<BusMessage> rxMessages;
    BusTrace *trace = _backend.getTrace();
    IsotpFlowControl flowControl;

    _intf.open();

    qRegisterMetaType<log_level_t >("log_level_t");
    log_info(QString(tr("Interface #%1: %2, Version: %3")).arg(QString::number(_intf.getId()), _intf.getName(), _intf.getVersion()));

    _openComplete = true;
    while (_shouldBeRunning) {
        if (_intf.readMessage(rxMessages, 100)) {
            for(const BusMessage &msg: std::as_const(rxMessages))
            {
                // CAN drivers call addFrameBits themselves; handle LIN here centrally.
                if (msg.busType() == BusType::LIN)
                    _intf.addFrameBits(msg);
                trace->enqueueMessage(msg, false);
                if (auto fc = flowControl.process(msg))
                    _intf.sendMessage(*fc);
            }
            rxMessages.clear();
        }
        else if(_intf.isOpen() == false)
        {
            log_error(QString(tr("Error on interface: %1, Closed!!!")).arg(_intf.getName()));
            rxMessages.clear();
            break;

        }
        else
        {
            rxMessages.clear();
        }

        QThread::msleep(1);
    }
    _intf.close();
    _thread->quit();
}

void BusListener::startThread()
{
    moveToThread(_thread);
    connect(_thread, &QThread::started, this, &BusListener::run);
    _thread->start();

    // Wait for interface to be open before returning so that beginMeasurement is emitted after interface open
    while(!_openComplete)
      QThread::usleep(250);
}

void BusListener::requestStop()
{
    _shouldBeRunning = false;
}

void BusListener::waitFinish()
{
    requestStop();
    _thread->wait();
}
