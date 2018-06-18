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

#include "Data.h"

namespace loti {

std::ostream& operator<<(ostream& out, const vector<uint8_t>& object)
{
    // TODO:
}

std::ostream& operator<<(ostream& out, const Event& event)
{
    return out << "Event { creator = " << event.getCreator() << ", hash = " << event.getHash() << ", salt = " << event.getSalt() << "}";
}

std::ostream& operator<<(ostream& out, const ClockEvent& clockEvent)
{
    return out << "ClockEvent { creator = " << clockEvent.getCreator() << ", hash = " << clockEvent.getHash() << ", salt = " << clockEvent.getSalt() << "}";
}

}
