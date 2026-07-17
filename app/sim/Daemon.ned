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

//
// Host module for one protocol node. All protocol logic lives in the shared
// loti-core engine (see core/); this module adapts the OMNeT++ runtime to it.
// The telemetry adapter emits precomputed numeric signals (byte sizes on the
// "created" signals, discovery times/lengths/intervals on the discovery signals),
// so the @statistic recorders below reproduce the original metrics without result
// filters or message objects.
//
simple Daemon like IApp
{
    parameters:
        volatile double createClockEventInterval @unit(s);
        double discoveryExpiryTime @unit(s);
        @display("i=block/app");
        @signal[clockEventCreated];
        @signal[eventCreated];
        @signal[eventChainDiscoveryStarted];
        @signal[eventChainDiscoveryAborted];
        @signal[eventChainDiscoveryCompleted];
        @signal[eventChainDiscoveryTime];
        @signal[eventChainDiscoveryLength];
        @signal[eventChainDiscoveryInterval];
        @signal[eventBoundsDiscoveryStarted];
        @signal[eventBoundsDiscoveryAborted];
        @signal[eventBoundsDiscoveryCompleted];
        @signal[eventBoundsDiscoveryTime];
        @signal[eventBoundsDiscoveryInterval];
        @signal[eventOrderDiscoveryStarted];
        @signal[eventOrderDiscoveryAborted];
        @signal[eventOrderDiscoveryCompleted];
        @signal[eventOrderDiscoveryTime];
        @signal[eventOrderDiscoveryOrder];
        @statistic[clockEventCreated](title="clock event created count"; source=clockEventCreated; record=count; interpolationmode=none);
        @statistic[clockEventsFileLength](title="clock events file length"; source=sum(clockEventCreated); record=vector; interpolationmode=sample-hold);
        @statistic[eventCreatedCount](title="event created count"; source=eventCreated; record=count; interpolationmode=none);
        @statistic[eventsFileLength](title="events file length"; source=sum(eventCreated); record=vector; interpolationmode=sample-hold);
        @statistic[eventChainDiscoveryStartedCount](title="event chain discovery started count"; source=eventChainDiscoveryStarted; record=count; interpolationmode=none);
        @statistic[eventChainDiscoveryAbortedCount](title="event chain discovery aborted count"; source=eventChainDiscoveryAborted; record=count; interpolationmode=none);
        @statistic[eventChainDiscoveryCompletedCount](title="event chain discovery completed count"; source=eventChainDiscoveryCompleted; record=count; interpolationmode=none);
        @statistic[eventChainDiscoveryTime](title="event chain discovery time"; source=eventChainDiscoveryTime; record=vector,histogram; interpolationmode=none);
        @statistic[eventChainDiscoveryLength](title="event chain discovery length"; source=eventChainDiscoveryLength; record=vector,histogram; interpolationmode=none);
        @statistic[eventChainDiscoveryInterval](title="event chain discovery interval"; source=eventChainDiscoveryInterval; record=vector,histogram; interpolationmode=none);
        @statistic[eventBoundsDiscoveryStartedCount](title="event bounds discovery started count"; source=eventBoundsDiscoveryStarted; record=count; interpolationmode=none);
        @statistic[eventBoundsDiscoveryAbortedCount](title="event bounds discovery aborted count"; source=eventBoundsDiscoveryAborted; record=count; interpolationmode=none);
        @statistic[eventBoundsDiscoveryCompletedCount](title="event bounds discovery completed count"; source=eventBoundsDiscoveryCompleted; record=count; interpolationmode=none);
        @statistic[eventBoundsDiscoveryTime](title="event bounds discovery time"; source=eventBoundsDiscoveryTime; record=vector,histogram; interpolationmode=none);
        @statistic[eventBoundsDiscoveryInterval](title="event bounds discovery interval"; source=eventBoundsDiscoveryInterval; record=vector,histogram; interpolationmode=none);
        @statistic[eventOrderDiscoveryStartedCount](title="event order discovery started count"; source=eventOrderDiscoveryStarted; record=count; interpolationmode=none);
        @statistic[eventOrderDiscoveryAbortedCount](title="event order discovery aborted count"; source=eventOrderDiscoveryAborted; record=count; interpolationmode=none);
        @statistic[eventOrderDiscoveryCompletedCount](title="event order discovery completed count"; source=eventOrderDiscoveryCompleted; record=count; interpolationmode=none);
        @statistic[eventOrderDiscoveryTime](title="event order discovery time"; source=eventOrderDiscoveryTime; record=vector,histogram; interpolationmode=none);
        @statistic[eventOrderDiscoveryOrder](title="event order discovery order"; source=eventOrderDiscoveryOrder; record=vector,histogram; interpolationmode=none);
    gates:
        input socketIn;
        output socketOut;
}
