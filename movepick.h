/*
  Glaurung, a UCI chess playing engine.
  Copyright (C) 2004-2008 Tord Romstad

  Glaurung is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  Glaurung is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#if !defined MOVEPICK_H_INCLUDED
#define MOVEPICK_H_INCLUDED

////
//// Includes
////

#include "depth.h"
#include "lock.h"
#include "position.h"


////
//// Types
////

/// MovePicker is a class which is used to pick one legal move at a time from
/// the current position.  It is initialized with a Position object and a few
/// moves we have reason to believe are good.  The most important method is
/// MovePicker::pick_next_move(), which returns a new legal move each time it
/// is called, until there are no legal moves left, when MOVE_NONE is returned.
/// In order to improve the efficiency of the alpha beta algorithm, MovePicker
/// attempts to return the moves which are most likely to be strongest first.

class MovePicker {

public:

  enum MovegenPhase {
    PH_TT_MOVE,        // Transposition table move
    PH_MATE_KILLER,    // Mate killer from the current ply
    PH_GOOD_CAPTURES,  // Queen promotions and captures with SEE values >= 0
    PH_BAD_CAPTURES,   // Queen promotions and captures with SEE valuse <= 0
    PH_KILLER_1,       // Killer move 1 from the current ply (not used yet).
    PH_KILLER_2,       // Killer move 2 from the current ply (not used yet).
    PH_NONCAPTURES,    // Non-captures and underpromotions
    PH_EVASIONS,       // Check evasions
    PH_QCAPTURES,      // Captures in quiescence search
    PH_QCHECKS,        // Checks in quiescence search
    PH_STOP
  };

  MovePicker(Position &p, bool pvnode, Move ttm, Move mk, Move k1, Move k2,
             Depth dpth);
  Move get_next_move();
  Move get_next_move(Lock &lock);
  int number_of_moves() const;
  int current_move_score() const;
  MovegenPhase current_move_type() const;
  Bitboard discovered_check_candidates() const;

  static void init_phase_table();

private:
  void score_captures();
  void score_noncaptures();
  void score_evasions();
  void score_qcaptures();
  Move pick_move_from_list();
  
  Position *pos;
  Move ttMove, mateKiller, killer1, killer2;
  Bitboard pinned, dc;
  MoveStack moves[256], badCaptures[64];
  static MovegenPhase PhaseTable[32];
  bool pvNode;
  Depth depth;
  int phaseIndex;
  int numOfMoves, numOfBadCaptures;
  int movesPicked, badCapturesPicked;
  bool finished;
};


////
//// Inline functions
////

/// MovePicker::discovered_check_candidates() returns a bitboard containing
/// all pieces which can possibly give discovered check.  This bitboard is
/// computed by the constructor function.

inline MovePicker::MovegenPhase MovePicker::current_move_type() const {
  return PhaseTable[phaseIndex];
}

inline Bitboard MovePicker::discovered_check_candidates() const {
  return dc;
}

#endif // !defined(MOVEPICK_H_INCLUDED)
