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

#include "Data.h"

namespace loti {

std::ostream& operator<<(ostream& out, const vector<uint8_t>& object)
{
    return out << picosha2::bytes_to_hex_string(object);
}

std::ostream& operator<<(ostream& out, const Event& event)
{
    return out << "Event { creator = " << event.getCreator() << ", hash = " << event.getHash() << ", salt = " << event.getSalt() << "}";
}

std::ostream& operator<<(ostream& out, const ClockEvent& clockEvent)
{
    return out << "ClockEvent { creator = " << clockEvent.getCreator() << ", hash = " << clockEvent.getHash() << ", salt = " << clockEvent.getSalt() << "}";
}

std::ostream& operator<<(ostream& out, const LocalClockEvent& clockEvent)
{
    return out << "LocalClockEvent { creator = " << clockEvent.getCreator() << ", hash = " << clockEvent.getHash() << ", salt = " << clockEvent.getSalt() << "}";
}

std::ostream& operator<<(ostream& out, const Neighbor& neighbor)
{
    return out << "Neighbor { nodeId = " << neighbor.getNodeId() << ", address = " << neighbor.getAddress() << "}";
}

std::ostream& operator<<(ostream& out, const EventChain& eventChain)
{
    return out << "EventChain { event = " << eventChain.getEvent() << ", lowerBound = " << eventChain.getLowerBound().size() << " events, upperBound = " << eventChain.getUpperBound().size() << "events }";
}

std::ostream& operator<<(ostream& out, const EventChainDiscovery& eventChainDiscovery)
{
    return out << "EventChainDiscovery { originator = " << eventChainDiscovery.getOriginator() << "}";
}

std::ostream& operator<<(ostream& out, const EventBoundsDiscovery& eventBoundsDiscovery)
{
    return out << "EventBoundsDiscovery { event = " << eventBoundsDiscovery.getEvent() << "}";
}

std::ostream& operator<<(ostream& out, const EventOrderDiscovery& eventOrderDiscovery)
{
    return out << "EventOrderDiscovery { event1 = " << eventOrderDiscovery.getEvent1() << ", event2 = " << eventOrderDiscovery.getEvent2() << "}";
}

B calculateLotiHeaderSize(const Ptr<const LotiHeader>& header)
{
    return B(2) + // LotiType type
           B(8);  // NodeId neighbor
}

B calculateClockEventNotificationSize(const Ptr<const ClockEventNotification>& notification)
{
    return B(32) + // EventHash lastClockEventHash
           B(32);  // EventHash neighborLastClockEventHash
}

B calculateEventChainDiscoveryRequestSize(const Ptr<const EventChainDiscoveryRequest>& request)
{
    return B(8) + // NodeId originator
           B(8) + B(32); // EventReference event
}

B calculateEventChainDiscoveryResponseSize(const Ptr<const EventChainDiscoveryResponse>& response)
{
    return B(8) + // NodeId originator
           calculateEventChainSize(response->getChain()); // EventChain chain
}

B calculateEventChainSize(const EventChain& eventChain)
{
    B size = B(0);
    for (auto& clockEvent : eventChain.getLowerBound())
        size += calculateClockEventSize(clockEvent);
    size += calculateEventSize(eventChain.getEvent());
    for (auto& clockEvent : eventChain.getUpperBound())
        size += calculateClockEventSize(clockEvent);
    return size;
}

B calculateClockEventSize(const ClockEvent& clockEvent)
{
    return B(8) + // NodeId creator
           B(32) + // EventHash hash
           B(8) + // simtime_t timestamp
           B(8) + // Salt salt
           (B(8) + B(32)) * clockEvent.getReferencedEvents().size(); // EventReferenceVector referencedEvents
}

B calculateEventSize(const Event& event)
{
    return B(8) + // NodeId creator
           B(32) + // EventHash hash
           // ByteVector data: not included
           B(8) + // Salt salt
           (B(8) + B(32)) * event.getReferencedEvents().size(); // EventReferenceVector referencedEvents
}

}
