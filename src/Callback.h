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

#ifndef _LOTI_CALLBACK_H_
#define _LOTI_CALLBACK_H_

#include <omnetpp.h>

namespace loti {

using namespace omnetpp;

class IEventChainDiscoveryCallback
{
  public:
    virtual void processEventChainDiscoveryFailed(const Event& event) = 0;
    virtual void processEventChainDiscoveryCompleted(const EventChain& eventChain) = 0;
};

class IEventBoundsDiscoveryCallback
{
  public:
    virtual void processEventBoundsDiscoveryFailed(const Event& event) = 0;
    virtual void processEventBoundsDiscoveryCompleted(const Event& event, simtime_t lowerBound, simtime_t upperBound) = 0;
};

class IEventOrderDiscoveryCallback
{
  public:
    virtual void processEventOrderDiscoveryFailed(const Event& event1, const Event& event2) = 0;
    virtual void processEventOrderDiscoveryCompleted(const Event& event1, const Event& event2, int order) = 0;
};

}

#endif // ifndef _LOTI_CALLBACK_H_
