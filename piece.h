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


#if !defined(PIECE_H_INCLUDED)
#define PIECE_H_INCLUDED

////
//// Includes
////

#include "color.h"
#include "misc.h"
#include "square.h"


////
//// Types
////

enum PieceType {
  NO_PIECE_TYPE = 0,
  PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6
};

enum Piece {
  NO_PIECE = 0, WP = 1, WN = 2, WB = 3, WR = 4, WQ = 5, WK = 6,
  BP = 9, BN = 10, BB = 11, BR = 12, BQ = 13, BK = 14,
  EMPTY = 16, OUTSIDE = 17
};


////
//// Constants
////

const PieceType PieceTypeMin = PAWN;
const PieceType PieceTypeMax = KING;

const int SlidingArray[18] = {
  0, 0, 0, 1, 2, 3, 0, 0, 0, 0, 0, 1, 2, 3, 0, 0, 0, 0
};

const SquareDelta Directions[16][16] = {
  {DELTA_ZERO},
  {DELTA_NW, DELTA_NE, DELTA_ZERO},
  {DELTA_SSW, DELTA_SSE, DELTA_SWW, DELTA_SEE,
   DELTA_NWW, DELTA_NEE, DELTA_NNW, DELTA_NNE, DELTA_ZERO},
  {DELTA_SE, DELTA_SW, DELTA_NE, DELTA_NW, DELTA_ZERO},
  {DELTA_S, DELTA_E, DELTA_W, DELTA_N, DELTA_ZERO},
  {DELTA_S, DELTA_E, DELTA_W, DELTA_N,
   DELTA_SE, DELTA_SW, DELTA_NE, DELTA_NW, DELTA_ZERO},
  {DELTA_S, DELTA_E, DELTA_W, DELTA_N,
   DELTA_SE, DELTA_SW, DELTA_NE, DELTA_NW, DELTA_ZERO},
  {DELTA_ZERO},
  {DELTA_ZERO},
  {DELTA_SW, DELTA_SE, DELTA_ZERO},
  {DELTA_SSW, DELTA_SSE, DELTA_SWW, DELTA_SEE,
   DELTA_NWW, DELTA_NEE, DELTA_NNW, DELTA_NNE, DELTA_ZERO},
  {DELTA_SE, DELTA_SW, DELTA_NE, DELTA_NW, DELTA_ZERO},
  {DELTA_S, DELTA_E, DELTA_W, DELTA_N, DELTA_ZERO},
  {DELTA_S, DELTA_E, DELTA_W, DELTA_N,
   DELTA_SE, DELTA_SW, DELTA_NE, DELTA_NW, DELTA_ZERO},
  {DELTA_S, DELTA_E, DELTA_W, DELTA_N,
   DELTA_SE, DELTA_SW, DELTA_NE, DELTA_NW, DELTA_ZERO},
};

const SquareDelta PawnPush[2] = {
  DELTA_N, DELTA_S
};


////
//// Inline functions
////

inline Piece operator+ (Piece p, int i) { return Piece(int(p) + i); }
inline void operator++ (Piece &p, int) { p = Piece(int(p) + 1); }
inline Piece operator- (Piece p, int i) { return Piece(int(p) - i); }
inline void operator-- (Piece &p, int) { p = Piece(int(p) - 1); }
inline PieceType operator+ (PieceType p, int i) {return PieceType(int(p) + i);}
inline void operator++ (PieceType &p, int) { p = PieceType(int(p) + 1); }
inline PieceType operator- (PieceType p, int i) {return PieceType(int(p) - i);}
inline void operator-- (PieceType &p, int) { p = PieceType(int(p) - 1); }

inline PieceType type_of_piece(Piece p)  {
  return PieceType(int(p) & 7);
}

inline Color color_of_piece(Piece p) {
  return Color(int(p) >> 3);
}

inline Piece piece_of_color_and_type(Color c, PieceType pt) {
  return Piece((int(c) << 3) | int(pt));
}

inline Piece pawn_of_color(Color c) {
  return piece_of_color_and_type(c, PAWN);
}

inline Piece knight_of_color(Color c) {
  return piece_of_color_and_type(c, KNIGHT);
}

inline Piece bishop_of_color(Color c) {
  return piece_of_color_and_type(c, BISHOP);
}

inline Piece rook_of_color(Color c) {
  return piece_of_color_and_type(c, ROOK);
}

inline Piece queen_of_color(Color c) {
  return piece_of_color_and_type(c, QUEEN);
}

inline Piece king_of_color(Color c) {
  return piece_of_color_and_type(c, KING);
}

inline int piece_is_slider(Piece p) {
  return SlidingArray[int(p)];
}

inline int piece_type_is_slider(PieceType pt) {
  return SlidingArray[int(pt)];
}

inline SquareDelta pawn_push(Color c) {
  return PawnPush[c];
}

inline bool piece_type_is_ok(PieceType pc) {
  return pc >= PAWN && pc <= KING;
}

inline bool piece_is_ok(Piece pc) {
  return piece_type_is_ok(type_of_piece(pc)) && color_is_ok(color_of_piece(pc));
}


////
//// Prototypes
////

extern int piece_type_to_char(PieceType pt, bool upcase = false);
extern PieceType piece_type_from_char(char c);


#endif // !defined(PIECE_H_INCLUDED)
