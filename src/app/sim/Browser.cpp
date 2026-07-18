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
    if (event)
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
    if (event)
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
    if (event1 && event2)
        daemon->discoverEventOrder(*event1, *event2, *this);
    else
        EV_WARN << "No events found for event order discovery" << endl;
}

std::optional<domain::Event> Browser::findRandomEvent()
{
    if (configurator->getNumNetworkNodes() > 0) {
        int networkNodeIndex = intrand(configurator->getNumNetworkNodes());
        auto networkNode = configurator->getNetworkNode(networkNodeIndex);
        auto peer = configurator->findDaemon(networkNode);
        if (peer->getNumEvents() > 0) {
            int eventIndex = intrand(peer->getNumEvents());
            return peer->getEvent(eventIndex);  // by value — the DAG lives in the store
        }
    }
    return std::nullopt;
}

void Browser::on_chain_completed(const domain::Event& event, const domain::EventChain&)
{
    EV_INFO << "Event chain discovery completed: creator = " << event.creator << endl;
}

void Browser::on_chain_aborted(const domain::Event& event)
{
    EV_INFO << "Event chain discovery aborted: creator = " << event.creator << endl;
}

void Browser::on_bounds_completed(const domain::Event& event, domain::Timestamp lower,
                                  domain::Timestamp upper)
{
    EV_INFO << "Event bounds discovery completed: creator = " << event.creator
            << ", interval = " << SimTime::fromRaw(upper - lower) << endl;
}

void Browser::on_bounds_aborted(const domain::Event& event)
{
    EV_INFO << "Event bounds discovery aborted: creator = " << event.creator << endl;
}

void Browser::on_order_completed(const domain::Event& event1, const domain::Event& event2,
                                 domain::Order order)
{
    EV_INFO << "Event order discovery completed: creator1 = " << event1.creator
            << ", creator2 = " << event2.creator << ", order = " << static_cast<int>(order) << endl;
}

void Browser::on_order_aborted(const domain::Event& event1, const domain::Event& event2)
{
    EV_INFO << "Event order discovery aborted: creator1 = " << event1.creator
            << ", creator2 = " << event2.creator << endl;
}

}
