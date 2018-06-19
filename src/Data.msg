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

import inet.networklayer.contract.ipv4.Ipv4Address;
import Type;

namespace loti;

class EventReference extends cObject
{
    NodeId creator;
    EventHash hash;
}

class ClockEvent extends cObject
{
    NodeId creator;
    EventHash hash;
    simtime_t timestamp;
    Salt salt;
    EventReferenceVector referencedEvents;
}

class LocalClockEvent extends ClockEvent
{
    EventReferenceVector referencingEvents;
}

class Event extends cObject
{
    NodeId creator;
    EventHash hash;
    ByteVector data;
    Salt salt;
    EventReferenceVector referencedEvents;
}

class EventChain extends cObject
{
    Event event;
    ClockEventDeque lowerBound;
    ClockEventDeque upperBound;
}

enum DiscoveryState
{
    DS_DISCOVERY_INPROGRESS = 0;
    DS_DISCOVERY_COMPLETED = 1;
    DS_DISCOVERY_ABORTED = -1;
}

class EventChainDiscovery extends cObject
{
    simtime_t startTime;
    simtime_t endTime;
    NodeId originator;
    Event event;
    EventChain chain;
    DiscoveryState state;
}

class EventChainDiscoveryParticipation extends cObject
{
    simtime_t startTime;
    NodeId neighbor;
}

class EventBoundsDiscovery extends cObject
{
    simtime_t startTime;
    simtime_t endTime;
    Event event;
    simtime_t lowerBound;
    simtime_t upperBound;
    DiscoveryState state;
}

class EventOrderDiscovery extends cObject
{
    simtime_t startTime;
    simtime_t endTime;
    Event event1;
    Event event2;
    int order;
    DiscoveryState state;
}

class Neighbor extends cObject
{
    NodeId nodeId;
    inet::Ipv4Address address;
    EventHash lastClockEventHash;
}
