/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008 Marco Costalba

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#if !defined(TT_H_INCLUDED)
#define TT_H_INCLUDED

////
//// Includes
////

#include "depth.h"
#include "position.h"
#include "value.h"


////
//// Types
////

/// The TTEntry class is the class of transposition table entries
///
/// A TTEntry needs 128 bits to be stored
///
/// bit    0-63: key
/// bit   64-95: data
/// bit  96-111: value
/// bit 112-127: depth
///
/// the 32 bits of the data field are so defined
///
/// bit  0-16: move
/// bit    17: stored value equals static value
/// bit 18-19: not used
/// bit 20-22: value type
/// bit 23-31: generation

class TTEntry {

public:
  TTEntry();
  TTEntry(Key k, Value v, ValueType t, Depth d, Move m, int generation);
  Key key() const { return key_; }
  Depth depth() const { return Depth(depth_); }
  Move move() const { return Move(data & 0x1FFFF); }
  Value value() const { return Value(value_); }
  ValueType type() const { return ValueType((data >> 20) & 7); }
  int generation() const { return (data >> 23); }
  bool staticValue() const { return ((data >> 17) & 1); }
  void setStaticValue() { data |= (1 << 17); }

private:
  Key key_;
  uint32_t data;
  int16_t value_;
  int16_t depth_;
};

/// The transposition table class.  This is basically just a huge array
/// containing TTEntry objects, and a few methods for writing new entries
/// and reading new ones.

class TranspositionTable {

public:
  TranspositionTable(unsigned mbSize);
  ~TranspositionTable();
  void set_size(unsigned mbSize);
  void clear();
  TTEntry* store(const Position &pos, Value v, Depth d, Move m, ValueType type);
  TTEntry* retrieve(const Position &pos) const;
  void new_search();
  void insert_pv(const Position &pos, Move pv[]);
  int full();

private:
  inline TTEntry* first_entry(const Position &pos) const;

  unsigned size;
  int writes;
  TTEntry* entries;
  uint8_t generation;
};


////
//// Constants and variables
////

// Default transposition table size, in megabytes:
const int TTDefaultSize = 32;


#endif // !defined(TT_H_INCLUDED)
