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

#ifndef _LOTI_PUBLISHER_H_
#define _LOTI_PUBLISHER_H_

#include <vector>
#include <omnetpp.h>
#include "Daemon.h"

namespace loti {

class Publisher : public cSimpleModule
{
  private:
    Daemon *daemon = nullptr;

    cMessage createEventTimer;

  protected:
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(cMessage *message) override;

  private:
    void scheduleCreateEventTimer();
    void processCreateEventTimer();

    const Event& createEvent();

  public:
    virtual ~Publisher();
};

}

#endif // ifndef _LOTI_PUBLISHER_H_
