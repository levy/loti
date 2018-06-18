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

class Event extends cObject
{
    NodeId creator;
    EventHash hash;
    ByteVector data;
    Salt salt;
    EventReferenceVector referencedEvents;
}