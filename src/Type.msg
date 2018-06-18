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

cplusplus {{
#include <deque>
#include <vector>
}}

namespace loti;

cplusplus {{
using namespace std;

class EventReference;
class ClockEvent;

typedef uint64_t NodeId;
typedef uint64_t Salt;
typedef vector<uint8_t> EventHash;
typedef vector<uint8_t> ByteVector;
typedef vector<EventReference> EventReferenceVector;
typedef deque<ClockEvent> ClockEventDeque;
}}

class NodeId { @existingClass; @primitive; @editable(false); }
class Salt { @existingClass; @primitive; @editable(false); }
class EventHash { @existingClass; @primitive; @editable(false); @toString(std::string("hash$")); }
class ByteVector { @existingClass; @toString(to_string($.size()) + " bytes"); }
class EventReferenceVector { @existingClass; }
class ClockEventDeque { @existingClass; }
