/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2010 Marco Costalba, Joona Kiiski, Tord Romstad

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

#if !defined(THREAD_H_INCLUDED)
#define THREAD_H_INCLUDED

#include <cstring>

#include "lock.h"
#include "material.h"
#include "movepick.h"
#include "pawns.h"
#include "position.h"

const int MAX_THREADS = 32;
const int MAX_ACTIVE_SPLIT_POINTS = 8;

struct SplitPoint {

  // Const data after splitPoint has been setup
  SplitPoint* parent;
  const Position* pos;
  Depth depth;
  bool pvNode;
  Value beta;
  int ply;
  int master;
  Move threatMove;

  // Const pointers to shared data
  MovePicker* mp;
  SearchStack* ss;

  // Shared data
  Lock lock;
  volatile int64_t nodes;
  volatile Value alpha;
  volatile Value bestValue;
  volatile int moveCount;
  volatile bool betaCutoff;
  volatile int slaves[MAX_THREADS];
};

// ThreadState type is used to represent thread's current state
enum ThreadState
{
  THREAD_INITIALIZING,  // thread is initializing itself
  THREAD_SEARCHING,     // thread is performing work
  THREAD_AVAILABLE,     // thread is waiting for work
  THREAD_BOOKED,        // other thread (master) has booked us as a slave
  THREAD_WORKISWAITING, // master has ordered us to start
  THREAD_TERMINATED     // we are quitting and thread is terminated
};


// We use per-thread Pawn and material hash tables so that once we get a
// pointer to an entry its life time is unlimited and we don't have to
// care about someone changing the entry under our feet.

struct Thread {
  MaterialInfoTable materialTable;
  PawnInfoTable pawnTable;
  int maxPly;
  Lock sleepLock;
  WaitCondition sleepCond;
  volatile ThreadState state;
  SplitPoint* volatile splitPoint;
  volatile int activeSplitPoints;
  SplitPoint splitPoints[MAX_ACTIVE_SPLIT_POINTS];

  void wake_up() {
    lock_grab(&sleepLock);
    cond_signal(&sleepCond);
    lock_release(&sleepLock);
  }
};


// ThreadsManager class is used to handle all the threads related stuff like init,
// starting, parking and, the most important, launching a slave thread at a split
// point. All the access to shared thread data is done through this class.

class ThreadsManager {
  /* As long as the single ThreadsManager object is defined as a global we don't
     need to explicitly initialize to zero its data members because variables with
     static storage duration are automatically set to zero before enter main()
  */
public:
  Thread& operator[](int threadID) { return threads[threadID]; }
  void init_threads();
  void exit_threads();
  void init_hash_tables();

  int min_split_depth() const { return minimumSplitDepth; }
  int active_threads() const { return activeThreads; }
  void set_active_threads(int cnt) { activeThreads = cnt; }

  void read_uci_options();
  bool available_thread_exists(int master) const;
  bool thread_is_available(int slave, int master) const;
  bool cutoff_at_splitpoint(int threadID) const;
  void idle_loop(int threadID, SplitPoint* sp);

  template <bool Fake>
  void split(Position& pos, SearchStack* ss, Value* alpha, const Value beta, Value* bestValue,
             Depth depth, Move threatMove, int moveCount, MovePicker* mp, bool pvNode);
private:
  Lock mpLock;
  Depth minimumSplitDepth;
  int maxThreadsPerSplitPoint;
  bool useSleepingThreads;
  int activeThreads;
  volatile bool allThreadsShouldExit;
  Thread threads[MAX_THREADS];
};

extern ThreadsManager ThreadsMgr;

#endif // !defined(THREAD_H_INCLUDED)
