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

#ifndef _LOTI_DATA_H_
#define _LOTI_DATA_H_

#include <omnetpp.h>
#include "picosha.h"
#include "Data_m.h"
#include "Packet_m.h"

namespace loti {

using namespace omnetpp;
using namespace inet;

std::ostream& operator<<(ostream& out, const vector<uint8_t>& object);
std::ostream& operator<<(ostream& out, const Event& event);
std::ostream& operator<<(ostream& out, const ClockEvent& clockEvent);
std::ostream& operator<<(ostream& out, const LocalClockEvent& clockEvent);
std::ostream& operator<<(ostream& out, const Neighbor& neighbor);
std::ostream& operator<<(ostream& out, const EventChain& eventChain);
std::ostream& operator<<(ostream& out, const EventChainDiscovery& eventChainDiscovery);
std::ostream& operator<<(ostream& out, const EventBoundsDiscovery& eventBoundsDiscovery);
std::ostream& operator<<(ostream& out, const EventOrderDiscovery& eventOrderDiscovery);

B calculateLotiHeaderSize(const Ptr<const LotiHeader>& header);
B calculateClockEventNotificationSize(const Ptr<const ClockEventNotification>& notification);
B calculateEventChainDiscoveryRequestSize(const Ptr<const EventChainDiscoveryRequest>& request);
B calculateEventChainDiscoveryResponseSize(const Ptr<const EventChainDiscoveryResponse>& response);
B calculateEventChainSize(const EventChain& eventChain);
B calculateClockEventSize(const ClockEvent& clockEvent);
B calculateEventSize(const Event& event);

}

#endif // ifndef _LOTI_DATA_H_
