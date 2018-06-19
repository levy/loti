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
        daemon = getModuleFromPar<Daemon>(par("daemonModule"), this);
        configurator = getModuleFromPar<NetworkConfigurator>(par("configuratorModule"), this);
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
    if (message == &discoverEventChainTimer) {
        processDiscoverEventChainTimer();
        scheduleDiscoverEventChainTimer();
    }
    else if (message == &discoverEventBoundsTimer) {
        processDiscoverEventBoundsTimer();
        scheduleDiscoverEventBoundsTimer();
    }
    else if (message == &discoverEventOrderTimer) {
        processDiscoverEventOrderTimer();
        scheduleDiscoverEventOrderTimer();
    }
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
    if (event != nullptr)
        daemon->discoverEventChain(*event, *this);
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
    if (event != nullptr)
        daemon->discoverEventBounds(*event, *this);
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
    auto event1 = findRandomEvent();
    auto event2 = findRandomEvent();
    if (event1 != nullptr && event2 != nullptr)
        daemon->discoverEventOrder(*event1, *event2, *this);
    else
        EV_WARN << "No events found for event order discovery" << endl;
}

void Browser::processEventChainDiscoveryAborted(const Event& event)
{
    EV_INFO << "Event chain discovery aborted: event = " << event << endl;
}

void Browser::processEventChainDiscoveryCompleted(const Event& event, const EventChain& eventChain)
{
    EV_INFO << "Event chain discovery completed: event = " << event << ", eventChain = " << eventChain << endl;
}

void Browser::processEventBoundsDiscoveryAborted(const Event& event)
{
    EV_INFO << "Event bounds discovery aborted: event = " << event << endl;
}

void Browser::processEventBoundsDiscoveryCompleted(const Event& event, simtime_t lowerBound, simtime_t upperBound)
{
    EV_INFO << "Event bounds discovery completed: event = " << event << ", lowerBound = " << lowerBound << ", upperBound = " << upperBound << ", eventTimeInterval = " << (upperBound - lowerBound) << endl;
}

void Browser::processEventOrderDiscoveryAborted(const Event& event1, const Event& event2)
{
    EV_INFO << "Event order discovery aborted: event = " << event1 << ", event = " << event2 << endl;
}

void Browser::processEventOrderDiscoveryCompleted(const Event& event1, const Event& event2, int order)
{
    EV_INFO << "Event order discovery completed: event = " << event1 << ", event = " << event2 << ", order = " << order << endl;
}

const Event *Browser::findRandomEvent()
{
    if (configurator->getNumNetworkNodes() > 0) {
        int networkNodeIndex = intrand(configurator->getNumNetworkNodes());
        auto networkNode = configurator->getNetworkNode(networkNodeIndex);
        auto daemon = configurator->findDaemon(networkNode);
        if (daemon->getNumEvents() > 0) {
            int eventIndex = intrand(daemon->getNumEvents());
            return &daemon->getEvent(eventIndex);
        }
    }
    return nullptr;
}

}
