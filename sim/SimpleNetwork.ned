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
import ned.DatarateChannel;

network SimpleNetwork
{
    types:
        channel FiberLine extends DatarateChannel
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
        host[57]: StandardHost;
    connections:
        host[0].ethg++ <--> FiberLine <--> host[1].ethg++;
        host[1].ethg++ <--> FiberLine <--> host[2].ethg++;
        host[1].ethg++ <--> FiberLine <--> host[4].ethg++;
        host[3].ethg++ <--> FiberLine <--> host[4].ethg++;
        host[4].ethg++ <--> FiberLine <--> host[5].ethg++;
        host[4].ethg++ <--> FiberLine <--> host[7].ethg++;
        host[5].ethg++ <--> FiberLine <--> host[6].ethg++;
        host[5].ethg++ <--> FiberLine <--> host[10].ethg++;
        host[6].ethg++ <--> FiberLine <--> host[7].ethg++;
        host[6].ethg++ <--> FiberLine <--> host[9].ethg++;
        host[7].ethg++ <--> FiberLine <--> host[8].ethg++;
        host[7].ethg++ <--> FiberLine <--> host[12].ethg++;
        host[9].ethg++ <--> FiberLine <--> host[11].ethg++;
        host[10].ethg++ <--> FiberLine <--> host[11].ethg++;
        host[10].ethg++ <--> FiberLine <--> host[13].ethg++;
        host[11].ethg++ <--> FiberLine <--> host[12].ethg++;
        host[11].ethg++ <--> FiberLine <--> host[14].ethg++;
        host[12].ethg++ <--> FiberLine <--> host[15].ethg++;
        host[13].ethg++ <--> FiberLine <--> host[18].ethg++;
        host[14].ethg++ <--> FiberLine <--> host[15].ethg++;
        host[14].ethg++ <--> FiberLine <--> host[22].ethg++;
        host[15].ethg++ <--> FiberLine <--> host[16].ethg++;
        host[16].ethg++ <--> FiberLine <--> host[17].ethg++;
        host[16].ethg++ <--> FiberLine <--> host[23].ethg++;
        host[18].ethg++ <--> FiberLine <--> host[19].ethg++;
        host[18].ethg++ <--> FiberLine <--> host[21].ethg++;
        host[19].ethg++ <--> FiberLine <--> host[20].ethg++;
        host[19].ethg++ <--> FiberLine <--> host[26].ethg++;
        host[20].ethg++ <--> FiberLine <--> host[21].ethg++;
        host[20].ethg++ <--> FiberLine <--> host[24].ethg++;
        host[20].ethg++ <--> FiberLine <--> host[25].ethg++;
        host[21].ethg++ <--> FiberLine <--> host[22].ethg++;
        host[21].ethg++ <--> FiberLine <--> host[23].ethg++;
        host[21].ethg++ <--> FiberLine <--> host[24].ethg++;
        host[23].ethg++ <--> FiberLine <--> host[24].ethg++;
        host[24].ethg++ <--> FiberLine <--> host[28].ethg++;
        host[25].ethg++ <--> FiberLine <--> host[27].ethg++;
        host[26].ethg++ <--> FiberLine <--> host[32].ethg++;
        host[27].ethg++ <--> FiberLine <--> host[29].ethg++;
        host[27].ethg++ <--> FiberLine <--> host[30].ethg++;
        host[27].ethg++ <--> FiberLine <--> host[32].ethg++;
        host[28].ethg++ <--> FiberLine <--> host[29].ethg++;
        host[28].ethg++ <--> FiberLine <--> host[31].ethg++;
        host[29].ethg++ <--> FiberLine <--> host[31].ethg++;
        host[30].ethg++ <--> FiberLine <--> host[36].ethg++;
        host[31].ethg++ <--> FiberLine <--> host[33].ethg++;
        host[32].ethg++ <--> FiberLine <--> host[34].ethg++;
        host[33].ethg++ <--> FiberLine <--> host[37].ethg++;
        host[34].ethg++ <--> FiberLine <--> host[35].ethg++;
        host[34].ethg++ <--> FiberLine <--> host[40].ethg++;
        host[35].ethg++ <--> FiberLine <--> host[36].ethg++;
        host[35].ethg++ <--> FiberLine <--> host[39].ethg++;
        host[36].ethg++ <--> FiberLine <--> host[37].ethg++;
        host[37].ethg++ <--> FiberLine <--> host[38].ethg++;
        host[38].ethg++ <--> FiberLine <--> host[39].ethg++;
        host[38].ethg++ <--> FiberLine <--> host[42].ethg++;
        host[39].ethg++ <--> FiberLine <--> host[40].ethg++;
        host[39].ethg++ <--> FiberLine <--> host[41].ethg++;
        host[40].ethg++ <--> FiberLine <--> host[43].ethg++;
        host[40].ethg++ <--> FiberLine <--> host[44].ethg++;
        host[40].ethg++ <--> FiberLine <--> host[52].ethg++;
        host[41].ethg++ <--> FiberLine <--> host[42].ethg++;
        host[41].ethg++ <--> FiberLine <--> host[43].ethg++;
        host[42].ethg++ <--> FiberLine <--> host[43].ethg++;
        host[43].ethg++ <--> FiberLine <--> host[46].ethg++;
        host[44].ethg++ <--> FiberLine <--> host[45].ethg++;
        host[44].ethg++ <--> FiberLine <--> host[47].ethg++;
        host[45].ethg++ <--> FiberLine <--> host[46].ethg++;
        host[46].ethg++ <--> FiberLine <--> host[49].ethg++;
        host[47].ethg++ <--> FiberLine <--> host[48].ethg++;
        host[48].ethg++ <--> FiberLine <--> host[50].ethg++;
        host[48].ethg++ <--> FiberLine <--> host[51].ethg++;
        host[49].ethg++ <--> FiberLine <--> host[50].ethg++;
        host[50].ethg++ <--> FiberLine <--> host[53].ethg++;
        host[51].ethg++ <--> FiberLine <--> host[52].ethg++;
        host[51].ethg++ <--> FiberLine <--> host[53].ethg++;
        host[52].ethg++ <--> FiberLine <--> host[53].ethg++;
        host[52].ethg++ <--> FiberLine <--> host[54].ethg++;
        host[53].ethg++ <--> FiberLine <--> host[56].ethg++;
        host[54].ethg++ <--> FiberLine <--> host[55].ethg++;
        host[55].ethg++ <--> FiberLine <--> host[56].ethg++;
}
