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

#ifndef _LOTI_APP_SIM_NETWORKCONFIGURATOR_H_
#define _LOTI_APP_SIM_NETWORKCONFIGURATOR_H_

#include <vector>

#include <omnetpp.h>

#include "Daemon.h"

namespace loti {

// Computes the protocol overlay: for every node it installs, on that node's
// hosted Node, the shortest-path next hop toward every destination and the
// directly-reachable neighbors (with their addresses). Replaces the original's
// friend-class field-poking with the Daemon's learnRoute()/addNeighbor() API.
class NetworkConfigurator : public cSimpleModule
{
  private:
    std::vector<cModule *> networkNodes;

    int numInitStages() const override { return NUM_INIT_STAGES; }
    void initialize(int stage) override;

  public:
    Daemon *findDaemon(const cModule *networkNode);

    int getNumNetworkNodes() const { return static_cast<int>(networkNodes.size()); }
    const cModule *getNetworkNode(int index) const { return networkNodes[index]; }
};

}

#endif // ifndef _LOTI_APP_SIM_NETWORKCONFIGURATOR_H_
