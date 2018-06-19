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

package loti;

import inet.applications.contract.IApp;

simple Daemon like IApp
{
    parameters:
        volatile double createClockEventInterval @unit(s);
        double discoveryExpiryTime @unit(s);
        @display("i=block/app");
        @signal[clockEventCreated](type=ClockEvent);
        @signal[eventCreated](type=Event);
        @signal[eventChainDiscoveryStarted](type=EventChainDiscovery);
        @signal[eventChainDiscoveryAborted](type=EventChainDiscovery);
        @signal[eventChainDiscoveryCompleted](type=EventChainDiscovery);
        @signal[eventBoundsDiscoveryStarted](type=EventBoundsDiscovery);
        @signal[eventBoundsDiscoveryAborted](type=EventBoundsDiscovery);
        @signal[eventBoundsDiscoveryCompleted](type=EventBoundsDiscovery);
        @signal[eventOrderDiscoveryStarted](type=EventOrderDiscovery);
        @signal[eventOrderDiscoveryAborted](type=EventOrderDiscovery);
        @signal[eventOrderDiscoveryCompleted](type=EventOrderDiscovery);
        @statistic[clockEventCreated](title="clock event created count"; source=clockEventCreated; record=count; interpolationmode=none);
        @statistic[clockEventsFileLength](title="clock events file length"; source=sum(clockEventSize(clockEventCreated)); record=vector; interpolationmode=sample-hold);
        @statistic[eventCreatedCount](title="event created count"; source=eventCreated; record=count; interpolationmode=none);
        @statistic[eventsFileLength](title="events file length"; source=sum(eventSize(eventCreated)); record=vector; interpolationmode=sample-hold);
        @statistic[eventChainDiscoveryStartedCount](title="event chain discovery started count"; source=eventChainDiscoveryStarted; record=count; interpolationmode=none);
        @statistic[eventChainDiscoveryAbortedCount](title="event chain discovery aborted count"; source=eventChainDiscoveryAborted; record=count; interpolationmode=none);
        @statistic[eventChainDiscoveryCompletedCount](title="event chain discovery completed count"; source=eventChainDiscoveryCompleted; record=count; interpolationmode=none);
        @statistic[eventChainDiscoveryTime](title="event chain discovery time"; source=eventChainDiscoveryTime(eventChainDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
        @statistic[eventChainDiscoveryLength](title="event chain discovery length"; source=eventChainDiscoveryLength(eventChainDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
        @statistic[eventChainDiscoveryInterval](title="event chain discovery interval"; source=eventChainDiscoveryInterval(eventChainDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
        @statistic[eventBoundsDiscoveryStartedCount](title="event bounds discovery started count"; source=eventBoundsDiscoveryStarted; record=count; interpolationmode=none);
        @statistic[eventBoundsDiscoveryAbortedCount](title="event bounds discovery aborted count"; source=eventBoundsDiscoveryAborted; record=count; interpolationmode=none);
        @statistic[eventBoundsDiscoveryCompletedCount](title="event bounds discovery completed count"; source=eventBoundsDiscoveryCompleted; record=count; interpolationmode=none);
        @statistic[eventBoundsDiscoveryTime](title="event bounds discovery time"; source=eventBoundsDiscoveryTime(eventBoundsDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
        @statistic[eventBoundsDiscoveryInterval](title="event bounds discovery interval"; source=eventBoundsDiscoveryInterval(eventBoundsDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
        @statistic[eventOrderDiscoveryStartedCount](title="event order discovery started count"; source=eventOrderDiscoveryStarted; record=count; interpolationmode=none);
        @statistic[eventOrderDiscoveryAbortedCount](title="event order discovery aborted count"; source=eventOrderDiscoveryAborted; record=count; interpolationmode=none);
        @statistic[eventOrderDiscoveryCompletedCount](title="event order discovery completed count"; source=eventOrderDiscoveryCompleted; record=count; interpolationmode=none);
        @statistic[eventOrderDiscoveryTime](title="event order discovery time"; source=eventOrderDiscoveryTime(eventOrderDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
        @statistic[eventOrderDiscoveryOrder](title="event order discovery order"; source=eventOrderDiscoveryOrder(eventOrderDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
    gates:
        input socketIn;
        output socketOut;
}
