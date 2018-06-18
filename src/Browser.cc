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
#include "Browser.h"

namespace loti {

Define_Module(Browser);

Browser::~Browser()
{
    cancelEvent(&discoverEventChainTimer);
    cancelEvent(&discoverEventBoundsTimer);
    cancelEvent(&discoverEventOrderTimer);
}

void Browser::initialize(int stage)
{
    if (stage == INITSTAGE_LOCAL) {
        discoverEventChainTimer.setName("DiscoverEventChainTimer");
        discoverEventBoundsTimer.setName("DiscoverEventBoundsTimer");
        discoverEventOrderTimer.setName("DiscoverEventOrderTimer");
        scheduleDiscoverEventChainTimer();
        scheduleDiscoverEventBoundsTimer();
        scheduleDiscoverEventOrderTimer();
    }
}

void Browser::handleMessage(cMessage *message)
{
    if (message == &discoverEventChainTimer)
        processDiscoverEventChainTimer();
    else if (message == &discoverEventBoundsTimer)
        processDiscoverEventBoundsTimer();
    else if (message == &discoverEventOrderTimer)
        processDiscoverEventOrderTimer();
    else
        throw cRuntimeError("Unknown message");
}

void Browser::scheduleDiscoverEventChainTimer()
{
    simtime_t interval = par("discoverEventChainInterval");
    if (interval != 0)
        scheduleAt(simTime() + interval, &discoverEventChainTimer);
}

void Browser::processDiscoverEventChainTimer()
{
    auto event = findRandomEvent();
    if (event != nullptr) {
        scheduleDiscoverEventChainTimer();
    }
    else
        EV_WARN << "No event found for event chain discovery" << endl;
}

void Browser::scheduleDiscoverEventBoundsTimer()
{
    simtime_t interval = par("discoverEventBoundsInterval");
    if (interval != 0)
        scheduleAt(simTime() + interval, &discoverEventBoundsTimer);
}

void Browser::processDiscoverEventBoundsTimer()
{
    auto event = findRandomEvent();
    if (event != nullptr) {
        scheduleDiscoverEventBoundsTimer();
    }
    else
        EV_WARN << "No event found for event bounds discovery" << endl;
}

void Browser::scheduleDiscoverEventOrderTimer()
{
    simtime_t interval = par("discoverEventOrderInterval");
    if (interval != 0)
        scheduleAt(simTime() + interval, &discoverEventOrderTimer);
}

void Browser::processDiscoverEventOrderTimer()
{
    // TODO:
}

}
