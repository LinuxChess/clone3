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


////
//// Includes
////

#include <cstring>

#include "piece.h"


////
//// Functions
////

/// Translating piece types to/from English piece letters

static const char PieceChars[] = " pnbrqk";

int piece_type_to_char(PieceType pt, bool upcase) {
  return upcase? toupper(PieceChars[pt]) : PieceChars[pt];
}

PieceType piece_type_from_char(char c) {
  const char* ch = strchr(PieceChars, tolower(c));
  return ch? PieceType(ch - PieceChars) : NO_PIECE_TYPE;
}
