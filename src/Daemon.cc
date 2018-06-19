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

simsignal_t Daemon::clockEventCreatedSignal = registerSignal("clockEventCreated");
simsignal_t Daemon::eventCreatedSignal = registerSignal("eventCreated");
simsignal_t Daemon::eventChainDiscoveryStartedSignal = registerSignal("eventChainDiscoveryStarted");
simsignal_t Daemon::eventChainDiscoveryAbortedSignal = registerSignal("eventChainDiscoveryAborted");
simsignal_t Daemon::eventChainDiscoveryCompletedSignal = registerSignal("eventChainDiscoveryCompleted");
simsignal_t Daemon::eventBoundsDiscoveryStartedSignal = registerSignal("eventBoundsDiscoveryStarted");
simsignal_t Daemon::eventBoundsDiscoveryAbortedSignal = registerSignal("eventBoundsDiscoveryAborted");
simsignal_t Daemon::eventBoundsDiscoveryCompletedSignal = registerSignal("eventBoundsDiscoveryCompleted");
simsignal_t Daemon::eventOrderDiscoveryStartedSignal = registerSignal("eventOrderDiscoveryStarted");
simsignal_t Daemon::eventOrderDiscoveryAbortedSignal = registerSignal("eventOrderDiscoveryAborted");
simsignal_t Daemon::eventOrderDiscoveryCompletedSignal = registerSignal("eventOrderDiscoveryCompleted");

#ifdef NDEBUG
#define ASSERT_VALID_EVENT_CHAIN(eventChain)
#else
#define ASSERT_VALID_EVENT_CHAIN(eventChain) validateEventChain(eventChain)
#endif

Daemon::~Daemon()
{
    cancelEvent(&createClockEventTimer);
    cancelEvent(&purgeDiscoveriesTimer);
}

void Daemon::initialize(int stage)
{
    if (stage == INITSTAGE_LOCAL) {
        nodeId = getId();
        createClockEventTimer.setName("CreateClockEventTimer");
        purgeDiscoveriesTimer.setName("PurgeDiscoveriesTimer");
        scheduleCreateClockEventTimer();
        schedulePurgeDiscoveriesTimer();
        WATCH(nodeId);
        WATCH_MAP(neighbors);
        WATCH_VECTOR(allEvents);
        WATCH_VECTOR(unreferencedEvents);
        WATCH_VECTOR(allClockEvents);
        WATCH_MAP(destinationToNextHop);
        // WATCH_MAP(eventHashToEventIndex);
        // WATCH_MAP(eventHashToClockEventIndex);
        // WATCH_MAP(eventChainDiscoveries);
        // WATCH_MAP(eventBoundsDiscoveries);
        // WATCH_MAP(eventOrderDiscoveries);
    }
    else if (stage == INITSTAGE_APPLICATION_LAYER) {
        socket.setOutputGate(gate("socketOut"));
        socket.setCallback(this);
        socket.bind(666);
    }
}

void Daemon::handleMessage(cMessage *message)
{
    if (message == &createClockEventTimer) {
        processCreateClockEventTimer();
        scheduleCreateClockEventTimer();
    }
    else if (message == &purgeDiscoveriesTimer) {
        processPurgeDiscoveriesTimer();
        schedulePurgeDiscoveriesTimer();
    }
    else if (socket.belongsToSocket(message))
        socket.processMessage(message);
    else
        throw cRuntimeError("Unknown message");
}

void Daemon::socketDataArrived(UdpSocket *socket, Packet *packet)
{
    const auto& header = packet->popAtFront<LotiHeader>();
    auto it = neighbors.find(header->getNeighbor());
    if (it != neighbors.end()) {
        auto& neighbor = it->second;
        if (header->getType() == LT_CLOCK_EVENT_NOTIFICATION) {
            const auto& notification = packet->popAtFront<ClockEventNotification>();
            processClockEventNotification(neighbor, *notification.get());
        }
        else if (header->getType() == LT_EVENT_CHAIN_DISCOVERY_REQUEST) {
            const auto& request = packet->popAtFront<EventChainDiscoveryRequest>();
            auto& eventReference = request->getEvent();
            auto originator = request->getOriginator();
            processEventChainDiscoveryRequest(originator, neighbor, eventReference);
        }
        else if (header->getType() == LT_EVENT_CHAIN_DISCOVERY_RESPONSE) {
            const auto& response = packet->popAtFront<EventChainDiscoveryResponse>();
            auto originator = response->getOriginator();
            processEventChainDiscoveryResponse(originator, neighbor, response->getChain());
        }
        else
            throw cRuntimeError("Unknown header type");
    }
    else
        EV_WARN << "Received packet from unknown neighbor" << endl;
    delete packet;
}

void Daemon::processEventChainDiscoveryAborted(const Event& event)
{
    auto it = eventBoundsDiscoveries.find(event.getHash());
    if (it != eventBoundsDiscoveries.end())
        abortEventBoundsDiscovery(event.getHash());
    for (auto& it : eventOrderDiscoveries)
        if (it.first.first == event.getHash() || it.first.second == event.getHash())
            abortEventOrderDiscovery(it.first.first, it.first.second);
}

void Daemon::processEventChainDiscoveryCompleted(const Event& event, const EventChain& eventChain)
{
    const auto& hash = eventChain.getEvent().getHash();
    auto it = eventBoundsDiscoveries.find(hash);
    if (it != eventBoundsDiscoveries.end()) {
        auto& eventBoundsDiscovery = it->second.first;
        if (eventBoundsDiscovery.getState() == DS_DISCOVERY_INPROGRESS) {
            auto lowerBound = eventChain.getLowerBound().front().getTimestamp();
            auto upperBound = eventChain.getUpperBound().back().getTimestamp();
            eventBoundsDiscovery.setLowerBound(lowerBound);
            eventBoundsDiscovery.setUpperBound(upperBound);
            completeEventBoundsDiscovery(hash);
        }
    }
    for (auto& it : eventOrderDiscoveries) {
        auto& eventOrderDiscovery = it.second.first;
        if (eventOrderDiscovery.getState() == DS_DISCOVERY_INPROGRESS) {
            if (it.first.first == hash) {
                auto jt = eventChainDiscoveries.find(it.first.second);
                if (jt != eventChainDiscoveries.end() && jt->second.first.getState() == DS_DISCOVERY_COMPLETED) {
                    auto order = compareEventChains(eventChain, jt->second.first.getChain());
                    eventOrderDiscovery.setOrder(order);
                    completeEventOrderDiscovery(it.first.first, it.first.second);
                }
            }
            else if (it.first.second == hash) {
                auto jt = eventChainDiscoveries.find(it.first.first);
                if (jt != eventChainDiscoveries.end() && jt->second.first.getState() == DS_DISCOVERY_COMPLETED) {
                    auto order = compareEventChains(jt->second.first.getChain(), eventChain);
                    eventOrderDiscovery.setOrder(order);
                    completeEventOrderDiscovery(it.first.first, it.first.second);
                }
            }
        }
    }
}

void Daemon::scheduleCreateClockEventTimer()
{
    scheduleAt(simTime() + par("createClockEventInterval"), &createClockEventTimer);
}

void Daemon::processCreateClockEventTimer()
{
    auto index = allClockEvents.size();
    const auto& clockEvent = insertClockEvent();
    eventHashToClockEventIndex[clockEvent.getHash()] = index;
    for (auto& referencedEvent : clockEvent.getReferencedEvents())
        eventHashToReferencingEventIndex.insert({referencedEvent.getHash(), index});
    unreferencedEvents.clear();
    emit(clockEventCreatedSignal, &clockEvent);
    sendClockEventNotification(clockEvent);
}

void Daemon::schedulePurgeDiscoveriesTimer()
{
    double discoveryExpiryTime = par("discoveryExpiryTime");
    scheduleAt(simTime() + discoveryExpiryTime / 100, &purgeDiscoveriesTimer);
}

void Daemon::processPurgeDiscoveriesTimer()
{
    simtime_t startTimeLimit = simTime() - par("discoveryExpiryTime");
    for (auto it = eventChainDiscoveries.begin(); it != eventChainDiscoveries.end();) {
        auto& eventChainDiscovery = it->second.first;
        if (eventChainDiscovery.getStartTime() < startTimeLimit) {
            if (eventChainDiscovery.getOriginator() == nodeId && eventChainDiscovery.getState() == DS_DISCOVERY_INPROGRESS)
                abortEventChainDiscovery(eventChainDiscovery.getEvent().getHash());
            it = eventChainDiscoveries.erase(it);
        }
        else
            it++;
    }
    for (auto it = eventBoundsDiscoveries.begin(); it != eventBoundsDiscoveries.end();) {
        auto& eventBoundsDiscovery = it->second.first;
        if (eventBoundsDiscovery.getStartTime() < startTimeLimit) {
            if (eventBoundsDiscovery.getState() == DS_DISCOVERY_INPROGRESS)
                abortEventBoundsDiscovery(eventBoundsDiscovery.getEvent().getHash());
            it = eventBoundsDiscoveries.erase(it);
        }
        else
            it++;
    }
    for (auto it = eventOrderDiscoveries.begin(); it != eventOrderDiscoveries.end();) {
        auto& eventOrderDiscovery = it->second.first;
        if (eventOrderDiscovery.getStartTime() < startTimeLimit) {
            const auto& hash1 = eventOrderDiscovery.getEvent1().getHash();
            const auto& hash2 = eventOrderDiscovery.getEvent2().getHash();
            if (eventOrderDiscovery.getState() == DS_DISCOVERY_INPROGRESS)
                abortEventOrderDiscovery(hash1, hash2);
            it = eventOrderDiscoveries.erase(it);
        }
        else
            it++;
    }
}

void Daemon::sendClockEventNotification(const ClockEvent& clockEvent)
{
    for (auto& it : neighbors) {
        auto& neighbor = it.second;
        Packet *packet = new Packet("ClockEventNotification");
        const auto& header = makeShared<LotiHeader>();
        header->setType(LT_CLOCK_EVENT_NOTIFICATION);
        header->setNeighbor(nodeId);
        header->setChunkLength(calculateLotiHeaderSize(header));
        packet->insertAtBack(header);
        const auto& notification = makeShared<ClockEventNotification>();
        notification->setLastClockEventHash(clockEvent.getHash());
        notification->setNeighborLastClockEventHash(neighbor.getLastClockEventHash());
        notification->setChunkLength(calculateClockEventNotificationSize(notification));
        packet->insertAtBack(notification);
        sendToNeighbor(neighbor, packet);
    }
}

void Daemon::processClockEventNotification(Neighbor& neighbor, const ClockEventNotification& notification)
{
    neighbor.setLastClockEventHash(notification.getLastClockEventHash());
    auto index = findClockEventIndex(notification.getNeighborLastClockEventHash());
    if (index != -1) {
        auto& clockEvent = allClockEvents[index];
        for (auto& referencingEvent : clockEvent.getReferencingEvents())
            if (referencingEvent.getCreator() == neighbor.getNodeId() && referencingEvent.getHash() == notification.getNeighborLastClockEventHash())
                return;
        if (notification.getNeighborLastClockEventHash().size() != 0) {
            EventReference eventReference;
            eventReference.setCreator(neighbor.getNodeId());
            eventReference.setHash(notification.getNeighborLastClockEventHash());
            clockEvent.getReferencingEventsForUpdate().push_back(eventReference);
        }
    }
}

void Daemon::sendEventChainDiscoveryRequest(NodeId originator, const Neighbor& neighbor, const EventReference& eventReference)
{
    Packet *packet = new Packet("EventChainDiscoveryRequest");
    const auto& header = makeShared<LotiHeader>();
    header->setType(LT_EVENT_CHAIN_DISCOVERY_REQUEST);
    header->setNeighbor(nodeId);
    header->setChunkLength(calculateLotiHeaderSize(header));
    packet->insertAtBack(header);
    const auto& request = makeShared<EventChainDiscoveryRequest>();
    request->setOriginator(originator);
    request->setEvent(eventReference);
    request->setChunkLength(calculateEventChainDiscoveryRequestSize(request));
    packet->insertAtBack(request);
    sendToNeighbor(neighbor, packet);
}

void Daemon::processEventChainDiscoveryRequest(NodeId originator, const Neighbor& neighbor, const EventReference& eventReference)
{
    if (eventReference.getCreator() == nodeId) {
        EV_INFO << "Received event chain discovery request for event created by this node" << endl;
        int eventIndex = findEventIndex(eventReference.getHash());
        if (eventIndex != -1) {
            auto& event = allEvents[eventIndex];
            EventChain eventChain;
            eventChain.setEvent(event);
            if (!addLocalLowerBound(eventChain))
                EV_WARN << "Cannot add local lower bound" << endl;
            else if (!addLocalUpperBound(eventChain))
                EV_WARN << "Cannot add local upper bound" << endl;
            else if (!extendLowerBoundForNeighbor(neighbor, eventChain))
                EV_WARN << "Cannot extend lower bound for neighbor" << endl;
            else if (!extendUpperBoundForNeighbor(neighbor, eventChain))
                EV_WARN << "Cannot extend upper bound for neighbor" << endl;
            else
                sendEventChainDiscoveryResponse(originator, neighbor, eventChain);
        }
        else
            EV_WARN << "Cannot find local event for event chain discovery" << endl;
    }
    else {
        EV_INFO << "Received event chain discovery request for event created by another node" << endl;
        auto nextHopNeighbor = findNextHopNeighbor(eventReference.getCreator());
        if (nextHopNeighbor != nullptr)
            sendEventChainDiscoveryRequest(originator, *nextHopNeighbor, eventReference);
        else
            EV_WARN << "Next hop not found for event chain discovery request" << endl;
    }
}

void Daemon::sendEventChainDiscoveryResponse(NodeId originator, const Neighbor& neighbor, const EventChain& eventChain)
{
    Packet *packet = new Packet("EventChainDiscoveryResponse");
    const auto& header = makeShared<LotiHeader>();
    header->setType(LT_EVENT_CHAIN_DISCOVERY_RESPONSE);
    header->setNeighbor(nodeId);
    header->setChunkLength(calculateLotiHeaderSize(header));
    packet->insertAtBack(header);
    const auto& response = makeShared<EventChainDiscoveryResponse>();
    response->setOriginator(originator);
    response->setChain(eventChain);
    response->setChunkLength(calculateEventChainDiscoveryResponseSize(response));
    packet->insertAtBack(response);
    sendToNeighbor(neighbor, packet);
}

void Daemon::processEventChainDiscoveryResponse(NodeId originator, const Neighbor& neighbor, const EventChain& eventChain)
{
    const auto& hash = eventChain.getEvent().getHash();
    if (originator == nodeId) {
        auto it = eventChainDiscoveries.find(hash);
        if (it != eventChainDiscoveries.end()) {
            auto& eventChainDiscovery = it->second.first;
            switch (eventChainDiscovery.getState()) {
                case DS_DISCOVERY_INPROGRESS: {
                    eventChainDiscovery.setChain(eventChain);
                    auto& eventChain = eventChainDiscovery.getChainForUpdate();
                    if (!addLocalLowerBound(eventChain)) {
                        EV_WARN << "Cannot add local lower bound" << endl;
                        abortEventChainDiscovery(hash);
                    }
                    else if (!addLocalUpperBound(eventChain)) {
                        EV_WARN << "Cannot add local upper bound" << endl;
                        abortEventChainDiscovery(hash);
                    }
                    else
                        completeEventChainDiscovery(hash);
                    break;
                }
                case DS_DISCOVERY_ABORTED:
                    EV_WARN << "Event chain discovery already aborted";
                    break;
                case DS_DISCOVERY_COMPLETED:
                    EV_WARN << "Event chain discovery already completed";
                    break;
                default:
                    throw cRuntimeError("Unknown event chain discovery state");
            }
        }
        else
            EV_WARN << "Event chain discovery missing";
    }
    else {
        auto nextHopNeighbor = findNextHopNeighbor(originator);
        if (nextHopNeighbor != nullptr) {
            EventChain updatedEventChain = eventChain;
            if (!addLocalLowerBound(updatedEventChain))
                EV_WARN << "Cannot add local lower bound" << endl;
            else if (!addLocalUpperBound(updatedEventChain))
                EV_WARN << "Cannot add local upper bound" << endl;
            else if (!extendLowerBoundForNeighbor(neighbor, updatedEventChain))
                EV_WARN << "Cannot extend lower bound for neighbor" << endl;
            else if (!extendUpperBoundForNeighbor(neighbor, updatedEventChain))
                EV_WARN << "Cannot extend upper bound for neighbor" << endl;
            else
                sendEventChainDiscoveryResponse(originator, *nextHopNeighbor, updatedEventChain);
        }
        else
            EV_WARN << "Next hop not found for event chain discovery response" << endl;
    }
}

const Event& Daemon::publishEvent(const vector<uint8_t>& data)
{
    Enter_Method_Silent();
    auto index = allEvents.size();
    const auto& event = insertEvent(data);
    eventHashToEventIndex[event.getHash()] = index;
    unreferencedEvents.push_back(event);
    emit(eventCreatedSignal, &event);
    return event;
}

void Daemon::discoverEventChain(const Event& event, IEventChainDiscoveryCallback& callback)
{
    Enter_Method_Silent();
    auto it = eventChainDiscoveries.find(event.getHash());
    if (it != eventChainDiscoveries.end()) {
        auto& eventChainDiscovery = it->second.first;
        switch (eventChainDiscovery.getState()) {
            case DS_DISCOVERY_INPROGRESS:
                it->second.second.push_back(&callback);
                break;
            case DS_DISCOVERY_ABORTED:
                callback.processEventChainDiscoveryAborted(event);
                break;
            case DS_DISCOVERY_COMPLETED:
                callback.processEventChainDiscoveryCompleted(event, eventChainDiscovery.getChain());
                break;
            default:
                throw cRuntimeError("Unknown event chain discovery state");
        }
    }
    else {
        auto& eventChainDiscovery = insertEventChainDiscovery(event, nodeId, &callback);
        emit(eventChainDiscoveryStartedSignal, &eventChainDiscovery);
        if (event.getCreator() == nodeId) {
            auto& eventChain = eventChainDiscovery.getChainForUpdate();
            if (!addLocalLowerBound(eventChain)) {
                EV_WARN << "Cannot add local lower bound" << endl;
                abortEventChainDiscovery(event.getHash());
            }
            else if (!addLocalUpperBound(eventChain)) {
                EV_WARN << "Cannot add local upper bound" << endl;
                abortEventChainDiscovery(event.getHash());
            }
            else
                completeEventChainDiscovery(event.getHash());
        }
        else {
            auto nextHopNeighbor = findNextHopNeighbor(event.getCreator());
            if (nextHopNeighbor != nullptr) {
                auto& event = eventChainDiscovery.getEvent();
                EventReference eventReference;
                eventReference.setCreator(event.getCreator());
                eventReference.setHash(event.getHash());
                sendEventChainDiscoveryRequest(nodeId, *nextHopNeighbor, eventReference);
            }
            else
                EV_WARN << "Next hop not found for event chain discovery request" << endl;
        }
    }
}

void Daemon::discoverEventBounds(const Event& event, IEventBoundsDiscoveryCallback& callback)
{
    Enter_Method_Silent();
    auto it = eventBoundsDiscoveries.find(event.getHash());
    if (it != eventBoundsDiscoveries.end()) {
        auto& eventBoundsDiscovery = it->second.first;
        switch (eventBoundsDiscovery.getState()) {
            case DS_DISCOVERY_INPROGRESS:
                it->second.second.push_back(&callback);
                break;
            case DS_DISCOVERY_ABORTED:
                callback.processEventBoundsDiscoveryAborted(event);
                break;
            case DS_DISCOVERY_COMPLETED:
                callback.processEventBoundsDiscoveryCompleted(event, eventBoundsDiscovery.getLowerBound(), eventBoundsDiscovery.getUpperBound());
                break;
            default:
                throw cRuntimeError("Unknown event bounds discovery state");
        }
    }
    else {
        auto& eventBoundsDiscovery = insertEventBoundsDiscovery(event, callback);
        emit(eventBoundsDiscoveryStartedSignal, &eventBoundsDiscovery);
        discoverEventChain(event, *this);
    }
}

void Daemon::discoverEventOrder(const Event& event1, const Event& event2, IEventOrderDiscoveryCallback& callback)
{
    Enter_Method_Silent();
    auto it = eventOrderDiscoveries.find({event1.getHash(), event2.getHash()});
    if (it != eventOrderDiscoveries.end()) {
        auto& eventOrderDiscovery = it->second.first;
        switch (eventOrderDiscovery.getState()) {
            case DS_DISCOVERY_INPROGRESS:
                it->second.second.push_back(&callback);
                break;
            case DS_DISCOVERY_ABORTED:
                callback.processEventOrderDiscoveryAborted(event1, event2);
                break;
            case DS_DISCOVERY_COMPLETED:
                callback.processEventOrderDiscoveryCompleted(event1, event2, eventOrderDiscovery.getOrder());
                break;
            default:
                throw cRuntimeError("Unknown event order discovery state");
        }
    }
    else {
        auto& eventOrderDiscovery = insertEventOrderDiscovery(event1, event2, callback);
        emit(eventOrderDiscoveryStartedSignal, &eventOrderDiscovery);
        discoverEventChain(event1, *this);
        discoverEventChain(event2, *this);
    }
}

EventChainDiscovery& Daemon::insertEventChainDiscovery(const Event& event, NodeId originator, IEventChainDiscoveryCallback *callback)
{
    EventChain eventChain;
    eventChain.setEvent(event);
    EventChainDiscovery eventChainDiscovery;
    eventChainDiscovery.setStartTime(simTime());
    eventChainDiscovery.setEvent(event);
    eventChainDiscovery.setOriginator(originator);
    eventChainDiscovery.setChain(eventChain);
    eventChainDiscovery.setState(DS_DISCOVERY_INPROGRESS);
    auto it = eventChainDiscoveries.insert({event.getHash(), {eventChainDiscovery, {callback}}});
    return it.first->second.first;
}

void Daemon::abortEventChainDiscovery(const EventHash& hash)
{
    auto it = eventChainDiscoveries.find(hash);
    ASSERT(it != eventChainDiscoveries.end());
    auto& eventChainDiscovery = it->second.first;
    ASSERT(eventChainDiscovery.getState() == DS_DISCOVERY_INPROGRESS);
    eventChainDiscovery.setEndTime(simTime());
    eventChainDiscovery.setState(DS_DISCOVERY_ABORTED);
    for (auto& callback : it->second.second)
        callback->processEventChainDiscoveryAborted(eventChainDiscovery.getEvent());
    emit(eventChainDiscoveryAbortedSignal, &eventChainDiscovery);
}

void Daemon::completeEventChainDiscovery(const EventHash& hash)
{
    auto it = eventChainDiscoveries.find(hash);
    ASSERT(it != eventChainDiscoveries.end());
    auto& eventChainDiscovery = it->second.first;
    ASSERT(eventChainDiscovery.getState() == DS_DISCOVERY_INPROGRESS);
    eventChainDiscovery.setEndTime(simTime());
    eventChainDiscovery.setState(DS_DISCOVERY_COMPLETED);
    validateEventChainDiscoveryResult(eventChainDiscovery);
    for (auto& callback : it->second.second)
        callback->processEventChainDiscoveryCompleted(eventChainDiscovery.getEvent(), eventChainDiscovery.getChain());
    emit(eventChainDiscoveryCompletedSignal, &eventChainDiscovery);
}

EventBoundsDiscovery& Daemon::insertEventBoundsDiscovery(const Event& event, IEventBoundsDiscoveryCallback& callback)
{
    EventBoundsDiscovery eventBoundsDiscovery;
    eventBoundsDiscovery.setStartTime(simTime());
    eventBoundsDiscovery.setEvent(event);
    eventBoundsDiscovery.setState(DS_DISCOVERY_INPROGRESS);
    auto it = eventBoundsDiscoveries.insert({event.getHash(), {eventBoundsDiscovery, {&callback}}});
    return it.first->second.first;
}

void Daemon::abortEventBoundsDiscovery(const EventHash& hash)
{
    auto it = eventBoundsDiscoveries.find(hash);
    ASSERT(it != eventBoundsDiscoveries.end());
    auto& eventBoundsDiscovery = it->second.first;
    ASSERT(eventBoundsDiscovery.getState() == DS_DISCOVERY_INPROGRESS);
    eventBoundsDiscovery.setEndTime(simTime());
    eventBoundsDiscovery.setState(DS_DISCOVERY_ABORTED);
    for (auto& callback : it->second.second)
        callback->processEventBoundsDiscoveryAborted(eventBoundsDiscovery.getEvent());
    emit(eventBoundsDiscoveryAbortedSignal, &eventBoundsDiscovery);
}

void Daemon::completeEventBoundsDiscovery(const EventHash& hash)
{
    auto it = eventBoundsDiscoveries.find(hash);
    ASSERT(it != eventBoundsDiscoveries.end());
    auto& eventBoundsDiscovery = it->second.first;
    ASSERT(eventBoundsDiscovery.getState() == DS_DISCOVERY_INPROGRESS);
    eventBoundsDiscovery.setEndTime(simTime());
    eventBoundsDiscovery.setState(DS_DISCOVERY_COMPLETED);
    for (auto& callback : it->second.second)
        callback->processEventBoundsDiscoveryCompleted(eventBoundsDiscovery.getEvent(), eventBoundsDiscovery.getLowerBound(), eventBoundsDiscovery.getUpperBound());
    emit(eventBoundsDiscoveryCompletedSignal, &eventBoundsDiscovery);
}

EventOrderDiscovery& Daemon::insertEventOrderDiscovery(const Event& event1, const Event& event2, IEventOrderDiscoveryCallback& callback)
{
    EventOrderDiscovery eventOrderDiscovery;
    eventOrderDiscovery.setStartTime(simTime());
    eventOrderDiscovery.setEvent1(event1);
    eventOrderDiscovery.setEvent2(event2);
    eventOrderDiscovery.setState(DS_DISCOVERY_INPROGRESS);
    auto it = eventOrderDiscoveries.insert({{event1.getHash(), event2.getHash()}, {eventOrderDiscovery, {&callback}}});
    return it.first->second.first;
}

void Daemon::abortEventOrderDiscovery(const EventHash& hash1, const EventHash& hash2)
{
    auto it = eventOrderDiscoveries.find({hash1, hash2});
    ASSERT(it != eventOrderDiscoveries.end());
    auto& eventOrderDiscovery = it->second.first;
    ASSERT(eventOrderDiscovery.getState() == DS_DISCOVERY_INPROGRESS);
    eventOrderDiscovery.setEndTime(simTime());
    eventOrderDiscovery.setState(DS_DISCOVERY_ABORTED);
    for (auto& callback : it->second.second)
        callback->processEventOrderDiscoveryAborted(eventOrderDiscovery.getEvent1(), eventOrderDiscovery.getEvent2());
    emit(eventOrderDiscoveryAbortedSignal, &eventOrderDiscovery);
}

void Daemon::completeEventOrderDiscovery(const EventHash& hash1, const EventHash& hash2)
{
    auto it = eventOrderDiscoveries.find({hash1, hash2});
    ASSERT(it != eventOrderDiscoveries.end());
    auto& eventOrderDiscovery = it->second.first;
    ASSERT(eventOrderDiscovery.getState() == DS_DISCOVERY_INPROGRESS);
    eventOrderDiscovery.setEndTime(simTime());
    eventOrderDiscovery.setState(DS_DISCOVERY_COMPLETED);
    for (auto& callback : it->second.second)
        callback->processEventOrderDiscoveryCompleted(eventOrderDiscovery.getEvent1(), eventOrderDiscovery.getEvent2(), eventOrderDiscovery.getOrder());
    emit(eventOrderDiscoveryCompletedSignal, &eventOrderDiscovery);
}

void Daemon::sendToNeighbor(const Neighbor& neighbor, Packet *packet)
{
    socket.sendTo(packet, neighbor.getAddress(), 666);
}

void Daemon::validateEventChain(const EventChain& eventChain)
{
    EventReference previousEventReference;
    // validate lower bound chain
    for (auto it = eventChain.getLowerBound().begin(); it != eventChain.getLowerBound().end(); it++) {
        auto& clockEvent = *it;
        if (calculateClockEventHash(clockEvent) != clockEvent.getHash())
            throw cRuntimeError("Invalid event hash");
        if (it != eventChain.getLowerBound().begin()) {
            for (auto& eventReference : clockEvent.getReferencedEvents()) {
                if (eventReference.getCreator() == previousEventReference.getCreator() && eventReference.getHash() == previousEventReference.getHash())
                    goto lowerFound;
            }
            throw cRuntimeError("Invalid lower bound");
            lowerFound:;
        }
        previousEventReference.setCreator(clockEvent.getCreator());
        previousEventReference.setHash(clockEvent.getHash());
    }
    // validate event
    for (auto& eventReference : eventChain.getEvent().getReferencedEvents()) {
        if (eventReference.getCreator() == previousEventReference.getCreator() && eventReference.getHash() == previousEventReference.getHash())
            goto eventFound;
    }
    if (eventChain.getLowerBound().size() != 0)
        throw cRuntimeError("Invalid event");
    eventFound:;
    previousEventReference.setCreator(eventChain.getEvent().getCreator());
    previousEventReference.setHash(eventChain.getEvent().getHash());
    // validate upper bound chain
    for (auto it = eventChain.getUpperBound().begin(); it != eventChain.getUpperBound().end(); it++) {
        auto& clockEvent = *it;
        if (calculateClockEventHash(clockEvent) != clockEvent.getHash())
            throw cRuntimeError("Invalid event hash");
        if (it != eventChain.getUpperBound().begin()) {
            for (auto& eventReference : clockEvent.getReferencedEvents()) {
                if (eventReference.getCreator() == previousEventReference.getCreator() && eventReference.getHash() == previousEventReference.getHash())
                    goto upperFound;
            }
            throw cRuntimeError("Invalid upper bound");
            upperFound:;
        }
        previousEventReference.setCreator(clockEvent.getCreator());
        previousEventReference.setHash(clockEvent.getHash());
    }
}

void Daemon::validateEventChainDiscoveryResult(const EventChainDiscovery& eventChainDiscovery)
{
    auto& eventChain = eventChainDiscovery.getChain();
    if (eventChain.getLowerBound().front().getCreator() != nodeId)
        throw cRuntimeError("Invalid first clock event");
    if (eventChain.getUpperBound().back().getCreator() != nodeId)
        throw cRuntimeError("Invalid last clock event");
    validateEventChain(eventChain);
}

bool Daemon::addLocalLowerBound(EventChain& eventChain)
{
    auto& referencedEvents = eventChain.getLowerBound().size() != 0 ? eventChain.getLowerBound().front().getReferencedEvents() : eventChain.getEvent().getReferencedEvents();
    for (auto& referencedEvent : referencedEvents) {
        if (referencedEvent.getCreator() == nodeId) {
            auto index = findClockEventIndex(referencedEvent.getHash());
            if (index != -1) {
                auto& clockEvent = allClockEvents[index];
                eventChain.getLowerBoundForUpdate().push_front(clockEvent);
                ASSERT_VALID_EVENT_CHAIN(eventChain);
                return true;
            }
        }
    }
    return false;
}

bool Daemon::addLocalUpperBound(EventChain& eventChain)
{
    auto referencedEventHash = eventChain.getUpperBound().size() != 0 ? eventChain.getUpperBound().back().getHash() : eventChain.getEvent().getHash();
    auto it = eventHashToReferencingEventIndex.lower_bound(referencedEventHash);
    auto jt = eventHashToReferencingEventIndex.upper_bound(referencedEventHash);
    for (; it != jt; it++) {
        auto index = it->second;
        auto& localClockEvent = allClockEvents[index];
        eventChain.getUpperBoundForUpdate().push_back(localClockEvent);
        ASSERT_VALID_EVENT_CHAIN(eventChain);
        return true;
    }
    return false;
}

bool Daemon::extendLowerBoundForNeighbor(const Neighbor& neighbor, EventChain& eventChain)
{
    auto& clockEvent = eventChain.getLowerBoundForUpdate().front();
    ASSERT(clockEvent.getCreator() == nodeId);
    unsigned int index = getClockEventIndex(clockEvent.getHash());
    while (true) {
        for (auto& referencedClockEvent : clockEvent.getReferencedEvents())
            if (referencedClockEvent.getCreator() == neighbor.getNodeId())
                return true;
        if (index == 0)
            return false;
        else {
            index--;
            auto& clockEvent = allClockEvents[index];
            eventChain.getLowerBoundForUpdate().push_front(clockEvent);
            ASSERT_VALID_EVENT_CHAIN(eventChain);
        }
    }
}

bool Daemon::extendUpperBoundForNeighbor(const Neighbor& neighbor, EventChain& eventChain)
{
    auto& clockEvent = eventChain.getUpperBoundForUpdate().back();
    ASSERT(clockEvent.getCreator() == nodeId);
    unsigned int index = getClockEventIndex(clockEvent.getHash());
    auto& localClockEvent = allClockEvents[index];
    while (true) {
        for (auto& referencingClockEvent : localClockEvent.getReferencingEvents())
            if (referencingClockEvent.getCreator() == neighbor.getNodeId())
                return true;
        if (index == allClockEvents.size() - 1)
            return false;
        else {
            index++;
            auto& clockEvent = allClockEvents[index];
            eventChain.getUpperBoundForUpdate().push_back(clockEvent);
            ASSERT_VALID_EVENT_CHAIN(eventChain);
        }
    }
}

int Daemon::compareEventChains(const EventChain& eventChain1, const EventChain& eventChain2)
{
    if (eventChain1.getUpperBound().back().getTimestamp() < eventChain2.getLowerBound().front().getTimestamp())
        return -1;
    else if (eventChain2.getUpperBound().back().getTimestamp() < eventChain1.getLowerBound().front().getTimestamp())
        return 1;
    else
        return 0;
}

const Event& Daemon::insertEvent(const vector<uint8_t>& data)
{
    vector<EventReference> referencedEvents;
    if (allClockEvents.size() != 0) {
        auto& lastClockEvent = allClockEvents.back();
        EventReference eventReference;
        eventReference.setCreator(lastClockEvent.getCreator());
        eventReference.setHash(lastClockEvent.getHash());
        referencedEvents.push_back(eventReference);
    }
    Event event;
    event.setData(data);
    event.setCreator(nodeId);
    event.setSalt(generateSalt());
    event.setReferencedEvents(referencedEvents);
    event.setHash(calculateEventHash(event));
    allEvents.push_back(event);
    return allEvents.back();
}

const LocalClockEvent& Daemon::insertClockEvent()
{
    vector<EventReference> referencedEvents;
    if (allClockEvents.size() != 0) {
        auto& lastClockEvent = allClockEvents.back();
        EventReference eventReference;
        eventReference.setCreator(lastClockEvent.getCreator());
        eventReference.setHash(lastClockEvent.getHash());
        referencedEvents.push_back(eventReference);
    }
    for (auto& it : neighbors) {
        auto& neighbor = it.second;
        if (neighbor.getLastClockEventHash().size() != 0) {
            EventReference eventReference;
            eventReference.setCreator(neighbor.getNodeId());
            eventReference.setHash(neighbor.getLastClockEventHash());
            referencedEvents.push_back(eventReference);
        }
    }
    for (auto& event : unreferencedEvents) {
        EventReference eventReference;
        eventReference.setCreator(event.getCreator());
        eventReference.setHash(event.getHash());
        referencedEvents.push_back(eventReference);
    }
    LocalClockEvent clockEvent;
    clockEvent.setTimestamp(simTime());
    clockEvent.setCreator(nodeId);
    clockEvent.setSalt(generateSalt());
    clockEvent.setReferencedEvents(referencedEvents);
    clockEvent.setHash(calculateClockEventHash(clockEvent));
    allClockEvents.push_back(clockEvent);
    return allClockEvents.back();
}

int Daemon::findClockEventIndex(const EventHash eventHash)
{
    auto it = eventHashToClockEventIndex.find(eventHash);
    return it != eventHashToClockEventIndex.end() ? it->second : -1;
}

unsigned int Daemon::getClockEventIndex(const EventHash eventHash)
{
    auto index = findClockEventIndex(eventHash);
    if (index == -1)
        throw cRuntimeError("Cannot find clock event");
    else
        return index;
}

int Daemon::findEventIndex(const EventHash& eventHash)
{
    auto it = eventHashToEventIndex.find(eventHash);
    return it != eventHashToEventIndex.end() ? it->second : -1;
}

unsigned int Daemon::getEventIndex(const EventHash& eventHash)
{
    auto index = findEventIndex(eventHash);
    if (index == -1)
        throw cRuntimeError("Cannot find event");
    else
        return index;
}

const Neighbor *Daemon::findNextHopNeighbor(NodeId nodeId)
{
    auto it = destinationToNextHop.find(nodeId);
    if (it == destinationToNextHop.end())
        return nullptr;
    else {
        auto jt = neighbors.find(it->second);
        if (jt == neighbors.end())
            return nullptr;
        else
            return &jt->second;
    }
}

Salt Daemon::generateSalt()
{
    Salt salt = intuniform(0, 0xFFFFFFFF);
    salt <<= 32;
    salt |= intuniform(0, 0xFFFFFFFF);
    return salt;
}

EventHash Daemon::calculateEventHash(const Event& event)
{
    MemoryOutputStream stream;
    stream.writeBytes(event.getData());
    stream.writeUint64Be(event.getSalt());
    for (auto& referencedEvent : event.getReferencedEvents()) {
        stream.writeUint64Be(referencedEvent.getCreator());
        stream.writeBytes(referencedEvent.getHash());
    }
    EventHash hash(picosha2::k_digest_size);
    picosha2::hash256(stream.getData(), hash);
    return hash;
}

EventHash Daemon::calculateClockEventHash(const ClockEvent& clockEvent)
{
    MemoryOutputStream stream;
    stream.writeUint64Be(clockEvent.getTimestamp().raw());
    stream.writeUint64Be(clockEvent.getSalt());
    for (auto& referencedEvent : clockEvent.getReferencedEvents()) {
        stream.writeUint64Be(referencedEvent.getCreator());
        stream.writeBytes(referencedEvent.getHash());
    }
    EventHash hash(picosha2::k_digest_size);
    picosha2::hash256(stream.getData(), hash);
    return hash;
}

}
