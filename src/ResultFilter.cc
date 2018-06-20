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

#include "ResultFilter.h"
#include "Data.h"

namespace loti {

Register_ResultFilter("eventChainDiscoveryTime", EventChainDiscoveryTimeResultFilter);

void EventChainDiscoveryTimeResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventChainDiscovery = dynamic_cast<EventChainDiscovery *>(object))
        fire(this, t, eventChainDiscovery->getEndTime() - eventChainDiscovery->getStartTime(), details);
}

Register_ResultFilter("eventChainDiscoveryLength", EventChainDiscoveryLengthResultFilter);

void EventChainDiscoveryLengthResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventChainDiscovery = dynamic_cast<EventChainDiscovery *>(object)) {
        auto& result = eventChainDiscovery->getChain();
        fire(this, t, result.getLowerBound().size() + result.getUpperBound().size() + 1, details);
    }
}

Register_ResultFilter("eventChainDiscoveryInterval", EventChainDiscoveryIntervalResultFilter);

void EventChainDiscoveryIntervalResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventChainDiscovery = dynamic_cast<EventChainDiscovery *>(object)) {
        auto& eventChain = eventChainDiscovery->getChain();
        auto lowerBound = eventChain.getLowerBound().front();
        auto upperBound = eventChain.getUpperBound().back();
        auto interval = upperBound.getTimestamp() - lowerBound.getTimestamp();
        fire(this, t, interval, details);
    }
}

Register_ResultFilter("eventBoundsDiscoveryTime", EventBoundsDiscoveryTimeResultFilter);

void EventBoundsDiscoveryTimeResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventBoundsDiscovery = dynamic_cast<EventBoundsDiscovery *>(object))
        fire(this, t, eventBoundsDiscovery->getEndTime() - eventBoundsDiscovery->getStartTime(), details);
}

Register_ResultFilter("eventBoundsDiscoveryInterval", EventBoundsDiscoveryIntervalResultFilter);

void EventBoundsDiscoveryIntervalResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventBoundsDiscovery = dynamic_cast<EventBoundsDiscovery *>(object))
        fire(this, t, eventBoundsDiscovery->getUpperBound() - eventBoundsDiscovery->getLowerBound(), details);
}

Register_ResultFilter("eventOrderDiscoveryTime", EventOrderDiscoveryTimeResultFilter);

void EventOrderDiscoveryTimeResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventOrderDiscovery = dynamic_cast<EventOrderDiscovery *>(object))
        fire(this, t, eventOrderDiscovery->getEndTime() - eventOrderDiscovery->getStartTime(), details);
}

Register_ResultFilter("eventOrderDiscoveryOrder", EventOrderDiscoveryOrderResultFilter);

void EventOrderDiscoveryOrderResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto eventOrderDiscovery = dynamic_cast<EventOrderDiscovery *>(object))
        fire(this, t, (double)eventOrderDiscovery->getOrder(), details);
}

Register_ResultFilter("clockEventSize", ClockEventSizeResultFilter);

void ClockEventSizeResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto clockEvent = dynamic_cast<ClockEvent *>(object))
        fire(this, t, calculateClockEventSize(*clockEvent).get(), details);
}

Register_ResultFilter("eventSize", EventSizeResultFilter);

void EventSizeResultFilter::receiveSignal(cResultFilter *prev, simtime_t_cref t, cObject *object, cObject *details)
{
    if (auto event = dynamic_cast<Event *>(object))
        fire(this, t, calculateEventSize(*event).get(), details);
}

}
