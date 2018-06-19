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

#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/networklayer/ipv4/Ipv4InterfaceData.h"
#include "NetworkConfigurator.h"

namespace loti {

Define_Module(NetworkConfigurator);

void NetworkConfigurator::initialize(int stage)
{
    if (stage == INITSTAGE_NETWORK_LAYER_3) {
        cTopology topology;
        L3AddressResolver resolver;
        topology.extractByProperty("networkNode");
        for (int i = 0; i < topology.getNumNodes(); i++) {
            auto destinationNode = topology.getNode(i);
            auto destinationDaemon = findDaemon(destinationNode->getModule());
            networkNodes.push_back(destinationNode->getModule());
            topology.calculateUnweightedSingleShortestPathsTo(destinationNode);
            for (int j = 0; j < topology.getNumNodes(); j++) {
                auto sourceNode = topology.getNode(j);
                auto sourceModule = sourceNode->getModule();
                auto sourceDaemon = findDaemon(sourceModule);
                if (destinationNode == sourceNode)
                    continue;
                if (sourceNode->getNumPaths() == 0)
                    continue;
                auto linkOut = sourceNode->getPath(0);
                auto nextHopNode = linkOut->getRemoteNode();
                auto nextHopModule = nextHopNode->getModule();
                auto nextHopDaemon = findDaemon(nextHopModule);
                auto nextHopInterfaceTable = resolver.findInterfaceTableOf(nextHopModule);
                auto nextHopInterfaceEntry = nextHopInterfaceTable->getInterfaceByNodeInputGateId(linkOut->getRemoteGateId());
                auto nextHopIpAddress = nextHopInterfaceEntry->ipv4Data()->getIPAddress();
                sourceDaemon->destinationToNextHop.insert({destinationDaemon->nodeId, nextHopDaemon->nodeId});
                if (sourceDaemon->neighbors.find(nextHopDaemon->nodeId) == sourceDaemon->neighbors.end()) {
                    Neighbor neighbor;
                    neighbor.setNodeId(nextHopDaemon->nodeId);
                    neighbor.setAddress(nextHopIpAddress);
                    sourceDaemon->neighbors.insert({neighbor.getNodeId(), neighbor});
                }
                auto sourceInterfaceTable = resolver.findInterfaceTableOf(sourceModule);
                auto sourceInterfaceEntry = sourceInterfaceTable->getInterfaceByNodeOutputGateId(linkOut->getLocalGateId());
                auto sourceIpAddress = sourceInterfaceEntry->ipv4Data()->getIPAddress();
                if (nextHopDaemon->neighbors.find(sourceDaemon->nodeId) == nextHopDaemon->neighbors.end()) {
                    Neighbor neighbor;
                    neighbor.setNodeId(nextHopDaemon->nodeId);
                    neighbor.setAddress(sourceIpAddress);
                    nextHopDaemon->neighbors.insert({neighbor.getNodeId(), neighbor});
                }
            }
        }
        WATCH_VECTOR(networkNodes);
    }
}

Daemon *NetworkConfigurator::findDaemon(const cModule *networkNode)
{
    return check_and_cast<Daemon *>(networkNode->getSubmodule("app", 0));
}

}
