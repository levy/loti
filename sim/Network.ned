//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

package sim;

import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;
import inet.node.inet.StandardHost;
import inet.visualizer.integrated.IntegratedCanvasVisualizer;
import loti.NetworkConfigurator;
import ned.DatarateChannel;

network SimpleNetwork
{
    parameters:
        @statistic[packetSent](title="packets sent"; source=packetSent; record=count,histogram(packetBytes); interpolationmode=none);
        @statistic[packetReceived](title="packets received"; source=packetReceived; record=count,histogram(packetBytes); interpolationmode=none);
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
        @statistic[eventOrderDiscoveryOrder](title="event order discovery result"; source=eventOrderDiscoveryOrder(eventOrderDiscoveryCompleted); record=vector,histogram; interpolationmode=none);
    types:
        channel fiberline extends DatarateChannel
        {
            delay = 1us;
            datarate = 1Gbps;
        }
    submodules:
        visualizer: IntegratedCanvasVisualizer {
            parameters:
                @display("p=100,100");
        }
        configurator: Ipv4NetworkConfigurator {
            parameters:
                @display("p=100,200");
        }
        lotiConfigurator: NetworkConfigurator {
            parameters:
                @display("p=100,300");
        }
        host[57]: StandardHost;
    connections:
        host[0].ethg++ <--> fiberline <--> host[1].ethg++;
        host[1].ethg++ <--> fiberline <--> host[2].ethg++;
        host[1].ethg++ <--> fiberline <--> host[4].ethg++;
        host[3].ethg++ <--> fiberline <--> host[4].ethg++;
        host[4].ethg++ <--> fiberline <--> host[5].ethg++;
        host[4].ethg++ <--> fiberline <--> host[7].ethg++;
        host[5].ethg++ <--> fiberline <--> host[6].ethg++;
        host[5].ethg++ <--> fiberline <--> host[10].ethg++;
        host[6].ethg++ <--> fiberline <--> host[7].ethg++;
        host[6].ethg++ <--> fiberline <--> host[9].ethg++;
        host[7].ethg++ <--> fiberline <--> host[8].ethg++;
        host[7].ethg++ <--> fiberline <--> host[12].ethg++;
        host[9].ethg++ <--> fiberline <--> host[11].ethg++;
        host[10].ethg++ <--> fiberline <--> host[11].ethg++;
        host[10].ethg++ <--> fiberline <--> host[13].ethg++;
        host[11].ethg++ <--> fiberline <--> host[12].ethg++;
        host[11].ethg++ <--> fiberline <--> host[14].ethg++;
        host[12].ethg++ <--> fiberline <--> host[15].ethg++;
        host[13].ethg++ <--> fiberline <--> host[18].ethg++;
        host[14].ethg++ <--> fiberline <--> host[15].ethg++;
        host[14].ethg++ <--> fiberline <--> host[22].ethg++;
        host[15].ethg++ <--> fiberline <--> host[16].ethg++;
        host[16].ethg++ <--> fiberline <--> host[17].ethg++;
        host[16].ethg++ <--> fiberline <--> host[23].ethg++;
        host[18].ethg++ <--> fiberline <--> host[19].ethg++;
        host[18].ethg++ <--> fiberline <--> host[21].ethg++;
        host[19].ethg++ <--> fiberline <--> host[20].ethg++;
        host[19].ethg++ <--> fiberline <--> host[26].ethg++;
        host[20].ethg++ <--> fiberline <--> host[21].ethg++;
        host[20].ethg++ <--> fiberline <--> host[24].ethg++;
        host[20].ethg++ <--> fiberline <--> host[25].ethg++;
        host[21].ethg++ <--> fiberline <--> host[22].ethg++;
        host[21].ethg++ <--> fiberline <--> host[23].ethg++;
        host[21].ethg++ <--> fiberline <--> host[24].ethg++;
        host[23].ethg++ <--> fiberline <--> host[24].ethg++;
        host[24].ethg++ <--> fiberline <--> host[28].ethg++;
        host[25].ethg++ <--> fiberline <--> host[27].ethg++;
        host[26].ethg++ <--> fiberline <--> host[32].ethg++;
        host[27].ethg++ <--> fiberline <--> host[29].ethg++;
        host[27].ethg++ <--> fiberline <--> host[30].ethg++;
        host[27].ethg++ <--> fiberline <--> host[32].ethg++;
        host[28].ethg++ <--> fiberline <--> host[29].ethg++;
        host[28].ethg++ <--> fiberline <--> host[31].ethg++;
        host[29].ethg++ <--> fiberline <--> host[31].ethg++;
        host[30].ethg++ <--> fiberline <--> host[36].ethg++;
        host[31].ethg++ <--> fiberline <--> host[33].ethg++;
        host[32].ethg++ <--> fiberline <--> host[34].ethg++;
        host[33].ethg++ <--> fiberline <--> host[37].ethg++;
        host[34].ethg++ <--> fiberline <--> host[35].ethg++;
        host[34].ethg++ <--> fiberline <--> host[40].ethg++;
        host[35].ethg++ <--> fiberline <--> host[36].ethg++;
        host[35].ethg++ <--> fiberline <--> host[39].ethg++;
        host[36].ethg++ <--> fiberline <--> host[37].ethg++;
        host[37].ethg++ <--> fiberline <--> host[38].ethg++;
        host[38].ethg++ <--> fiberline <--> host[39].ethg++;
        host[38].ethg++ <--> fiberline <--> host[42].ethg++;
        host[39].ethg++ <--> fiberline <--> host[40].ethg++;
        host[39].ethg++ <--> fiberline <--> host[41].ethg++;
        host[40].ethg++ <--> fiberline <--> host[43].ethg++;
        host[40].ethg++ <--> fiberline <--> host[44].ethg++;
        host[40].ethg++ <--> fiberline <--> host[52].ethg++;
        host[41].ethg++ <--> fiberline <--> host[42].ethg++;
        host[41].ethg++ <--> fiberline <--> host[43].ethg++;
        host[42].ethg++ <--> fiberline <--> host[43].ethg++;
        host[43].ethg++ <--> fiberline <--> host[46].ethg++;
        host[44].ethg++ <--> fiberline <--> host[45].ethg++;
        host[44].ethg++ <--> fiberline <--> host[47].ethg++;
        host[45].ethg++ <--> fiberline <--> host[46].ethg++;
        host[46].ethg++ <--> fiberline <--> host[49].ethg++;
        host[47].ethg++ <--> fiberline <--> host[48].ethg++;
        host[48].ethg++ <--> fiberline <--> host[50].ethg++;
        host[48].ethg++ <--> fiberline <--> host[51].ethg++;
        host[49].ethg++ <--> fiberline <--> host[50].ethg++;
        host[50].ethg++ <--> fiberline <--> host[53].ethg++;
        host[51].ethg++ <--> fiberline <--> host[52].ethg++;
        host[51].ethg++ <--> fiberline <--> host[53].ethg++;
        host[52].ethg++ <--> fiberline <--> host[53].ethg++;
        host[52].ethg++ <--> fiberline <--> host[54].ethg++;
        host[53].ethg++ <--> fiberline <--> host[56].ethg++;
        host[54].ethg++ <--> fiberline <--> host[55].ethg++;
        host[55].ethg++ <--> fiberline <--> host[56].ethg++;
}
