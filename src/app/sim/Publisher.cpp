//
// This file is part of the LOTI distribution (https://github.com/levy/loti/).
// Copyright (c) 2018 Levente Mészáros.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//

#include "inet/common/ModuleAccess.h"
#include "Publisher.h"

namespace loti {

Define_Module(Publisher);

Publisher::~Publisher()
{
    cancelEvent(&createEventTimer);
}

void Publisher::initialize(int stage)
{
    if (stage == INITSTAGE_LOCAL) {
        daemon = getModuleFromPar<Daemon>(par("daemonModule"), this);
        createEventTimer.setName("CreateEventTimer");
        scheduleCreateEventTimer();
    }
}

void Publisher::handleMessage(cMessage *message)
{
    if (message == &createEventTimer) {
        processCreateEventTimer();
        scheduleCreateEventTimer();
    }
    else
        throw cRuntimeError("Unknown message");
}

void Publisher::scheduleCreateEventTimer()
{
    scheduleAt(simTime() + par("createEventInterval"), &createEventTimer);
}

void Publisher::processCreateEventTimer()
{
    createEvent();
}

domain::Event Publisher::createEvent()
{
    domain::Bytes data;
    int contentLength = par("contentLength");
    for (int i = 0; i < contentLength; i++)
        data.push_back(static_cast<uint8_t>(intrand(256)));
    return daemon->publishEvent(data);
}

}
