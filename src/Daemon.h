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

#ifndef _LOTI_DAEMON_H_
#define _LOTI_DAEMON_H_

#include <omnetpp.h>
#include "inet/transportlayer/contract/udp/UdpSocket.h"
#include "Data.h"

namespace loti {

using namespace omnetpp;
using namespace inet;

class NetworkConfigurator;

class IEventChainDiscoveryCallback
{
  public:
    virtual void processEventChainDiscoveryAborted(const Event& event) = 0;
    virtual void processEventChainDiscoveryCompleted(const Event& event, const EventChain& eventChain) = 0;
};

class IEventBoundsDiscoveryCallback
{
  public:
    virtual void processEventBoundsDiscoveryAborted(const Event& event) = 0;
    virtual void processEventBoundsDiscoveryCompleted(const Event& event, simtime_t lowerBound, simtime_t upperBound) = 0;
};

class IEventOrderDiscoveryCallback
{
  public:
    virtual void processEventOrderDiscoveryAborted(const Event& event1, const Event& event2) = 0;
    virtual void processEventOrderDiscoveryCompleted(const Event& event1, const Event& event2, int order) = 0;
};

class Daemon : public cSimpleModule, public UdpSocket::ICallback, public IEventChainDiscoveryCallback
{
  friend class NetworkConfigurator;

  private:
    static simsignal_t clockEventCreatedSignal;
    static simsignal_t eventCreatedSignal;
    static simsignal_t eventChainDiscoveryStartedSignal;
    static simsignal_t eventChainDiscoveryAbortedSignal;
    static simsignal_t eventChainDiscoveryCompletedSignal;
    static simsignal_t eventBoundsDiscoveryStartedSignal;
    static simsignal_t eventBoundsDiscoveryAbortedSignal;
    static simsignal_t eventBoundsDiscoveryCompletedSignal;
    static simsignal_t eventOrderDiscoveryStartedSignal;
    static simsignal_t eventOrderDiscoveryAbortedSignal;
    static simsignal_t eventOrderDiscoveryCompletedSignal;

  private:
    NodeId nodeId;

    map<NodeId, Neighbor> neighbors;

    vector<Event> allEvents;
    vector<Event> unreferencedEvents;
    vector<LocalClockEvent> allClockEvents;

    map<NodeId, NodeId> destinationToNextHop;

    map<EventHash, unsigned int> eventHashToEventIndex;
    map<EventHash, unsigned int> eventHashToClockEventIndex;
    multimap<EventHash, unsigned int> eventHashToReferencingEventIndex;

    map<EventHash, pair<EventChainDiscovery, vector<IEventChainDiscoveryCallback *>>> eventChainDiscoveries;
    map<EventHash, pair<EventBoundsDiscovery, vector<IEventBoundsDiscoveryCallback *>>> eventBoundsDiscoveries;
    map<pair<EventHash, EventHash>, pair<EventOrderDiscovery, vector<IEventOrderDiscoveryCallback *>>> eventOrderDiscoveries;

    UdpSocket socket;

    cMessage createClockEventTimer;
    cMessage purgeDiscoveriesTimer;

  public:
    virtual ~Daemon();

    const Event& publishEvent(const vector<uint8_t>& data);

    void discoverEventChain(const Event& event, IEventChainDiscoveryCallback& callback);
    void discoverEventBounds(const Event& event, IEventBoundsDiscoveryCallback& callback);
    void discoverEventOrder(const Event& event1, const Event& event2, IEventOrderDiscoveryCallback& callback);

    int getNumEvents() const { return allEvents.size(); }
    const Event& getEvent(int index) const { return allEvents[index]; }

  protected:
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(cMessage *message) override;

    virtual void socketDataArrived(UdpSocket *socket, Packet *packet) override;
    virtual void socketErrorArrived(UdpSocket *socket, Indication *indication) override { delete indication; }

    virtual void processEventChainDiscoveryAborted(const Event& event) override;
    virtual void processEventChainDiscoveryCompleted(const Event& event, const EventChain& eventChain) override;

  private:
    void scheduleCreateClockEventTimer();
    void processCreateClockEventTimer();

    void schedulePurgeDiscoveriesTimer();
    void processPurgeDiscoveriesTimer();

    void sendClockEventNotification(const Neighbor& neighbor, const ClockEvent& clockEvent);
    void processClockEventNotification(Neighbor& neighbor, const EventHash& lastClockEventHash, const EventHash& neighborLastClockEventHash);

    void sendEventChainDiscoveryRequest(NodeId originator, const Neighbor& neighbor, const EventReference& eventReference);
    void processEventChainDiscoveryRequest(NodeId originator, const Neighbor& neighbor, const EventReference& eventReference);

    void sendEventChainDiscoveryResponse(NodeId originator, const Neighbor& neighbor, const EventChain& eventChain);
    void processEventChainDiscoveryResponse(NodeId originator, const Neighbor& neighbor, const EventChain& eventChain);

    EventChainDiscovery& insertEventChainDiscovery(const Event& event, NodeId originator, IEventChainDiscoveryCallback *callback);
    void abortEventChainDiscovery(const EventHash& hash);
    void completeEventChainDiscovery(const EventHash& hash);

    EventBoundsDiscovery& insertEventBoundsDiscovery(const Event& event, IEventBoundsDiscoveryCallback& callback);
    void abortEventBoundsDiscovery(const EventHash& hash);
    void completeEventBoundsDiscovery(const EventHash& hash);

    EventOrderDiscovery& insertEventOrderDiscovery(const Event& event1, const Event& event2, IEventOrderDiscoveryCallback& callback);
    void abortEventOrderDiscovery(const EventHash& hash1, const EventHash& hash2);
    void completeEventOrderDiscovery(const EventHash& hash1, const EventHash& hash2);

    void sendToNeighbor(const Neighbor& neighbor, Packet *packet);

    void validateEventChain(const EventChain& eventChain) const;
    void validateEventChainDiscoveryResult(const EventChainDiscovery& eventChainDiscovery) const;

    bool addLocalLowerBound(EventChain& eventChain) const;
    bool addLocalUpperBound(EventChain& eventChain) const;

    bool extendLowerBoundForNeighbor(const Neighbor& neighbor, EventChain& eventChain) const;
    bool extendUpperBoundForNeighbor(const Neighbor& neighbor, EventChain& eventChain) const;

    int compareEventChains(const EventChain& eventChain1, const EventChain& eventChain2) const;

    const Event& insertEvent(const vector<uint8_t>& data);
    const LocalClockEvent& insertClockEvent();

    int findClockEventIndex(const EventHash& eventHash) const;
    unsigned int getClockEventIndex(const EventHash& eventHash) const;

    int findEventIndex(const EventHash& eventHash) const;
    unsigned int getEventIndex(const EventHash& eventHash) const;

    const Neighbor *findNextHopNeighbor(NodeId nodeId) const;

    Salt generateSalt() const;

    EventHash calculateEventHash(const Event& event) const;
    EventHash calculateClockEventHash(const ClockEvent& clockEvent) const;
};

}

#endif // ifndef _LOTI_DAEMON_H_
