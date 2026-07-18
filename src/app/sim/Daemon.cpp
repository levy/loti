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

#include "Daemon.h"

namespace loti {

Define_Module(Daemon);

Daemon::Daemon()
    : scheduler_([this](cMessage *m, simtime_t t) { scheduleAt(t, m); },
                 [this](cMessage *m) { cancelEvent(m); }),
      transport_(socket_, kPort),
      rng_(this),
      telemetry_(this)
{
}

Daemon::~Daemon()
{
    cancelEvent(&createClockEventTimer_);
}

void Daemon::initialize(int stage)
{
    if (stage == INITSTAGE_LOCAL) {
        nodeId_ = static_cast<domain::NodeId>(getId());
        NodeConfig config;
        // The clock-event timer stays in this module so it keeps re-sampling the volatile
        // createClockEventInterval; only the purge timer is core-driven. A default (empty)
        // chain schedule means the core starts no clock timer — this module drives ticks.
        simtime_t expiry(par("discoveryExpiryTime").doubleValue());
        config.discovery_expiry = expiry.raw();
        node_ = std::make_unique<Node>(
            nodeId_, NodePorts{clock_, scheduler_, transport_, rng_, signer_, telemetry_, store_},
            config);
        createClockEventTimer_.setName("CreateClockEventTimer");
        scheduleCreateClockEventTimer();
        node_->start();
        WATCH(nodeId_);
    }
    else if (stage == INITSTAGE_APPLICATION_LAYER) {
        socket_.setOutputGate(gate("socketOut"));
        socket_.setCallback(this);
        socket_.bind(kPort);
    }
}

void Daemon::handleMessage(cMessage *message)
{
    if (message == &createClockEventTimer_) {
        node_->create_clock_event();
        scheduleCreateClockEventTimer();
    }
    else if (scheduler_.owns(message))
        scheduler_.fire(message);
    else if (socket_.belongsToSocket(message))
        socket_.processMessage(message);
    else
        throw cRuntimeError("Unknown message");
}

void Daemon::socketDataArrived(UdpSocket *socket, Packet *packet)
{
    const auto& chunk = packet->peekDataAsBytes();
    node_->on_packet_received(chunk->getBytes());
    delete packet;
}

void Daemon::scheduleCreateClockEventTimer()
{
    scheduleAt(simTime() + par("createClockEventInterval"), &createClockEventTimer_);
}

domain::Event Daemon::publishEvent(const domain::Bytes& data)
{
    Enter_Method_Silent();
    return node_->publish_event(data);
}

void Daemon::discoverEventChain(const domain::Event& event, ChainCallback& callback)
{
    Enter_Method_Silent();
    node_->discover_event_chain(event, callback);
}

void Daemon::discoverEventBounds(const domain::Event& event, BoundsCallback& callback)
{
    Enter_Method_Silent();
    node_->discover_event_bounds(event, callback);
}

void Daemon::discoverEventOrder(const domain::Event& event1, const domain::Event& event2,
                                OrderCallback& callback)
{
    Enter_Method_Silent();
    node_->discover_event_order(event1, event2, callback);
}

void Daemon::learnRoute(domain::NodeId destination, domain::NodeId nextHop)
{
    Enter_Method_Silent();
    node_->learn_route(destination, nextHop);
}

void Daemon::addNeighbor(domain::NodeId id, const L3Address& address)
{
    Enter_Method_Silent();
    node_->add_neighbor(id);
    transport_.set_address(id, address);
}

}
