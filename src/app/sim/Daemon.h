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

#ifndef _LOTI_APP_SIM_DAEMON_H_
#define _LOTI_APP_SIM_DAEMON_H_

#include <memory>

#include <omnetpp.h>

#include "inet/networklayer/common/L3Address.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"

#include "adapters/sim/clock.hpp"
#include "adapters/sim/in_memory_store.hpp"
#include "adapters/sim/rng.hpp"
#include "adapters/sim/scheduler.hpp"
#include "adapters/sim/signer.hpp"
#include "adapters/sim/telemetry.hpp"
#include "adapters/sim/transport.hpp"
#include "node.hpp"

namespace loti {

using namespace omnetpp;
using namespace inet;

// The OMNeT++ host for one protocol node: a thin cSimpleModule that owns a
// loti::Node (the shared protocol engine in core/) and the simulation port
// adapters, wiring the module's time/scheduling/socket/RNG/signals into it. All
// protocol logic lives in the core; this class only adapts the runtime.
class Daemon : public cSimpleModule, public UdpSocket::ICallback
{
  private:
    static constexpr int kPort = 666;

    sim::SimClock clock_;
    sim::SimScheduler scheduler_;
    UdpSocket socket_;
    sim::SimTransport transport_;
    sim::SimRng rng_;
    sim::NullSigner signer_;
    sim::SimTelemetry telemetry_;
    sim::InMemoryStore store_;  // the DAG store — in RAM, dies with the run (no persistence)

    domain::NodeId nodeId_ = 0;
    cMessage createClockEventTimer_;

    // Multi-resolution clock chains driven off the single fast timer: chain 0 ticks every
    // timer firing; chain L also ticks every clockChainFactor^L firings (see handleMessage).
    int numChains_ = 1;
    int chainFactor_ = 1;
    long clockTickCount_ = 0;
    simsignal_t clockEventsRetainedSignal_ = -1;

    // Constructed at INITSTAGE_LOCAL, once getId() is available. Declared last so
    // it is destroyed first — while the port adapters it references are still alive.
    std::unique_ptr<Node> node_;

  public:
    Daemon();
    ~Daemon() override;

    // Application API used by the Publisher/Browser modules. Events come back by value —
    // the DAG lives in the store now, so there is no stable in-RAM Event to reference.
    domain::Event publishEvent(const domain::Bytes& data);
    void discoverEventChain(const domain::Event& event, ChainCallback& callback);
    void discoverEventBounds(const domain::Event& event, BoundsCallback& callback);
    void discoverEventOrder(const domain::Event& event1, const domain::Event& event2,
                            OrderCallback& callback);

    int getNumEvents() const { return static_cast<int>(node_->event_count()); }
    domain::Event getEvent(int index) const { return node_->event_at(index); }

    // Overlay wiring, filled in by the NetworkConfigurator.
    domain::NodeId nodeId() const { return nodeId_; }
    void learnRoute(domain::NodeId destination, domain::NodeId nextHop);
    void addNeighbor(domain::NodeId id, const L3Address& address);

  protected:
    int numInitStages() const override { return NUM_INIT_STAGES; }
    void initialize(int stage) override;
    void handleMessage(cMessage *message) override;

    void socketDataArrived(UdpSocket *socket, Packet *packet) override;
    void socketErrorArrived(UdpSocket *socket, Indication *indication) override { delete indication; }
    void socketClosed(UdpSocket *socket) override {}

  private:
    void scheduleCreateClockEventTimer();
};

}

#endif // ifndef _LOTI_APP_SIM_DAEMON_H_
