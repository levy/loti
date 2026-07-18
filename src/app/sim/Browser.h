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

#ifndef _LOTI_APP_SIM_BROWSER_H_
#define _LOTI_APP_SIM_BROWSER_H_

#include <optional>

#include <omnetpp.h>

#include "Daemon.h"
#include "NetworkConfigurator.h"

namespace loti {

// Drives random discoveries against the network's events. It is the core's
// discovery callback (ChainCallback/BoundsCallback/OrderCallback); the recorded
// statistics come from the Daemon's telemetry, so the callbacks here only log.
class Browser : public cSimpleModule, public ChainCallback, public BoundsCallback, public OrderCallback
{
  private:
    Daemon *daemon = nullptr;
    NetworkConfigurator *configurator = nullptr;

    cMessage discoverEventChainTimer;
    cMessage discoverEventBoundsTimer;
    cMessage discoverEventOrderTimer;

  protected:
    int numInitStages() const override { return NUM_INIT_STAGES; }
    void initialize(int stage) override;
    void handleMessage(cMessage *message) override;

  private:
    void scheduleDiscoverEventChainTimer();
    void processDiscoverEventChainTimer();
    void scheduleDiscoverEventBoundsTimer();
    void processDiscoverEventBoundsTimer();
    void scheduleDiscoverEventOrderTimer();
    void processDiscoverEventOrderTimer();
    std::optional<domain::Event> findRandomEvent();

  public:
    ~Browser() override;

    void on_chain_completed(const domain::Event& event, const domain::EventChain& chain) override;
    void on_chain_aborted(const domain::Event& event) override;
    void on_bounds_completed(const domain::Event& event, domain::Timestamp lower,
                             domain::Timestamp upper) override;
    void on_bounds_aborted(const domain::Event& event) override;
    void on_order_completed(const domain::Event& event1, const domain::Event& event2,
                            domain::Order order) override;
    void on_order_aborted(const domain::Event& event1, const domain::Event& event2) override;
};

}

#endif // ifndef _LOTI_APP_SIM_BROWSER_H_
