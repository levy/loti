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

#ifndef _LOTI_BROWSER_H_
#define _LOTI_BROWSER_H_

#include <omnetpp.h>
#include "Daemon.h"
#include "NetworkConfigurator.h"

namespace loti {

class Browser : public cSimpleModule, public IEventChainDiscoveryCallback, public IEventBoundsDiscoveryCallback, public IEventOrderDiscoveryCallback
{
  private:
    Daemon *daemon = nullptr;
    NetworkConfigurator *configurator = nullptr;

    cMessage discoverEventChainTimer;
    cMessage discoverEventBoundsTimer;
    cMessage discoverEventOrderTimer;

  protected:
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(cMessage *message) override;

  private:
    void scheduleDiscoverEventChainTimer();
    void processDiscoverEventChainTimer();

    void scheduleDiscoverEventBoundsTimer();
    void processDiscoverEventBoundsTimer();

    void scheduleDiscoverEventOrderTimer();
    void processDiscoverEventOrderTimer();

    const Event *findRandomEvent();

  public:
    virtual ~Browser();

    virtual void processEventChainDiscoveryAborted(const Event& event) override;
    virtual void processEventChainDiscoveryCompleted(const Event& event, const EventChain& eventChain) override;

    virtual void processEventBoundsDiscoveryAborted(const Event& event) override;
    virtual void processEventBoundsDiscoveryCompleted(const Event& event, simtime_t lowerBound, simtime_t upperBound) override;

    virtual void processEventOrderDiscoveryAborted(const Event& event1, const Event& event2) override;
    virtual void processEventOrderDiscoveryCompleted(const Event& event1, const Event& event2, int order) override;
};

}

#endif // ifndef _LOTI_BROWSER_H_
