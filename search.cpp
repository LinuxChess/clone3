/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2009 Marco Costalba

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

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "book.h"
#include "evaluate.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "lock.h"
#include "san.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "ucioption.h"

using std::cout;
using std::endl;

////
//// Local definitions
////

namespace {

  /// Types

  // The BetaCounterType class is used to order moves at ply one.
  // Apart for the first one that has its score, following moves
  // normally have score -VALUE_INFINITE, so are ordered according
  // to the number of beta cutoffs occurred under their subtree during
  // the last iteration. The counters are per thread variables to avoid
  // concurrent accessing under SMP case.

  struct BetaCounterType {

    BetaCounterType();
    void clear();
    void add(Color us, Depth d, int threadID);
    void read(Color us, int64_t& our, int64_t& their);
  };


  // The RootMove class is used for moves at the root at the tree. For each
  // root move, we store a score, a node count, and a PV (really a refutation
  // in the case of moves which fail low).

  struct RootMove {

    RootMove() { nodes = cumulativeNodes = ourBeta = theirBeta = 0ULL; }

    // RootMove::operator<() is the comparison function used when
    // sorting the moves. A move m1 is considered to be better
    // than a move m2 if it has a higher score, or if the moves
    // have equal score but m1 has the higher node count.
    bool operator<(const RootMove& m) const {

        return score != m.score ? score < m.score : theirBeta <= m.theirBeta;
    }

    Move move;
    Value score;
    int64_t nodes, cumulativeNodes, ourBeta, theirBeta;
    Move pv[PLY_MAX_PLUS_2];
  };


  // The RootMoveList class is essentially an array of RootMove objects, with
  // a handful of methods for accessing the data in the individual moves.

  class RootMoveList {

  public:
    RootMoveList(Position& pos, Move searchMoves[]);

    int move_count() const { return count; }
    Move get_move(int moveNum) const { return moves[moveNum].move; }
    Value get_move_score(int moveNum) const { return moves[moveNum].score; }
    void set_move_score(int moveNum, Value score) { moves[moveNum].score = score; }
    Move get_move_pv(int moveNum, int i) const { return moves[moveNum].pv[i]; }
    int64_t get_move_cumulative_nodes(int moveNum) const { return moves[moveNum].cumulativeNodes; }

    void set_move_nodes(int moveNum, int64_t nodes);
    void set_beta_counters(int moveNum, int64_t our, int64_t their);
    void set_move_pv(int moveNum, const Move pv[]);
    void sort();
    void sort_multipv(int n);

  private:
    static const int MaxRootMoves = 500;
    RootMove moves[MaxRootMoves];
    int count;
  };


  /// Constants

  // Search depth at iteration 1
  const Depth InitialDepth = OnePly;

  // Depth limit for selective search
  const Depth SelectiveDepth = 7 * OnePly;

  // Use internal iterative deepening?
  const bool UseIIDAtPVNodes = true;
  const bool UseIIDAtNonPVNodes = true;

  // Internal iterative deepening margin. At Non-PV moves, when
  // UseIIDAtNonPVNodes is true, we do an internal iterative deepening
  // search when the static evaluation is at most IIDMargin below beta.
  const Value IIDMargin = Value(0x100);

  // Easy move margin. An easy move candidate must be at least this much
  // better than the second best move.
  const Value EasyMoveMargin = Value(0x200);

  // Problem margin. If the score of the first move at iteration N+1 has
  // dropped by more than this since iteration N, the boolean variable
  // "Problem" is set to true, which will make the program spend some extra
  // time looking for a better move.
  const Value ProblemMargin = Value(0x28);

  // No problem margin. If the boolean "Problem" is true, and a new move
  // is found at the root which is less than NoProblemMargin worse than the
  // best move from the previous iteration, Problem is set back to false.
  const Value NoProblemMargin = Value(0x14);

  // Null move margin. A null move search will not be done if the static
  // evaluation of the position is more than NullMoveMargin below beta.
  const Value NullMoveMargin = Value(0x200);

  // If the TT move is at least SingleReplyMargin better then the
  // remaining ones we will extend it.
  const Value SingleReplyMargin = Value(0x20);

  // Margins for futility pruning in the quiescence search, and at frontier
  // and near frontier nodes.
  const Value FutilityMarginQS = Value(0x80);

  Value FutilityMargins[2 * PLY_MAX_PLUS_2]; // Initialized at startup.

  // Each move futility margin is decreased
  const Value IncrementalFutilityMargin = Value(0x8);

  // Depth limit for razoring
  const Depth RazorDepth = 4 * OnePly;

  /// Variables initialized by UCI options

  // Depth limit for use of dynamic threat detection
  Depth ThreatDepth;

  // Last seconds noise filtering (LSN)
  const bool UseLSNFiltering = true;
  const int LSNTime = 4000; // In milliseconds
  const Value LSNValue = value_from_centipawns(200);
  bool loseOnTime = false;

  // Extensions. Array index 0 is used at non-PV nodes, index 1 at PV nodes.
  Depth CheckExtension[2], SingleEvasionExtension[2], PawnPushTo7thExtension[2];
  Depth PassedPawnExtension[2], PawnEndgameExtension[2], MateThreatExtension[2];

  // Iteration counters
  int Iteration;
  BetaCounterType BetaCounter;

  // Scores and number of times the best move changed for each iteration
  Value ValueByIteration[PLY_MAX_PLUS_2];
  int BestMoveChangesByIteration[PLY_MAX_PLUS_2];

  // Search window management
  int AspirationDelta;

  // MultiPV mode
  int MultiPV;

  // Time managment variables
  int RootMoveNumber;
  int SearchStartTime;
  int MaxNodes, MaxDepth;
  int MaxSearchTime, AbsoluteMaxSearchTime, ExtraSearchTime, ExactMaxTime;
  bool UseTimeManagement, InfiniteSearch, PonderSearch, StopOnPonderhit;
  bool AbortSearch, Quit;
  bool FailLow, Problem;

  // Show current line?
  bool ShowCurrentLine;

  // Log file
  bool UseLogFile;
  std::ofstream LogFile;

  // Reduction lookup tables and their getter functions
  // Initialized at startup
  int8_t    PVReductionMatrix[64][64]; // [depth][moveNumber]
  int8_t NonPVReductionMatrix[64][64]; // [depth][moveNumber]

  inline Depth    pv_reduction(Depth d, int mn) { return (Depth)    PVReductionMatrix[Min(d / 2, 63)][Min(mn, 63)]; }
  inline Depth nonpv_reduction(Depth d, int mn) { return (Depth) NonPVReductionMatrix[Min(d / 2, 63)][Min(mn, 63)]; }

  // MP related variables
  int ActiveThreads = 1;
  Depth MinimumSplitDepth;
  int MaxThreadsPerSplitPoint;
  Thread Threads[THREAD_MAX];
  Lock MPLock;
  Lock IOLock;
  bool AllThreadsShouldExit = false;
  SplitPoint SplitPointStack[THREAD_MAX][ACTIVE_SPLIT_POINTS_MAX];
  bool Idle = true;

#if !defined(_MSC_VER)
  pthread_cond_t WaitCond;
  pthread_mutex_t WaitLock;
#else
  HANDLE SitIdleEvent[THREAD_MAX];
#endif

  // Node counters, used only by thread[0] but try to keep in different
  // cache lines (64 bytes each) from the heavy SMP read accessed variables.
  int NodesSincePoll;
  int NodesBetweenPolls = 30000;

  // History table
  History H;

  /// Functions

  Value id_loop(const Position& pos, Move searchMoves[]);
  Value root_search(Position& pos, SearchStack ss[], RootMoveList& rml, Value& oldAlpha, Value& beta);
  Value search_pv(Position& pos, SearchStack ss[], Value alpha, Value beta, Depth depth, int ply, int threadID);
  Value search(Position& pos, SearchStack ss[], Value beta, Depth depth, int ply, bool allowNullmove, int threadID, Move excludedMove = MOVE_NONE);
  Value qsearch(Position& pos, SearchStack ss[], Value alpha, Value beta, Depth depth, int ply, int threadID);
  void sp_search(SplitPoint* sp, int threadID);
  void sp_search_pv(SplitPoint* sp, int threadID);
  void init_node(SearchStack ss[], int ply, int threadID);
  void update_pv(SearchStack ss[], int ply);
  void sp_update_pv(SearchStack* pss, SearchStack ss[], int ply);
  bool connected_moves(const Position& pos, Move m1, Move m2);
  bool value_is_mate(Value value);
  bool move_is_killer(Move m, const SearchStack& ss);
  Depth extension(const Position&, Move, bool, bool, bool, bool, bool, bool*);
  bool ok_to_do_nullmove(const Position& pos);
  bool ok_to_prune(const Position& pos, Move m, Move threat);
  bool ok_to_use_TT(const TTEntry* tte, Depth depth, Value beta, int ply);
  Value refine_eval(const TTEntry* tte, Value defaultEval, int ply);
  void update_history(const Position& pos, Move move, Depth depth, Move movesSearched[], int moveCount);
  void update_killers(Move m, SearchStack& ss);
  void update_gains(const Position& pos, Move move, Value before, Value after);

  int current_search_time();
  int nps();
  void poll();
  void ponderhit();
  void print_current_line(SearchStack ss[], int ply, int threadID);
  void wait_for_stop_or_ponderhit();
  void init_ss_array(SearchStack ss[]);

  void idle_loop(int threadID, SplitPoint* waitSp);
  void init_split_point_stack();
  void destroy_split_point_stack();
  bool thread_should_stop(int threadID);
  bool thread_is_available(int slave, int master);
  bool idle_thread_exists(int master);
  bool split(const Position& pos, SearchStack* ss, int ply,
             Value *alpha, Value *beta, Value *bestValue,
             const Value futilityValue, Depth depth, int *moves,
             MovePicker *mp, int master, bool pvNode);
  void wake_sleeping_threads();

#if !defined(_MSC_VER)
  void *init_thread(void *threadID);
#else
  DWORD WINAPI init_thread(LPVOID threadID);
#endif

}


////
//// Functions
////


/// perft() is our utility to verify move generation is bug free. All the legal
/// moves up to given depth are generated and counted and the sum returned.

int perft(Position& pos, Depth depth)
{
    Move move;
    int sum = 0;
    MovePicker mp = MovePicker(pos, MOVE_NONE, depth, H);

    // If we are at the last ply we don't need to do and undo
    // the moves, just to count them.
    if (depth <= OnePly) // Replace with '<' to test also qsearch
    {
        while (mp.get_next_move()) sum++;
        return sum;
    }

    // Loop through all legal moves
    CheckInfo ci(pos);
    while ((move = mp.get_next_move()) != MOVE_NONE)
    {
        StateInfo st;
        pos.do_move(move, st, ci, pos.move_is_check(move, ci));
        sum += perft(pos, depth - OnePly);
        pos.undo_move(move);
    }
    return sum;
}


/// think() is the external interface to Stockfish's search, and is called when
/// the program receives the UCI 'go' command. It initializes various
/// search-related global variables, and calls root_search(). It returns false
/// when a quit command is received during the search.

bool think(const Position& pos, bool infinite, bool ponder, int side_to_move,
           int time[], int increment[], int movesToGo, int maxDepth,
           int maxNodes, int maxTime, Move searchMoves[]) {

  // Initialize global search variables
  Idle = StopOnPonderhit = AbortSearch = Quit = false;
  FailLow = Problem = false;
  NodesSincePoll = 0;
  SearchStartTime = get_system_time();
  ExactMaxTime = maxTime;
  MaxDepth = maxDepth;
  MaxNodes = maxNodes;
  InfiniteSearch = infinite;
  PonderSearch = ponder;
  UseTimeManagement = !ExactMaxTime && !MaxDepth && !MaxNodes && !InfiniteSearch;

  // Look for a book move, only during games, not tests
  if (UseTimeManagement && !ponder && get_option_value_bool("OwnBook"))
  {
      Move bookMove;
      if (get_option_value_string("Book File") != OpeningBook.file_name())
          OpeningBook.open(get_option_value_string("Book File"));

      bookMove = OpeningBook.get_move(pos);
      if (bookMove != MOVE_NONE)
      {
          cout << "bestmove " << bookMove << endl;
          return true;
      }
  }

  for (int i = 0; i < THREAD_MAX; i++)
  {
      Threads[i].nodes = 0ULL;
  }

  if (button_was_pressed("New Game"))
      loseOnTime = false; // Reset at the beginning of a new game

  // Read UCI option values
  TT.set_size(get_option_value_int("Hash"));
  if (button_was_pressed("Clear Hash"))
      TT.clear();

  bool PonderingEnabled = get_option_value_bool("Ponder");
  MultiPV = get_option_value_int("MultiPV");

  CheckExtension[1] = Depth(get_option_value_int("Check Extension (PV nodes)"));
  CheckExtension[0] = Depth(get_option_value_int("Check Extension (non-PV nodes)"));

  SingleEvasionExtension[1] = Depth(get_option_value_int("Single Evasion Extension (PV nodes)"));
  SingleEvasionExtension[0] = Depth(get_option_value_int("Single Evasion Extension (non-PV nodes)"));

  PawnPushTo7thExtension[1] = Depth(get_option_value_int("Pawn Push to 7th Extension (PV nodes)"));
  PawnPushTo7thExtension[0] = Depth(get_option_value_int("Pawn Push to 7th Extension (non-PV nodes)"));

  PassedPawnExtension[1] = Depth(get_option_value_int("Passed Pawn Extension (PV nodes)"));
  PassedPawnExtension[0] = Depth(get_option_value_int("Passed Pawn Extension (non-PV nodes)"));

  PawnEndgameExtension[1] = Depth(get_option_value_int("Pawn Endgame Extension (PV nodes)"));
  PawnEndgameExtension[0] = Depth(get_option_value_int("Pawn Endgame Extension (non-PV nodes)"));

  MateThreatExtension[1] = Depth(get_option_value_int("Mate Threat Extension (PV nodes)"));
  MateThreatExtension[0] = Depth(get_option_value_int("Mate Threat Extension (non-PV nodes)"));

  ThreatDepth   = get_option_value_int("Threat Depth") * OnePly;

  Chess960 = get_option_value_bool("UCI_Chess960");
  ShowCurrentLine = get_option_value_bool("UCI_ShowCurrLine");
  UseLogFile = get_option_value_bool("Use Search Log");
  if (UseLogFile)
      LogFile.open(get_option_value_string("Search Log Filename").c_str(), std::ios::out | std::ios::app);

  MinimumSplitDepth = get_option_value_int("Minimum Split Depth") * OnePly;
  MaxThreadsPerSplitPoint = get_option_value_int("Maximum Number of Threads per Split Point");

  read_weights(pos.side_to_move());

  // Set the number of active threads
  int newActiveThreads = get_option_value_int("Threads");
  if (newActiveThreads != ActiveThreads)
  {
      ActiveThreads = newActiveThreads;
      init_eval(ActiveThreads);
      // HACK: init_eval() destroys the static castleRightsMask[] array in the
      // Position class. The below line repairs the damage.
      Position p(pos.to_fen());
      assert(pos.is_ok());
  }

  // Wake up sleeping threads
  wake_sleeping_threads();

  for (int i = 1; i < ActiveThreads; i++)
      assert(thread_is_available(i, 0));

  // Set thinking time
  int myTime = time[side_to_move];
  int myIncrement = increment[side_to_move];
  if (UseTimeManagement)
  {
      if (!movesToGo) // Sudden death time control
      {
          if (myIncrement)
          {
              MaxSearchTime = myTime / 30 + myIncrement;
              AbsoluteMaxSearchTime = Max(myTime / 4, myIncrement - 100);
          }
          else // Blitz game without increment
          {
              MaxSearchTime = myTime / 30;
              AbsoluteMaxSearchTime = myTime / 8;
          }
      }
      else // (x moves) / (y minutes)
      {
          if (movesToGo == 1)
          {
              MaxSearchTime = myTime / 2;
              AbsoluteMaxSearchTime = (myTime > 3000)? (myTime - 500) : ((myTime * 3) / 4);
          }
          else
          {
              MaxSearchTime = myTime / Min(movesToGo, 20);
              AbsoluteMaxSearchTime = Min((4 * myTime) / movesToGo, myTime / 3);
          }
      }

      if (PonderingEnabled)
      {
          MaxSearchTime += MaxSearchTime / 4;
          MaxSearchTime = Min(MaxSearchTime, AbsoluteMaxSearchTime);
      }
  }

  // Set best NodesBetweenPolls interval
  if (MaxNodes)
      NodesBetweenPolls = Min(MaxNodes, 30000);
  else if (myTime && myTime < 1000)
      NodesBetweenPolls = 1000;
  else if (myTime && myTime < 5000)
      NodesBetweenPolls = 5000;
  else
      NodesBetweenPolls = 30000;

  // Write information to search log file
  if (UseLogFile)
      LogFile << "Searching: " << pos.to_fen() << endl
              << "infinite: "  << infinite
              << " ponder: "   << ponder
              << " time: "     << myTime
              << " increment: " << myIncrement
              << " moves to go: " << movesToGo << endl;

  // LSN filtering. Used only for developing purpose. Disabled by default.
  if (   UseLSNFiltering
      && loseOnTime)
  {
      // Step 2. If after last move we decided to lose on time, do it now!
       while (SearchStartTime + myTime + 1000 > get_system_time())
           /* wait here */;
  }

  // We're ready to start thinking. Call the iterative deepening loop function
  Value v = id_loop(pos, searchMoves);

  if (UseLSNFiltering)
  {
      // Step 1. If this is sudden death game and our position is hopeless,
      // decide to lose on time.
      if (   !loseOnTime // If we already lost on time, go to step 3.
          && myTime < LSNTime
          && myIncrement == 0
          && movesToGo == 0
          && v < -LSNValue)
      {
          loseOnTime = true;
      }
      else if (loseOnTime)
      {
          // Step 3. Now after stepping over the time limit, reset flag for next match.
          loseOnTime = false;
      }
  }

  if (UseLogFile)
      LogFile.close();

  Idle = true;
  return !Quit;
}


/// init_threads() is called during startup. It launches all helper threads,
/// and initializes the split point stack and the global locks and condition
/// objects.

void init_threads() {

  volatile int i;
  bool ok;

#if !defined(_MSC_VER)
  pthread_t pthread[1];
#endif

  // Init our reduction lookup tables
  for (i = 1; i < 64; i++) // i == depth
      for (int j = 1; j < 64; j++) // j == moveNumber
      {
          double    pvRed = 0.5 + log(double(i)) * log(double(j)) / 6.0;
          double nonPVRed = 0.5 + log(double(i)) * log(double(j)) / 3.0;
          PVReductionMatrix[i][j]    = (int8_t) (   pvRed >= 1.0 ? floor(   pvRed * int(OnePly)) : 0);
          NonPVReductionMatrix[i][j] = (int8_t) (nonPVRed >= 1.0 ? floor(nonPVRed * int(OnePly)) : 0);
      }

  // Init futility margins array
  FutilityMargins[0] = FutilityMargins[1] = Value(0);

  for (i = 2; i < 2 * PLY_MAX_PLUS_2; i++)
  {
      FutilityMargins[i] = Value(112 * bitScanReverse32(i * i / 2)); // FIXME: test using log instead of BSR
  }

  for (i = 0; i < THREAD_MAX; i++)
      Threads[i].activeSplitPoints = 0;

  // Initialize global locks
  lock_init(&MPLock, NULL);
  lock_init(&IOLock, NULL);

  init_split_point_stack();

#if !defined(_MSC_VER)
  pthread_mutex_init(&WaitLock, NULL);
  pthread_cond_init(&WaitCond, NULL);
#else
  for (i = 0; i < THREAD_MAX; i++)
      SitIdleEvent[i] = CreateEvent(0, FALSE, FALSE, 0);
#endif

  // All threads except the main thread should be initialized to idle state
  for (i = 1; i < THREAD_MAX; i++)
  {
      Threads[i].stop = false;
      Threads[i].workIsWaiting = false;
      Threads[i].idle = true;
      Threads[i].running = false;
  }

  // Launch the helper threads
  for (i = 1; i < THREAD_MAX; i++)
  {
#if !defined(_MSC_VER)
      ok = (pthread_create(pthread, NULL, init_thread, (void*)(&i)) == 0);
#else
      DWORD iID[1];
      ok = (CreateThread(NULL, 0, init_thread, (LPVOID)(&i), 0, iID) != NULL);
#endif

      if (!ok)
      {
          cout << "Failed to create thread number " << i << endl;
          Application::exit_with_failure();
      }

      // Wait until the thread has finished launching
      while (!Threads[i].running);
  }
}


/// stop_threads() is called when the program exits. It makes all the
/// helper threads exit cleanly.

void stop_threads() {

  ActiveThreads = THREAD_MAX;  // HACK
  Idle = false;  // HACK
  wake_sleeping_threads();
  AllThreadsShouldExit = true;
  for (int i = 1; i < THREAD_MAX; i++)
  {
      Threads[i].stop = true;
      while (Threads[i].running);
  }
  destroy_split_point_stack();
}


/// nodes_searched() returns the total number of nodes searched so far in
/// the current search.

int64_t nodes_searched() {

  int64_t result = 0ULL;
  for (int i = 0; i < ActiveThreads; i++)
      result += Threads[i].nodes;
  return result;
}


// SearchStack::init() initializes a search stack. Used at the beginning of a
// new search from the root.
void SearchStack::init(int ply) {

  pv[ply] = pv[ply + 1] = MOVE_NONE;
  currentMove = threatMove = MOVE_NONE;
  reduction = Depth(0);
  eval = VALUE_NONE;
  evalInfo = NULL;
}

void SearchStack::initKillers() {

  mateKiller = MOVE_NONE;
  for (int i = 0; i < KILLER_MAX; i++)
      killers[i] = MOVE_NONE;
}

namespace {

  // id_loop() is the main iterative deepening loop. It calls root_search
  // repeatedly with increasing depth until the allocated thinking time has
  // been consumed, the user stops the search, or the maximum search depth is
  // reached.

  Value id_loop(const Position& pos, Move searchMoves[]) {

    Position p(pos);
    SearchStack ss[PLY_MAX_PLUS_2];

    // searchMoves are verified, copied, scored and sorted
    RootMoveList rml(p, searchMoves);

    // Handle special case of searching on a mate/stale position
    if (rml.move_count() == 0)
    {
        if (PonderSearch)
            wait_for_stop_or_ponderhit();

        return pos.is_check()? -VALUE_MATE : VALUE_DRAW;
    }

    // Print RootMoveList c'tor startup scoring to the standard output,
    // so that we print information also for iteration 1.
    cout << "info depth " << 1 << "\ninfo depth " << 1
         << " score " << value_to_string(rml.get_move_score(0))
         << " time " << current_search_time()
         << " nodes " << nodes_searched()
         << " nps " << nps()
         << " pv " << rml.get_move(0) << "\n";

    // Initialize
    TT.new_search();
    H.clear();
    init_ss_array(ss);
    ValueByIteration[1] = rml.get_move_score(0);
    Iteration = 1;

    // Is one move significantly better than others after initial scoring ?
    Move EasyMove = MOVE_NONE;
    if (   rml.move_count() == 1
        || rml.get_move_score(0) > rml.get_move_score(1) + EasyMoveMargin)
        EasyMove = rml.get_move(0);

    // Iterative deepening loop
    while (Iteration < PLY_MAX)
    {
        // Initialize iteration
        rml.sort();
        Iteration++;
        BestMoveChangesByIteration[Iteration] = 0;
        if (Iteration <= 5)
            ExtraSearchTime = 0;

        cout << "info depth " << Iteration << endl;

        // Calculate dynamic search window based on previous iterations
        Value alpha, beta;

        if (MultiPV == 1 && Iteration >= 6 && abs(ValueByIteration[Iteration - 1]) < VALUE_KNOWN_WIN)
        {
            int prevDelta1 = ValueByIteration[Iteration - 1] - ValueByIteration[Iteration - 2];
            int prevDelta2 = ValueByIteration[Iteration - 2] - ValueByIteration[Iteration - 3];

            AspirationDelta = Max(abs(prevDelta1) + abs(prevDelta2) / 2, 16);
            AspirationDelta = (AspirationDelta + 7) / 8 * 8; // Round to match grainSize

            alpha = Max(ValueByIteration[Iteration - 1] - AspirationDelta, -VALUE_INFINITE);
            beta  = Min(ValueByIteration[Iteration - 1] + AspirationDelta,  VALUE_INFINITE);
        }
        else
        {
            alpha = - VALUE_INFINITE;
            beta  =   VALUE_INFINITE;
        }

        // Search to the current depth
        Value value = root_search(p, ss, rml, alpha, beta);

        // Write PV to transposition table, in case the relevant entries have
        // been overwritten during the search.
        TT.insert_pv(p, ss[0].pv);

        if (AbortSearch)
            break; // Value cannot be trusted. Break out immediately!

        //Save info about search result
        ValueByIteration[Iteration] = value;

        // Drop the easy move if it differs from the new best move
        if (ss[0].pv[0] != EasyMove)
            EasyMove = MOVE_NONE;

        Problem = false;

        if (UseTimeManagement)
        {
            // Time to stop?
            bool stopSearch = false;

            // Stop search early if there is only a single legal move,
            // we search up to Iteration 6 anyway to get a proper score.
            if (Iteration >= 6 && rml.move_count() == 1)
                stopSearch = true;

            // Stop search early when the last two iterations returned a mate score
            if (  Iteration >= 6
                && abs(ValueByIteration[Iteration]) >= abs(VALUE_MATE) - 100
                && abs(ValueByIteration[Iteration-1]) >= abs(VALUE_MATE) - 100)
                stopSearch = true;

            // Stop search early if one move seems to be much better than the rest
            int64_t nodes = nodes_searched();
            if (   Iteration >= 8
                && EasyMove == ss[0].pv[0]
                && (  (   rml.get_move_cumulative_nodes(0) > (nodes * 85) / 100
                       && current_search_time() > MaxSearchTime / 16)
                    ||(   rml.get_move_cumulative_nodes(0) > (nodes * 98) / 100
                       && current_search_time() > MaxSearchTime / 32)))
                stopSearch = true;

            // Add some extra time if the best move has changed during the last two iterations
            if (Iteration > 5 && Iteration <= 50)
                ExtraSearchTime = BestMoveChangesByIteration[Iteration]   * (MaxSearchTime / 2)
                                + BestMoveChangesByIteration[Iteration-1] * (MaxSearchTime / 3);

            // Stop search if most of MaxSearchTime is consumed at the end of the
            // iteration. We probably don't have enough time to search the first
            // move at the next iteration anyway.
            if (current_search_time() > ((MaxSearchTime + ExtraSearchTime) * 80) / 128)
                stopSearch = true;

            if (stopSearch)
            {
                if (!PonderSearch)
                    break;
                else
                    StopOnPonderhit = true;
            }
        }

        if (MaxDepth && Iteration >= MaxDepth)
            break;
    }

    rml.sort();

    // If we are pondering or in infinite search, we shouldn't print the
    // best move before we are told to do so.
    if (!AbortSearch && (PonderSearch || InfiniteSearch))
        wait_for_stop_or_ponderhit();
    else
        // Print final search statistics
        cout << "info nodes " << nodes_searched()
             << " nps " << nps()
             << " time " << current_search_time()
             << " hashfull " << TT.full() << endl;

    // Print the best move and the ponder move to the standard output
    if (ss[0].pv[0] == MOVE_NONE)
    {
        ss[0].pv[0] = rml.get_move(0);
        ss[0].pv[1] = MOVE_NONE;
    }
    cout << "bestmove " << ss[0].pv[0];
    if (ss[0].pv[1] != MOVE_NONE)
        cout << " ponder " << ss[0].pv[1];

    cout << endl;

    if (UseLogFile)
    {
        if (dbg_show_mean)
            dbg_print_mean(LogFile);

        if (dbg_show_hit_rate)
            dbg_print_hit_rate(LogFile);

        LogFile << "\nNodes: " << nodes_searched()
                << "\nNodes/second: " << nps()
                << "\nBest move: " << move_to_san(p, ss[0].pv[0]);

        StateInfo st;
        p.do_move(ss[0].pv[0], st);
        LogFile << "\nPonder move: " << move_to_san(p, ss[0].pv[1]) << endl;
    }
    return rml.get_move_score(0);
  }


  // root_search() is the function which searches the root node. It is
  // similar to search_pv except that it uses a different move ordering
  // scheme and prints some information to the standard output.

  Value root_search(Position& pos, SearchStack ss[], RootMoveList& rml, Value& oldAlpha, Value& beta) {

    int64_t nodes;
    Move move;
    StateInfo st;
    Depth depth, ext, newDepth;
    Value value;
    CheckInfo ci(pos);
    int researchCount = 0;
    bool moveIsCheck, captureOrPromotion, dangerous;
    Value alpha = oldAlpha;
    bool isCheck = pos.is_check();

    // Evaluate the position statically
    EvalInfo ei;
    ss[0].eval = !isCheck ? evaluate(pos, ei, 0) : VALUE_NONE;

    while (1) // Fail low loop
    {

        // Loop through all the moves in the root move list
        for (int i = 0; i <  rml.move_count() && !AbortSearch; i++)
        {
            if (alpha >= beta)
            {
                // We failed high, invalidate and skip next moves, leave node-counters
                // and beta-counters as they are and quickly return, we will try to do
                // a research at the next iteration with a bigger aspiration window.
                rml.set_move_score(i, -VALUE_INFINITE);
                continue;
            }

            RootMoveNumber = i + 1;

            // Save the current node count before the move is searched
            nodes = nodes_searched();

            // Reset beta cut-off counters
            BetaCounter.clear();

            // Pick the next root move, and print the move and the move number to
            // the standard output.
            move = ss[0].currentMove = rml.get_move(i);

            if (current_search_time() >= 1000)
                cout << "info currmove " << move
                     << " currmovenumber " << RootMoveNumber << endl;

            // Decide search depth for this move
            moveIsCheck = pos.move_is_check(move);
            captureOrPromotion = pos.move_is_capture_or_promotion(move);
            depth = (Iteration - 2) * OnePly + InitialDepth;
            ext = extension(pos, move, true, captureOrPromotion, moveIsCheck, false, false, &dangerous);
            newDepth = depth + ext;

            value = - VALUE_INFINITE;

            while (1) // Fail high loop
            {

                // Make the move, and search it
                pos.do_move(move, st, ci, moveIsCheck);

                if (i < MultiPV || value > alpha)
                {
                    // Aspiration window is disabled in multi-pv case
                    if (MultiPV > 1)
                        alpha = -VALUE_INFINITE;

                    value = -search_pv(pos, ss, -beta, -alpha, newDepth, 1, 0);

                    // If the value has dropped a lot compared to the last iteration,
                    // set the boolean variable Problem to true. This variable is used
                    // for time managment: When Problem is true, we try to complete the
                    // current iteration before playing a move.
                    Problem = (   Iteration >= 2
                               && value <= ValueByIteration[Iteration - 1] - ProblemMargin);

                    if (Problem && StopOnPonderhit)
                        StopOnPonderhit = false;
                }
                else
                {
                    // Try to reduce non-pv search depth by one ply if move seems not problematic,
                    // if the move fails high will be re-searched at full depth.
                    bool doFullDepthSearch = true;

                    if (   depth >= 3*OnePly // FIXME was newDepth
                        && !dangerous
                        && !captureOrPromotion
                        && !move_is_castle(move))
                    {
                        ss[0].reduction = pv_reduction(depth, RootMoveNumber - MultiPV + 1);
                        if (ss[0].reduction)
                        {
                            value = -search(pos, ss, -alpha, newDepth-ss[0].reduction, 1, true, 0);
                            doFullDepthSearch = (value > alpha);
                        }
                    }

                    if (doFullDepthSearch)
                    {
                        ss[0].reduction = Depth(0);
                        value = -search(pos, ss, -alpha, newDepth, 1, true, 0);

                        if (value > alpha)
                            value = -search_pv(pos, ss, -beta, -alpha, newDepth, 1, 0);
                    }
                }

                pos.undo_move(move);

                // Can we exit fail high loop ?
                if (AbortSearch || value < beta)
                    break;

                // We are failing high and going to do a research. It's important to update score
                // before research in case we run out of time while researching.
                rml.set_move_score(i, value);
                update_pv(ss, 0);
                TT.extract_pv(pos, ss[0].pv, PLY_MAX);
                rml.set_move_pv(i, ss[0].pv);

                // Print search information to the standard output
                cout << "info depth " << Iteration
                     << " score " << value_to_string(value)
                     << ((value >= beta) ? " lowerbound" :
                        ((value <= alpha)? " upperbound" : ""))
                     << " time "  << current_search_time()
                     << " nodes " << nodes_searched()
                     << " nps "   << nps()
                     << " pv ";

                for (int j = 0; ss[0].pv[j] != MOVE_NONE && j < PLY_MAX; j++)
                    cout << ss[0].pv[j] << " ";

                cout << endl;

                if (UseLogFile)
                {
                    ValueType type =  (value >= beta  ? VALUE_TYPE_LOWER
                                    : (value <= alpha ? VALUE_TYPE_UPPER : VALUE_TYPE_EXACT));

                    LogFile << pretty_pv(pos, current_search_time(), Iteration,
                                         nodes_searched(), value, type, ss[0].pv) << endl;
                }

                // Prepare for a research after a fail high, each time with a wider window
                researchCount++;
                beta = Min(beta + AspirationDelta * (1 << researchCount), VALUE_INFINITE);

            } // End of fail high loop

            // Finished searching the move. If AbortSearch is true, the search
            // was aborted because the user interrupted the search or because we
            // ran out of time. In this case, the return value of the search cannot
            // be trusted, and we break out of the loop without updating the best
            // move and/or PV.
            if (AbortSearch)
                break;

            // Remember beta-cutoff and searched nodes counts for this move. The
            // info is used to sort the root moves at the next iteration.
            int64_t our, their;
            BetaCounter.read(pos.side_to_move(), our, their);
            rml.set_beta_counters(i, our, their);
            rml.set_move_nodes(i, nodes_searched() - nodes);

            assert(value >= -VALUE_INFINITE && value <= VALUE_INFINITE);

            if (value <= alpha && i >= MultiPV)
                rml.set_move_score(i, -VALUE_INFINITE);
            else
            {
                // PV move or new best move!

                // Update PV
                rml.set_move_score(i, value);
                update_pv(ss, 0);
                TT.extract_pv(pos, ss[0].pv, PLY_MAX);
                rml.set_move_pv(i, ss[0].pv);

                if (MultiPV == 1)
                {
                    // We record how often the best move has been changed in each
                    // iteration. This information is used for time managment: When
                    // the best move changes frequently, we allocate some more time.
                    if (i > 0)
                        BestMoveChangesByIteration[Iteration]++;

                    // Print search information to the standard output
                    cout << "info depth " << Iteration
                         << " score " << value_to_string(value)
                         << ((value >= beta) ? " lowerbound" :
                            ((value <= alpha)? " upperbound" : ""))
                         << " time "  << current_search_time()
                         << " nodes " << nodes_searched()
                         << " nps "   << nps()
                         << " pv ";

                    for (int j = 0; ss[0].pv[j] != MOVE_NONE && j < PLY_MAX; j++)
                        cout << ss[0].pv[j] << " ";

                    cout << endl;

                    if (UseLogFile)
                    {
                        ValueType type =  (value >= beta  ? VALUE_TYPE_LOWER
                                        : (value <= alpha ? VALUE_TYPE_UPPER : VALUE_TYPE_EXACT));

                        LogFile << pretty_pv(pos, current_search_time(), Iteration,
                                             nodes_searched(), value, type, ss[0].pv) << endl;
                    }
                    if (value > alpha)
                        alpha = value;

                    // Reset the global variable Problem to false if the value isn't too
                    // far below the final value from the last iteration.
                    if (value > ValueByIteration[Iteration - 1] - NoProblemMargin)
                        Problem = false;
                }
                else // MultiPV > 1
                {
                    rml.sort_multipv(i);
                    for (int j = 0; j < Min(MultiPV, rml.move_count()); j++)
                    {
                        cout << "info multipv " << j + 1
                             << " score " << value_to_string(rml.get_move_score(j))
                             << " depth " << ((j <= i)? Iteration : Iteration - 1)
                             << " time " << current_search_time()
                             << " nodes " << nodes_searched()
                             << " nps " << nps()
                             << " pv ";

                        for (int k = 0; rml.get_move_pv(j, k) != MOVE_NONE && k < PLY_MAX; k++)
                            cout << rml.get_move_pv(j, k) << " ";

                        cout << endl;
                    }
                    alpha = rml.get_move_score(Min(i, MultiPV-1));
                }
            } // PV move or new best move

            assert(alpha >= oldAlpha);

            FailLow = (alpha == oldAlpha);
        }

        // Can we exit fail low loop ?
        if (AbortSearch || alpha > oldAlpha)
            break;

        // Prepare for a research after a fail low, each time with a wider window
        researchCount++;
        alpha = Max(alpha - AspirationDelta * (1 << researchCount), -VALUE_INFINITE);
        oldAlpha = alpha;

    } // Fail low loop

    return alpha;
  }


  // search_pv() is the main search function for PV nodes.

  Value search_pv(Position& pos, SearchStack ss[], Value alpha, Value beta,
                  Depth depth, int ply, int threadID) {

    assert(alpha >= -VALUE_INFINITE && alpha <= VALUE_INFINITE);
    assert(beta > alpha && beta <= VALUE_INFINITE);
    assert(ply >= 0 && ply < PLY_MAX);
    assert(threadID >= 0 && threadID < ActiveThreads);

    Move movesSearched[256];
    StateInfo st;
    const TTEntry* tte;
    Move ttMove, move;
    Depth ext, newDepth;
    Value oldAlpha, value;
    bool isCheck, mateThreat, singleEvasion, moveIsCheck, captureOrPromotion, dangerous;
    int moveCount = 0;
    Value bestValue = value = -VALUE_INFINITE;

    if (depth < OnePly)
        return qsearch(pos, ss, alpha, beta, Depth(0), ply, threadID);

    // Initialize, and make an early exit in case of an aborted search,
    // an instant draw, maximum ply reached, etc.
    init_node(ss, ply, threadID);

    // After init_node() that calls poll()
    if (AbortSearch || thread_should_stop(threadID))
        return Value(0);

    if (pos.is_draw() || ply >= PLY_MAX - 1)
        return VALUE_DRAW;

    // Mate distance pruning
    oldAlpha = alpha;
    alpha = Max(value_mated_in(ply), alpha);
    beta = Min(value_mate_in(ply+1), beta);
    if (alpha >= beta)
        return alpha;

    // Transposition table lookup. At PV nodes, we don't use the TT for
    // pruning, but only for move ordering. This is to avoid problems in
    // the following areas:
    //
    // * Repetition draw detection
    // * Fifty move rule detection
    // * Searching for a mate
    // * Printing of full PV line
    //
    tte = TT.retrieve(pos.get_key());
    ttMove = (tte ? tte->move() : MOVE_NONE);

    // Go with internal iterative deepening if we don't have a TT move
    if (   UseIIDAtPVNodes
        && depth >= 5*OnePly
        && ttMove == MOVE_NONE)
    {
        search_pv(pos, ss, alpha, beta, depth-2*OnePly, ply, threadID);
        ttMove = ss[ply].pv[ply];
        tte = TT.retrieve(pos.get_key());
    }

    isCheck = pos.is_check();
    if (!isCheck)
    {
        // Update gain statistics of the previous move that lead
        // us in this position.
        EvalInfo ei;
        ss[ply].eval = evaluate(pos, ei, threadID);
        update_gains(pos, ss[ply - 1].currentMove, ss[ply - 1].eval, ss[ply].eval);
    }

    // Initialize a MovePicker object for the current position, and prepare
    // to search all moves
    mateThreat = pos.has_mate_threat(opposite_color(pos.side_to_move()));
    CheckInfo ci(pos);
    MovePicker mp = MovePicker(pos, ttMove, depth, H, &ss[ply]);

    // Loop through all legal moves until no moves remain or a beta cutoff
    // occurs.
    while (   alpha < beta
           && (move = mp.get_next_move()) != MOVE_NONE
           && !thread_should_stop(threadID))
    {
      assert(move_is_ok(move));

      singleEvasion = (isCheck && mp.number_of_evasions() == 1);
      moveIsCheck = pos.move_is_check(move, ci);
      captureOrPromotion = pos.move_is_capture_or_promotion(move);

      // Decide the new search depth
      ext = extension(pos, move, true, captureOrPromotion, moveIsCheck, singleEvasion, mateThreat, &dangerous);

      // Singular extension search. We extend the TT move if its value is much better than
      // its siblings. To verify this we do a reduced search on all the other moves but the
      // ttMove, if result is lower then ttValue minus a margin then we extend ttMove.
      if (   depth >= 6 * OnePly
          && tte
          && move == tte->move()
          && ext < OnePly
          && is_lower_bound(tte->type())
          && tte->depth() >= depth - 3 * OnePly)
      {
          Value ttValue = value_from_tt(tte->value(), ply);

          if (abs(ttValue) < VALUE_KNOWN_WIN)
          {
              Value excValue = search(pos, ss, ttValue - SingleReplyMargin, depth / 2, ply, false, threadID, move);

              if (excValue < ttValue - SingleReplyMargin)
                  ext = OnePly;
          }
      }

      newDepth = depth - OnePly + ext;

      // Update current move
      movesSearched[moveCount++] = ss[ply].currentMove = move;

      // Make and search the move
      pos.do_move(move, st, ci, moveIsCheck);

      if (moveCount == 1) // The first move in list is the PV
          value = -search_pv(pos, ss, -beta, -alpha, newDepth, ply+1, threadID);
      else
      {
        // Try to reduce non-pv search depth by one ply if move seems not problematic,
        // if the move fails high will be re-searched at full depth.
        bool doFullDepthSearch = true;

        if (    depth >= 3*OnePly
            && !dangerous
            && !captureOrPromotion
            && !move_is_castle(move)
            && !move_is_killer(move, ss[ply]))
        {
            ss[ply].reduction = pv_reduction(depth, moveCount);
            if (ss[ply].reduction)
            {
                value = -search(pos, ss, -alpha, newDepth-ss[ply].reduction, ply+1, true, threadID);
                doFullDepthSearch = (value > alpha);
            }
        }

        if (doFullDepthSearch) // Go with full depth non-pv search
        {
            ss[ply].reduction = Depth(0);
            value = -search(pos, ss, -alpha, newDepth, ply+1, true, threadID);
            if (value > alpha && value < beta)
                value = -search_pv(pos, ss, -beta, -alpha, newDepth, ply+1, threadID);
        }
      }
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // New best move?
      if (value > bestValue)
      {
          bestValue = value;
          if (value > alpha)
          {
              alpha = value;
              update_pv(ss, ply);
              if (value == value_mate_in(ply + 1))
                  ss[ply].mateKiller = move;
          }
          // If we are at ply 1, and we are searching the first root move at
          // ply 0, set the 'Problem' variable if the score has dropped a lot
          // (from the computer's point of view) since the previous iteration.
          if (   ply == 1
              && Iteration >= 2
              && -value <= ValueByIteration[Iteration-1] - ProblemMargin)
              Problem = true;
      }

      // Split?
      if (   ActiveThreads > 1
          && bestValue < beta
          && depth >= MinimumSplitDepth
          && Iteration <= 99
          && idle_thread_exists(threadID)
          && !AbortSearch
          && !thread_should_stop(threadID)
          && split(pos, ss, ply, &alpha, &beta, &bestValue, VALUE_NONE,
                   depth, &moveCount, &mp, threadID, true))
          break;
    }

    // All legal moves have been searched.  A special case: If there were
    // no legal moves, it must be mate or stalemate.
    if (moveCount == 0)
        return (isCheck ? value_mated_in(ply) : VALUE_DRAW);

    // If the search is not aborted, update the transposition table,
    // history counters, and killer moves.
    if (AbortSearch || thread_should_stop(threadID))
        return bestValue;

    if (bestValue <= oldAlpha)
        TT.store(pos.get_key(), value_to_tt(bestValue, ply), VALUE_TYPE_UPPER, depth, MOVE_NONE);

    else if (bestValue >= beta)
    {
        BetaCounter.add(pos.side_to_move(), depth, threadID);
        move = ss[ply].pv[ply];
        if (!pos.move_is_capture_or_promotion(move))
        {
            update_history(pos, move, depth, movesSearched, moveCount);
            update_killers(move, ss[ply]);
        }
        TT.store(pos.get_key(), value_to_tt(bestValue, ply), VALUE_TYPE_LOWER, depth, move);
    }
    else
        TT.store(pos.get_key(), value_to_tt(bestValue, ply), VALUE_TYPE_EXACT, depth, ss[ply].pv[ply]);

    return bestValue;
  }


  // search() is the search function for zero-width nodes.

  Value search(Position& pos, SearchStack ss[], Value beta, Depth depth,
               int ply, bool allowNullmove, int threadID, Move excludedMove) {

    assert(beta >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
    assert(ply >= 0 && ply < PLY_MAX);
    assert(threadID >= 0 && threadID < ActiveThreads);

    Move movesSearched[256];
    EvalInfo ei;
    StateInfo st;
    const TTEntry* tte;
    Move ttMove, move;
    Depth ext, newDepth;
    Value bestValue, staticValue, nullValue, value, futilityValue, futilityValueScaled;
    bool isCheck, singleEvasion, moveIsCheck, captureOrPromotion, dangerous;
    bool mateThreat = false;
    int moveCount = 0;
    futilityValue = staticValue = bestValue = value = -VALUE_INFINITE;

    if (depth < OnePly)
        return qsearch(pos, ss, beta-1, beta, Depth(0), ply, threadID);

    // Initialize, and make an early exit in case of an aborted search,
    // an instant draw, maximum ply reached, etc.
    init_node(ss, ply, threadID);

    // After init_node() that calls poll()
    if (AbortSearch || thread_should_stop(threadID))
        return Value(0);

    if (pos.is_draw() || ply >= PLY_MAX - 1)
        return VALUE_DRAW;

    // Mate distance pruning
    if (value_mated_in(ply) >= beta)
        return beta;

    if (value_mate_in(ply + 1) < beta)
        return beta - 1;

    // We don't want the score of a partial search to overwrite a previous full search
    // TT value, so we use a different position key in case of an excluded move exsists.
    Key posKey = excludedMove ? pos.get_exclusion_key() : pos.get_key();

    // Transposition table lookup
    tte = TT.retrieve(posKey);
    ttMove = (tte ? tte->move() : MOVE_NONE);

    if (tte && ok_to_use_TT(tte, depth, beta, ply))
    {
        ss[ply].currentMove = ttMove; // Can be MOVE_NONE
        return value_from_tt(tte->value(), ply);
    }

    isCheck = pos.is_check();

    // Calculate depth dependant futility pruning parameters
    const int FutilityMoveCountMargin = 3 + (1 << (3 * int(depth) / 8));

    // Evaluate the position statically
    if (!isCheck)
    {
        if (tte && (tte->type() & VALUE_TYPE_EVAL))
            staticValue = value_from_tt(tte->value(), ply);
        else
        {
            staticValue = evaluate(pos, ei, threadID);
            ss[ply].evalInfo = &ei;
        }

        ss[ply].eval = staticValue;
        futilityValue = staticValue + FutilityMargins[int(depth)]; //FIXME: Remove me, only for split
        staticValue = refine_eval(tte, staticValue, ply); // Enhance accuracy with TT value if possible
        update_gains(pos, ss[ply - 1].currentMove, ss[ply - 1].eval, ss[ply].eval);
    }

    // Static null move pruning. We're betting that the opponent doesn't have
    // a move that will reduce the score by more than FutilityMargins[int(depth)]
    // if we do a null move.
    if (  !isCheck
        && allowNullmove
        && depth < RazorDepth
        && staticValue - FutilityMargins[int(depth)] >= beta)
        return staticValue - FutilityMargins[int(depth)];

    // Null move search
    if (    allowNullmove
        &&  depth > OnePly
        && !isCheck
        && !value_is_mate(beta)
        &&  ok_to_do_nullmove(pos)
        &&  staticValue >= beta - NullMoveMargin)
    {
        ss[ply].currentMove = MOVE_NULL;

        pos.do_null_move(st);

        // Null move dynamic reduction based on depth
        int R = 3 + (depth >= 5 * OnePly ? depth / 8 : 0);

        // Null move dynamic reduction based on value
        if (staticValue - beta > PawnValueMidgame)
            R++;

        nullValue = -search(pos, ss, -(beta-1), depth-R*OnePly, ply+1, false, threadID);

        pos.undo_null_move();

        if (nullValue >= beta)
        {
            if (depth < 6 * OnePly)
                return beta;

            // Do zugzwang verification search
            Value v = search(pos, ss, beta, depth-5*OnePly, ply, false, threadID);
            if (v >= beta)
                return beta;
        } else {
            // The null move failed low, which means that we may be faced with
            // some kind of threat. If the previous move was reduced, check if
            // the move that refuted the null move was somehow connected to the
            // move which was reduced. If a connection is found, return a fail
            // low score (which will cause the reduced move to fail high in the
            // parent node, which will trigger a re-search with full depth).
            if (nullValue == value_mated_in(ply + 2))
                mateThreat = true;

            ss[ply].threatMove = ss[ply + 1].currentMove;
            if (   depth < ThreatDepth
                && ss[ply - 1].reduction
                && connected_moves(pos, ss[ply - 1].currentMove, ss[ply].threatMove))
                return beta - 1;
        }
    }
    // Null move search not allowed, try razoring
    else if (   !value_is_mate(beta)
             && !isCheck
             && depth < RazorDepth
             && staticValue < beta - (NullMoveMargin + 16 * depth)
             && ss[ply - 1].currentMove != MOVE_NULL
             && ttMove == MOVE_NONE
             && !pos.has_pawn_on_7th(pos.side_to_move()))
    {
        Value rbeta = beta - (NullMoveMargin + 16 * depth);
        Value v = qsearch(pos, ss, rbeta-1, rbeta, Depth(0), ply, threadID);
        if (v < rbeta)
          return v;
    }

    // Go with internal iterative deepening if we don't have a TT move
    if (UseIIDAtNonPVNodes && ttMove == MOVE_NONE && depth >= 8*OnePly &&
        !isCheck && ss[ply].eval >= beta - IIDMargin)
    {
        search(pos, ss, beta, Min(depth/2, depth-2*OnePly), ply, false, threadID);
        ttMove = ss[ply].pv[ply];
        tte = TT.retrieve(pos.get_key());
    }

    // Initialize a MovePicker object for the current position, and prepare
    // to search all moves.
    MovePicker mp = MovePicker(pos, ttMove, depth, H, &ss[ply]);
    CheckInfo ci(pos);

    // Loop through all legal moves until no moves remain or a beta cutoff occurs
    while (   bestValue < beta
           && (move = mp.get_next_move()) != MOVE_NONE
           && !thread_should_stop(threadID))
    {
      assert(move_is_ok(move));

      if (move == excludedMove)
          continue;

      moveIsCheck = pos.move_is_check(move, ci);
      singleEvasion = (isCheck && mp.number_of_evasions() == 1);
      captureOrPromotion = pos.move_is_capture_or_promotion(move);

      // Decide the new search depth
      ext = extension(pos, move, false, captureOrPromotion, moveIsCheck, singleEvasion, mateThreat, &dangerous);

      // Singular extension search. We extend the TT move if its value is much better than
      // its siblings. To verify this we do a reduced search on all the other moves but the
      // ttMove, if result is lower then ttValue minus a margin then we extend ttMove.
      if (   depth >= 8 * OnePly
          && tte
          && move == tte->move()
          && !excludedMove // Do not allow recursive single-reply search
          && ext < OnePly
          && is_lower_bound(tte->type())
          && tte->depth() >= depth - 3 * OnePly)
      {
          Value ttValue = value_from_tt(tte->value(), ply);

          if (abs(ttValue) < VALUE_KNOWN_WIN)
          {
              Value excValue = search(pos, ss, ttValue - SingleReplyMargin, depth / 2, ply, false, threadID, move);

              if (excValue < ttValue - SingleReplyMargin)
                  ext = OnePly;
          }
      }

      newDepth = depth - OnePly + ext;

      // Update current move
      movesSearched[moveCount++] = ss[ply].currentMove = move;

      // Futility pruning
      if (   !isCheck
          && !dangerous
          && !captureOrPromotion
          && !move_is_castle(move)
          &&  move != ttMove)
      {
          // Move count based pruning
          if (   moveCount >= FutilityMoveCountMargin
              && ok_to_prune(pos, move, ss[ply].threatMove)
              && bestValue > value_mated_in(PLY_MAX))
              continue;

          // Value based pruning
          Depth predictedDepth = newDepth;

          //FIXME: We are ignoring condition: depth >= 3*OnePly, BUG??
          ss[ply].reduction = nonpv_reduction(depth, moveCount);
          if (ss[ply].reduction)
              predictedDepth -= ss[ply].reduction;

          if (predictedDepth < SelectiveDepth)
          {
              int preFutilityValueMargin = 0;
              if (predictedDepth >= OnePly)
                  preFutilityValueMargin = FutilityMargins[int(predictedDepth)];

              preFutilityValueMargin += H.gain(pos.piece_on(move_from(move)), move_to(move)) + 45;

              futilityValueScaled = ss[ply].eval + preFutilityValueMargin - moveCount * IncrementalFutilityMargin;

              if (futilityValueScaled < beta)
              {
                  if (futilityValueScaled > bestValue)
                      bestValue = futilityValueScaled;
                  continue;
              }
          }
      }

      // Make and search the move
      pos.do_move(move, st, ci, moveIsCheck);

      // Try to reduce non-pv search depth by one ply if move seems not problematic,
      // if the move fails high will be re-searched at full depth.
      bool doFullDepthSearch = true;

      if (    depth >= 3*OnePly
          && !dangerous
          && !captureOrPromotion
          && !move_is_castle(move)
          && !move_is_killer(move, ss[ply]))
      {
          ss[ply].reduction = nonpv_reduction(depth, moveCount);
          if (ss[ply].reduction)
          {
              value = -search(pos, ss, -(beta-1), newDepth-ss[ply].reduction, ply+1, true, threadID);
              doFullDepthSearch = (value >= beta);
          }
      }

      if (doFullDepthSearch) // Go with full depth non-pv search
      {
          ss[ply].reduction = Depth(0);
          value = -search(pos, ss, -(beta-1), newDepth, ply+1, true, threadID);
      }
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // New best move?
      if (value > bestValue)
      {
          bestValue = value;
          if (value >= beta)
              update_pv(ss, ply);

          if (value == value_mate_in(ply + 1))
              ss[ply].mateKiller = move;
      }

      // Split?
      if (   ActiveThreads > 1
          && bestValue < beta
          && depth >= MinimumSplitDepth
          && Iteration <= 99
          && idle_thread_exists(threadID)
          && !AbortSearch
          && !thread_should_stop(threadID)
          && split(pos, ss, ply, &beta, &beta, &bestValue, futilityValue, //FIXME: SMP & futilityValue
                   depth, &moveCount, &mp, threadID, false))
          break;
    }

    // All legal moves have been searched. A special case: If there were
    // no legal moves, it must be mate or stalemate.
    if (!moveCount)
        return excludedMove ? beta - 1 : (pos.is_check() ? value_mated_in(ply) : VALUE_DRAW);

    // If the search is not aborted, update the transposition table,
    // history counters, and killer moves.
    if (AbortSearch || thread_should_stop(threadID))
        return bestValue;

    if (bestValue < beta)
        TT.store(posKey, value_to_tt(bestValue, ply), VALUE_TYPE_UPPER, depth, MOVE_NONE);
    else
    {
        BetaCounter.add(pos.side_to_move(), depth, threadID);
        move = ss[ply].pv[ply];
        TT.store(posKey, value_to_tt(bestValue, ply), VALUE_TYPE_LOWER, depth, move);
        if (!pos.move_is_capture_or_promotion(move))
        {
            update_history(pos, move, depth, movesSearched, moveCount);
            update_killers(move, ss[ply]);
        }

    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main
  // search function when the remaining depth is zero (or, to be more precise,
  // less than OnePly).

  Value qsearch(Position& pos, SearchStack ss[], Value alpha, Value beta,
                Depth depth, int ply, int threadID) {

    assert(alpha >= -VALUE_INFINITE && alpha <= VALUE_INFINITE);
    assert(beta >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
    assert(depth <= 0);
    assert(ply >= 0 && ply < PLY_MAX);
    assert(threadID >= 0 && threadID < ActiveThreads);

    EvalInfo ei;
    StateInfo st;
    Move ttMove, move;
    Value staticValue, bestValue, value, futilityBase, futilityValue;
    bool isCheck, enoughMaterial, moveIsCheck, evasionPrunable;
    const TTEntry* tte = NULL;
    int moveCount = 0;
    bool pvNode = (beta - alpha != 1);
    Value oldAlpha = alpha;

    // Initialize, and make an early exit in case of an aborted search,
    // an instant draw, maximum ply reached, etc.
    init_node(ss, ply, threadID);

    // After init_node() that calls poll()
    if (AbortSearch || thread_should_stop(threadID))
        return Value(0);

    if (pos.is_draw() || ply >= PLY_MAX - 1)
        return VALUE_DRAW;

    // Transposition table lookup. At PV nodes, we don't use the TT for
    // pruning, but only for move ordering.
    tte = TT.retrieve(pos.get_key());
    ttMove = (tte ? tte->move() : MOVE_NONE);

    if (!pvNode && tte && ok_to_use_TT(tte, depth, beta, ply))
    {
        assert(tte->type() != VALUE_TYPE_EVAL);

        ss[ply].currentMove = ttMove; // Can be MOVE_NONE
        return value_from_tt(tte->value(), ply);
    }

    isCheck = pos.is_check();

    // Evaluate the position statically
    if (isCheck)
        staticValue = -VALUE_INFINITE;
    else if (tte && (tte->type() & VALUE_TYPE_EVAL))
        staticValue = value_from_tt(tte->value(), ply);
    else
        staticValue = evaluate(pos, ei, threadID);

    if (!isCheck)
    {
        ss[ply].eval = staticValue;
        update_gains(pos, ss[ply - 1].currentMove, ss[ply - 1].eval, ss[ply].eval);
    }

    // Initialize "stand pat score", and return it immediately if it is
    // at least beta.
    bestValue = staticValue;

    if (bestValue >= beta)
    {
        // Store the score to avoid a future costly evaluation() call
        if (!isCheck && !tte && ei.futilityMargin[pos.side_to_move()] == 0)
            TT.store(pos.get_key(), value_to_tt(bestValue, ply), VALUE_TYPE_EV_LO, Depth(-127*OnePly), MOVE_NONE);

        return bestValue;
    }

    if (bestValue > alpha)
        alpha = bestValue;

    // If we are near beta then try to get a cutoff pushing checks a bit further
    bool deepChecks = depth == -OnePly && staticValue >= beta - PawnValueMidgame / 8;

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions and checks (only if depth == 0 or depth == -OnePly
    // and we are near beta) will be generated.
    MovePicker mp = MovePicker(pos, ttMove, deepChecks ? Depth(0) : depth, H);
    CheckInfo ci(pos);
    enoughMaterial = pos.non_pawn_material(pos.side_to_move()) > RookValueMidgame;
    futilityBase = staticValue + FutilityMarginQS + ei.futilityMargin[pos.side_to_move()];

    // Loop through the moves until no moves remain or a beta cutoff
    // occurs.
    while (   alpha < beta
           && (move = mp.get_next_move()) != MOVE_NONE)
    {
      assert(move_is_ok(move));

      moveIsCheck = pos.move_is_check(move, ci);

      // Update current move
      moveCount++;
      ss[ply].currentMove = move;

      // Futility pruning
      if (   enoughMaterial
          && !isCheck
          && !pvNode
          && !moveIsCheck
          &&  move != ttMove
          && !move_is_promotion(move)
          && !pos.move_is_passed_pawn_push(move))
      {
          futilityValue =  futilityBase
                         + pos.endgame_value_of_piece_on(move_to(move))
                         + (move_is_ep(move) ? PawnValueEndgame : Value(0));

          if (futilityValue < alpha)
          {
              if (futilityValue > bestValue)
                  bestValue = futilityValue;
              continue;
          }
      }

      // Detect blocking evasions that are candidate to be pruned
      evasionPrunable =   isCheck
                       && bestValue != -VALUE_INFINITE
                       && !pos.move_is_capture(move)
                       && pos.type_of_piece_on(move_from(move)) != KING
                       && !pos.can_castle(pos.side_to_move());

      // Don't search moves with negative SEE values
      if (   (!isCheck || evasionPrunable)
          &&  move != ttMove
          && !move_is_promotion(move)
          &&  pos.see_sign(move) < 0)
          continue;

      // Make and search the move
      pos.do_move(move, st, ci, moveIsCheck);
      value = -qsearch(pos, ss, -beta, -alpha, depth-OnePly, ply+1, threadID);
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // New best move?
      if (value > bestValue)
      {
          bestValue = value;
          if (value > alpha)
          {
              alpha = value;
              update_pv(ss, ply);
          }
       }
    }

    // All legal moves have been searched. A special case: If we're in check
    // and no legal moves were found, it is checkmate.
    if (!moveCount && pos.is_check()) // Mate!
        return value_mated_in(ply);

    // Update transposition table
    Depth d = (depth == Depth(0) ? Depth(0) : Depth(-1));
    if (bestValue <= oldAlpha)
    {
        // If bestValue isn't changed it means it is still the static evaluation
        // of the node, so keep this info to avoid a future evaluation() call.
        ValueType type = (bestValue == staticValue && !ei.futilityMargin[pos.side_to_move()] ? VALUE_TYPE_EV_UP : VALUE_TYPE_UPPER);
        TT.store(pos.get_key(), value_to_tt(bestValue, ply), type, d, MOVE_NONE);
    }
    else if (bestValue >= beta)
    {
        move = ss[ply].pv[ply];
        TT.store(pos.get_key(), value_to_tt(bestValue, ply), VALUE_TYPE_LOWER, d, move);

        // Update killers only for good checking moves
        if (!pos.move_is_capture_or_promotion(move))
            update_killers(move, ss[ply]);
    }
    else
        TT.store(pos.get_key(), value_to_tt(bestValue, ply), VALUE_TYPE_EXACT, d, ss[ply].pv[ply]);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // sp_search() is used to search from a split point.  This function is called
  // by each thread working at the split point.  It is similar to the normal
  // search() function, but simpler.  Because we have already probed the hash
  // table, done a null move search, and searched the first move before
  // splitting, we don't have to repeat all this work in sp_search().  We
  // also don't need to store anything to the hash table here:  This is taken
  // care of after we return from the split point.

  void sp_search(SplitPoint* sp, int threadID) {

    assert(threadID >= 0 && threadID < ActiveThreads);
    assert(ActiveThreads > 1);

    Position pos(*sp->pos);
    CheckInfo ci(pos);
    SearchStack* ss = sp->sstack[threadID];
    Value value = -VALUE_INFINITE;
    Move move;
    int moveCount;
    bool isCheck = pos.is_check();
    bool useFutilityPruning =     sp->depth < SelectiveDepth
                              && !isCheck;

    const int FutilityMoveCountMargin = 3 + (1 << (3 * int(sp->depth) / 8));

    while (    lock_grab_bool(&(sp->lock))
           &&  sp->bestValue < sp->beta
           && !thread_should_stop(threadID)
           && (move = sp->mp->get_next_move()) != MOVE_NONE)
    {
      moveCount = ++sp->moves;
      lock_release(&(sp->lock));

      assert(move_is_ok(move));

      bool moveIsCheck = pos.move_is_check(move, ci);
      bool captureOrPromotion = pos.move_is_capture_or_promotion(move);

      ss[sp->ply].currentMove = move;

      // Decide the new search depth
      bool dangerous;
      Depth ext = extension(pos, move, false, captureOrPromotion, moveIsCheck, false, false, &dangerous);
      Depth newDepth = sp->depth - OnePly + ext;

      // Prune?
      if (    useFutilityPruning
          && !dangerous
          && !captureOrPromotion)
      {
          // Move count based pruning
          if (   moveCount >= FutilityMoveCountMargin
              && ok_to_prune(pos, move, ss[sp->ply].threatMove)
              && sp->bestValue > value_mated_in(PLY_MAX))
              continue;

          // Value based pruning
          Value futilityValueScaled = sp->futilityValue - moveCount * IncrementalFutilityMargin;

          if (futilityValueScaled < sp->beta)
          {
              if (futilityValueScaled > sp->bestValue) // Less then 1% of cases
              {
                  lock_grab(&(sp->lock));
                  if (futilityValueScaled > sp->bestValue)
                      sp->bestValue = futilityValueScaled;
                  lock_release(&(sp->lock));
              }
              continue;
          }
      }

      // Make and search the move.
      StateInfo st;
      pos.do_move(move, st, ci, moveIsCheck);

      // Try to reduce non-pv search depth by one ply if move seems not problematic,
      // if the move fails high will be re-searched at full depth.
      bool doFullDepthSearch = true;

      if (   !dangerous
          && !captureOrPromotion
          && !move_is_castle(move)
          && !move_is_killer(move, ss[sp->ply]))
      {
          ss[sp->ply].reduction = nonpv_reduction(sp->depth, moveCount);
          if (ss[sp->ply].reduction)
          {
              value = -search(pos, ss, -(sp->beta-1), newDepth-ss[sp->ply].reduction, sp->ply+1, true, threadID);
              doFullDepthSearch = (value >= sp->beta);
          }
      }

      if (doFullDepthSearch) // Go with full depth non-pv search
      {
          ss[sp->ply].reduction = Depth(0);
          value = -search(pos, ss, -(sp->beta - 1), newDepth, sp->ply+1, true, threadID);
      }
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      if (thread_should_stop(threadID))
      {
          lock_grab(&(sp->lock));
          break;
      }

      // New best move?
      if (value > sp->bestValue) // Less then 2% of cases
      {
          lock_grab(&(sp->lock));
          if (value > sp->bestValue && !thread_should_stop(threadID))
          {
              sp->bestValue = value;
              if (sp->bestValue >= sp->beta)
              {
                  sp_update_pv(sp->parentSstack, ss, sp->ply);
                  for (int i = 0; i < ActiveThreads; i++)
                      if (i != threadID && (i == sp->master || sp->slaves[i]))
                          Threads[i].stop = true;

                  sp->finished = true;
              }
          }
          lock_release(&(sp->lock));
      }
    }

    /* Here we have the lock still grabbed */

    // If this is the master thread and we have been asked to stop because of
    // a beta cutoff higher up in the tree, stop all slave threads.
    if (sp->master == threadID && thread_should_stop(threadID))
        for (int i = 0; i < ActiveThreads; i++)
            if (sp->slaves[i])
                Threads[i].stop = true;

    sp->cpus--;
    sp->slaves[threadID] = 0;

    lock_release(&(sp->lock));
  }


  // sp_search_pv() is used to search from a PV split point.  This function
  // is called by each thread working at the split point.  It is similar to
  // the normal search_pv() function, but simpler.  Because we have already
  // probed the hash table and searched the first move before splitting, we
  // don't have to repeat all this work in sp_search_pv().  We also don't
  // need to store anything to the hash table here: This is taken care of
  // after we return from the split point.

  void sp_search_pv(SplitPoint* sp, int threadID) {

    assert(threadID >= 0 && threadID < ActiveThreads);
    assert(ActiveThreads > 1);

    Position pos(*sp->pos);
    CheckInfo ci(pos);
    SearchStack* ss = sp->sstack[threadID];
    Value value = -VALUE_INFINITE;
    int moveCount;
    Move move;

    while (    lock_grab_bool(&(sp->lock))
           &&  sp->alpha < sp->beta
           && !thread_should_stop(threadID)
           && (move = sp->mp->get_next_move()) != MOVE_NONE)
    {
      moveCount = ++sp->moves;
      lock_release(&(sp->lock));

      assert(move_is_ok(move));

      bool moveIsCheck = pos.move_is_check(move, ci);
      bool captureOrPromotion = pos.move_is_capture_or_promotion(move);

      ss[sp->ply].currentMove = move;

      // Decide the new search depth
      bool dangerous;
      Depth ext = extension(pos, move, true, captureOrPromotion, moveIsCheck, false, false, &dangerous);
      Depth newDepth = sp->depth - OnePly + ext;

      // Make and search the move.
      StateInfo st;
      pos.do_move(move, st, ci, moveIsCheck);

      // Try to reduce non-pv search depth by one ply if move seems not problematic,
      // if the move fails high will be re-searched at full depth.
      bool doFullDepthSearch = true;

      if (   !dangerous
          && !captureOrPromotion
          && !move_is_castle(move)
          && !move_is_killer(move, ss[sp->ply]))
      {
          ss[sp->ply].reduction = pv_reduction(sp->depth, moveCount);
          if (ss[sp->ply].reduction)
          {
              Value localAlpha = sp->alpha;
              value = -search(pos, ss, -localAlpha, newDepth-ss[sp->ply].reduction, sp->ply+1, true, threadID);
              doFullDepthSearch = (value > localAlpha);
          }
      }

      if (doFullDepthSearch) // Go with full depth non-pv search
      {
          Value localAlpha = sp->alpha;
          ss[sp->ply].reduction = Depth(0);
          value = -search(pos, ss, -localAlpha, newDepth, sp->ply+1, true, threadID);

          if (value > localAlpha && value < sp->beta)
          {
              // If another thread has failed high then sp->alpha has been increased
              // to be higher or equal then beta, if so, avoid to start a PV search.
              localAlpha = sp->alpha;
              if (localAlpha < sp->beta)
                  value = -search_pv(pos, ss, -sp->beta, -localAlpha, newDepth, sp->ply+1, threadID);
              else
                  assert(thread_should_stop(threadID));
        }
      }
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      if (thread_should_stop(threadID))
      {
          lock_grab(&(sp->lock));
          break;
      }

      // New best move?
      if (value > sp->bestValue) // Less then 2% of cases
      {
          lock_grab(&(sp->lock));
          if (value > sp->bestValue && !thread_should_stop(threadID))
          {
              sp->bestValue = value;
              if (value > sp->alpha)
              {
                  // Ask threads to stop before to modify sp->alpha
                  if (value >= sp->beta)
                  {
                      for (int i = 0; i < ActiveThreads; i++)
                          if (i != threadID && (i == sp->master || sp->slaves[i]))
                              Threads[i].stop = true;

                      sp->finished = true;
                  }

                  sp->alpha = value;

                  sp_update_pv(sp->parentSstack, ss, sp->ply);
                  if (value == value_mate_in(sp->ply + 1))
                      ss[sp->ply].mateKiller = move;
              }
              // If we are at ply 1, and we are searching the first root move at
              // ply 0, set the 'Problem' variable if the score has dropped a lot
              // (from the computer's point of view) since the previous iteration.
              if (   sp->ply == 1
                     && Iteration >= 2
                     && -value <= ValueByIteration[Iteration-1] - ProblemMargin)
                  Problem = true;
          }
          lock_release(&(sp->lock));
      }
    }

    /* Here we have the lock still grabbed */

    // If this is the master thread and we have been asked to stop because of
    // a beta cutoff higher up in the tree, stop all slave threads.
    if (sp->master == threadID && thread_should_stop(threadID))
        for (int i = 0; i < ActiveThreads; i++)
            if (sp->slaves[i])
                Threads[i].stop = true;

    sp->cpus--;
    sp->slaves[threadID] = 0;

    lock_release(&(sp->lock));
  }

  /// The BetaCounterType class

  BetaCounterType::BetaCounterType() { clear(); }

  void BetaCounterType::clear() {

    for (int i = 0; i < THREAD_MAX; i++)
        Threads[i].betaCutOffs[WHITE] = Threads[i].betaCutOffs[BLACK] = 0ULL;
  }

  void BetaCounterType::add(Color us, Depth d, int threadID) {

    // Weighted count based on depth
    Threads[threadID].betaCutOffs[us] += unsigned(d);
  }

  void BetaCounterType::read(Color us, int64_t& our, int64_t& their) {

    our = their = 0UL;
    for (int i = 0; i < THREAD_MAX; i++)
    {
        our += Threads[i].betaCutOffs[us];
        their += Threads[i].betaCutOffs[opposite_color(us)];
    }
  }


  /// The RootMoveList class

  // RootMoveList c'tor

  RootMoveList::RootMoveList(Position& pos, Move searchMoves[]) : count(0) {

    SearchStack ss[PLY_MAX_PLUS_2];
    MoveStack mlist[MaxRootMoves];
    StateInfo st;
    bool includeAllMoves = (searchMoves[0] == MOVE_NONE);

    // Generate all legal moves
    MoveStack* last = generate_moves(pos, mlist);

    // Add each move to the moves[] array
    for (MoveStack* cur = mlist; cur != last; cur++)
    {
        bool includeMove = includeAllMoves;

        for (int k = 0; !includeMove && searchMoves[k] != MOVE_NONE; k++)
            includeMove = (searchMoves[k] == cur->move);

        if (!includeMove)
            continue;

        // Find a quick score for the move
        init_ss_array(ss);
        pos.do_move(cur->move, st);
        moves[count].move = cur->move;
        moves[count].score = -qsearch(pos, ss, -VALUE_INFINITE, VALUE_INFINITE, Depth(0), 1, 0);
        moves[count].pv[0] = cur->move;
        moves[count].pv[1] = MOVE_NONE;
        pos.undo_move(cur->move);
        count++;
    }
    sort();
  }


  // RootMoveList simple methods definitions

  void RootMoveList::set_move_nodes(int moveNum, int64_t nodes) {

    moves[moveNum].nodes = nodes;
    moves[moveNum].cumulativeNodes += nodes;
  }

  void RootMoveList::set_beta_counters(int moveNum, int64_t our, int64_t their) {

    moves[moveNum].ourBeta = our;
    moves[moveNum].theirBeta = their;
  }

  void RootMoveList::set_move_pv(int moveNum, const Move pv[]) {

    int j;

    for (j = 0; pv[j] != MOVE_NONE; j++)
        moves[moveNum].pv[j] = pv[j];

    moves[moveNum].pv[j] = MOVE_NONE;
  }


  // RootMoveList::sort() sorts the root move list at the beginning of a new
  // iteration.

  void RootMoveList::sort() {

    sort_multipv(count - 1); // Sort all items
  }


  // RootMoveList::sort_multipv() sorts the first few moves in the root move
  // list by their scores and depths. It is used to order the different PVs
  // correctly in MultiPV mode.

  void RootMoveList::sort_multipv(int n) {

    int i,j;

    for (i = 1; i <= n; i++)
    {
        RootMove rm = moves[i];
        for (j = i; j > 0 && moves[j - 1] < rm; j--)
            moves[j] = moves[j - 1];

        moves[j] = rm;
    }
  }


  // init_node() is called at the beginning of all the search functions
  // (search(), search_pv(), qsearch(), and so on) and initializes the
  // search stack object corresponding to the current node. Once every
  // NodesBetweenPolls nodes, init_node() also calls poll(), which polls
  // for user input and checks whether it is time to stop the search.

  void init_node(SearchStack ss[], int ply, int threadID) {

    assert(ply >= 0 && ply < PLY_MAX);
    assert(threadID >= 0 && threadID < ActiveThreads);

    Threads[threadID].nodes++;

    if (threadID == 0)
    {
        NodesSincePoll++;
        if (NodesSincePoll >= NodesBetweenPolls)
        {
            poll();
            NodesSincePoll = 0;
        }
    }
    ss[ply].init(ply);
    ss[ply + 2].initKillers();

    if (Threads[threadID].printCurrentLine)
        print_current_line(ss, ply, threadID);
  }


  // update_pv() is called whenever a search returns a value > alpha.
  // It updates the PV in the SearchStack object corresponding to the
  // current node.

  void update_pv(SearchStack ss[], int ply) {

    assert(ply >= 0 && ply < PLY_MAX);

    int p;

    ss[ply].pv[ply] = ss[ply].currentMove;

    for (p = ply + 1; ss[ply + 1].pv[p] != MOVE_NONE; p++)
        ss[ply].pv[p] = ss[ply + 1].pv[p];

    ss[ply].pv[p] = MOVE_NONE;
  }


  // sp_update_pv() is a variant of update_pv for use at split points. The
  // difference between the two functions is that sp_update_pv also updates
  // the PV at the parent node.

  void sp_update_pv(SearchStack* pss, SearchStack ss[], int ply) {

    assert(ply >= 0 && ply < PLY_MAX);

    int p;

    ss[ply].pv[ply] = pss[ply].pv[ply] = ss[ply].currentMove;

    for (p = ply + 1; ss[ply + 1].pv[p] != MOVE_NONE; p++)
        ss[ply].pv[p] = pss[ply].pv[p] = ss[ply + 1].pv[p];

    ss[ply].pv[p] = pss[ply].pv[p] = MOVE_NONE;
  }


  // connected_moves() tests whether two moves are 'connected' in the sense
  // that the first move somehow made the second move possible (for instance
  // if the moving piece is the same in both moves). The first move is assumed
  // to be the move that was made to reach the current position, while the
  // second move is assumed to be a move from the current position.

  bool connected_moves(const Position& pos, Move m1, Move m2) {

    Square f1, t1, f2, t2;
    Piece p;

    assert(move_is_ok(m1));
    assert(move_is_ok(m2));

    if (m2 == MOVE_NONE)
        return false;

    // Case 1: The moving piece is the same in both moves
    f2 = move_from(m2);
    t1 = move_to(m1);
    if (f2 == t1)
        return true;

    // Case 2: The destination square for m2 was vacated by m1
    t2 = move_to(m2);
    f1 = move_from(m1);
    if (t2 == f1)
        return true;

    // Case 3: Moving through the vacated square
    if (   piece_is_slider(pos.piece_on(f2))
        && bit_is_set(squares_between(f2, t2), f1))
      return true;

    // Case 4: The destination square for m2 is defended by the moving piece in m1
    p = pos.piece_on(t1);
    if (bit_is_set(pos.attacks_from(p, t1), t2))
        return true;

    // Case 5: Discovered check, checking piece is the piece moved in m1
    if (    piece_is_slider(p)
        &&  bit_is_set(squares_between(t1, pos.king_square(pos.side_to_move())), f2)
        && !bit_is_set(squares_between(t1, pos.king_square(pos.side_to_move())), t2))
    {
        // discovered_check_candidates() works also if the Position's side to
        // move is the opposite of the checking piece.
        Color them = opposite_color(pos.side_to_move());
        Bitboard dcCandidates = pos.discovered_check_candidates(them);

        if (bit_is_set(dcCandidates, f2))
            return true;
    }
    return false;
  }


  // value_is_mate() checks if the given value is a mate one
  // eventually compensated for the ply.

  bool value_is_mate(Value value) {

    assert(abs(value) <= VALUE_INFINITE);

    return   value <= value_mated_in(PLY_MAX)
          || value >= value_mate_in(PLY_MAX);
  }


  // move_is_killer() checks if the given move is among the
  // killer moves of that ply.

  bool move_is_killer(Move m, const SearchStack& ss) {

      const Move* k = ss.killers;
      for (int i = 0; i < KILLER_MAX; i++, k++)
          if (*k == m)
              return true;

      return false;
  }


  // extension() decides whether a move should be searched with normal depth,
  // or with extended depth. Certain classes of moves (checking moves, in
  // particular) are searched with bigger depth than ordinary moves and in
  // any case are marked as 'dangerous'. Note that also if a move is not
  // extended, as example because the corresponding UCI option is set to zero,
  // the move is marked as 'dangerous' so, at least, we avoid to prune it.

  Depth extension(const Position& pos, Move m, bool pvNode, bool captureOrPromotion,
                  bool moveIsCheck, bool singleEvasion, bool mateThreat, bool* dangerous) {

    assert(m != MOVE_NONE);

    Depth result = Depth(0);
    *dangerous = moveIsCheck | singleEvasion | mateThreat;

    if (*dangerous)
    {
        if (moveIsCheck)
            result += CheckExtension[pvNode];

        if (singleEvasion)
            result += SingleEvasionExtension[pvNode];

        if (mateThreat)
            result += MateThreatExtension[pvNode];
    }

    if (pos.type_of_piece_on(move_from(m)) == PAWN)
    {
        Color c = pos.side_to_move();
        if (relative_rank(c, move_to(m)) == RANK_7)
        {
            result += PawnPushTo7thExtension[pvNode];
            *dangerous = true;
        }
        if (pos.pawn_is_passed(c, move_to(m)))
        {
            result += PassedPawnExtension[pvNode];
            *dangerous = true;
        }
    }

    if (   captureOrPromotion
        && pos.type_of_piece_on(move_to(m)) != PAWN
        && (  pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK)
            - pos.midgame_value_of_piece_on(move_to(m)) == Value(0))
        && !move_is_promotion(m)
        && !move_is_ep(m))
    {
        result += PawnEndgameExtension[pvNode];
        *dangerous = true;
    }

    if (   pvNode
        && captureOrPromotion
        && pos.type_of_piece_on(move_to(m)) != PAWN
        && pos.see_sign(m) >= 0)
    {
        result += OnePly/2;
        *dangerous = true;
    }

    return Min(result, OnePly);
  }


  // ok_to_do_nullmove() looks at the current position and decides whether
  // doing a 'null move' should be allowed. In order to avoid zugzwang
  // problems, null moves are not allowed when the side to move has very
  // little material left. Currently, the test is a bit too simple: Null
  // moves are avoided only when the side to move has only pawns left.
  // It's probably a good idea to avoid null moves in at least some more
  // complicated endgames, e.g. KQ vs KR.  FIXME

  bool ok_to_do_nullmove(const Position& pos) {

    return pos.non_pawn_material(pos.side_to_move()) != Value(0);
  }


  // ok_to_prune() tests whether it is safe to forward prune a move. Only
  // non-tactical moves late in the move list close to the leaves are
  // candidates for pruning.

  bool ok_to_prune(const Position& pos, Move m, Move threat) {

    assert(move_is_ok(m));
    assert(threat == MOVE_NONE || move_is_ok(threat));
    assert(!pos.move_is_check(m));
    assert(!pos.move_is_capture_or_promotion(m));
    assert(!pos.move_is_passed_pawn_push(m));

    Square mfrom, mto, tfrom, tto;

    // Prune if there isn't any threat move
    if (threat == MOVE_NONE)
        return true;

    mfrom = move_from(m);
    mto = move_to(m);
    tfrom = move_from(threat);
    tto = move_to(threat);

    // Case 1: Don't prune moves which move the threatened piece
    if (mfrom == tto)
        return false;

    // Case 2: If the threatened piece has value less than or equal to the
    // value of the threatening piece, don't prune move which defend it.
    if (   pos.move_is_capture(threat)
        && (   pos.midgame_value_of_piece_on(tfrom) >= pos.midgame_value_of_piece_on(tto)
            || pos.type_of_piece_on(tfrom) == KING)
        && pos.move_attacks_square(m, tto))
        return false;

    // Case 3: If the moving piece in the threatened move is a slider, don't
    // prune safe moves which block its ray.
    if (   piece_is_slider(pos.piece_on(tfrom))
        && bit_is_set(squares_between(tfrom, tto), mto)
        && pos.see_sign(m) >= 0)
        return false;

    return true;
  }


  // ok_to_use_TT() returns true if a transposition table score
  // can be used at a given point in search.

  bool ok_to_use_TT(const TTEntry* tte, Depth depth, Value beta, int ply) {

    Value v = value_from_tt(tte->value(), ply);

    return   (   tte->depth() >= depth
              || v >= Max(value_mate_in(PLY_MAX), beta)
              || v < Min(value_mated_in(PLY_MAX), beta))

          && (   (is_lower_bound(tte->type()) && v >= beta)
              || (is_upper_bound(tte->type()) && v < beta));
  }


  // refine_eval() returns the transposition table score if
  // possible otherwise falls back on static position evaluation.

  Value refine_eval(const TTEntry* tte, Value defaultEval, int ply) {

      if (!tte)
          return defaultEval;

      Value v = value_from_tt(tte->value(), ply);

      if (   (is_lower_bound(tte->type()) && v >= defaultEval)
          || (is_upper_bound(tte->type()) && v < defaultEval))
          return v;

      return defaultEval;
  }


  // update_history() registers a good move that produced a beta-cutoff
  // in history and marks as failures all the other moves of that ply.

  void update_history(const Position& pos, Move move, Depth depth,
                      Move movesSearched[], int moveCount) {

    Move m;

    H.success(pos.piece_on(move_from(move)), move_to(move), depth);

    for (int i = 0; i < moveCount - 1; i++)
    {
        m = movesSearched[i];

        assert(m != move);

        if (!pos.move_is_capture_or_promotion(m))
            H.failure(pos.piece_on(move_from(m)), move_to(m), depth);
    }
  }


  // update_killers() add a good move that produced a beta-cutoff
  // among the killer moves of that ply.

  void update_killers(Move m, SearchStack& ss) {

    if (m == ss.killers[0])
        return;

    for (int i = KILLER_MAX - 1; i > 0; i--)
        ss.killers[i] = ss.killers[i - 1];

    ss.killers[0] = m;
  }


  // update_gains() updates the gains table of a non-capture move given
  // the static position evaluation before and after the move.

  void update_gains(const Position& pos, Move m, Value before, Value after) {

    if (   m != MOVE_NULL
        && before != VALUE_NONE
        && after != VALUE_NONE
        && pos.captured_piece() == NO_PIECE_TYPE
        && !move_is_castle(m)
        && !move_is_promotion(m))
        H.set_gain(pos.piece_on(move_to(m)), move_to(m), -(before + after));
  }


  // current_search_time() returns the number of milliseconds which have passed
  // since the beginning of the current search.

  int current_search_time() {

    return get_system_time() - SearchStartTime;
  }


  // nps() computes the current nodes/second count.

  int nps() {

    int t = current_search_time();
    return (t > 0 ? int((nodes_searched() * 1000) / t) : 0);
  }


  // poll() performs two different functions: It polls for user input, and it
  // looks at the time consumed so far and decides if it's time to abort the
  // search.

  void poll() {

    static int lastInfoTime;
    int t = current_search_time();

    //  Poll for input
    if (Bioskey())
    {
        // We are line oriented, don't read single chars
        std::string command;

        if (!std::getline(std::cin, command))
            command = "quit";

        if (command == "quit")
        {
            AbortSearch = true;
            PonderSearch = false;
            Quit = true;
            return;
        }
        else if (command == "stop")
        {
            AbortSearch = true;
            PonderSearch = false;
        }
        else if (command == "ponderhit")
            ponderhit();
    }

    // Print search information
    if (t < 1000)
        lastInfoTime = 0;

    else if (lastInfoTime > t)
        // HACK: Must be a new search where we searched less than
        // NodesBetweenPolls nodes during the first second of search.
        lastInfoTime = 0;

    else if (t - lastInfoTime >= 1000)
    {
        lastInfoTime = t;
        lock_grab(&IOLock);

        if (dbg_show_mean)
            dbg_print_mean();

        if (dbg_show_hit_rate)
            dbg_print_hit_rate();

        cout << "info nodes " << nodes_searched() << " nps " << nps()
             << " time " << t << " hashfull " << TT.full() << endl;

        lock_release(&IOLock);

        if (ShowCurrentLine)
            Threads[0].printCurrentLine = true;
    }

    // Should we stop the search?
    if (PonderSearch)
        return;

    bool stillAtFirstMove =    RootMoveNumber == 1
                           && !FailLow
                           &&  t > MaxSearchTime + ExtraSearchTime;

    bool noMoreTime =   t > AbsoluteMaxSearchTime
                     || stillAtFirstMove;

    if (   (Iteration >= 3 && UseTimeManagement && noMoreTime)
        || (ExactMaxTime && t >= ExactMaxTime)
        || (Iteration >= 3 && MaxNodes && nodes_searched() >= MaxNodes))
        AbortSearch = true;
  }


  // ponderhit() is called when the program is pondering (i.e. thinking while
  // it's the opponent's turn to move) in order to let the engine know that
  // it correctly predicted the opponent's move.

  void ponderhit() {

    int t = current_search_time();
    PonderSearch = false;

    bool stillAtFirstMove =    RootMoveNumber == 1
                           && !FailLow
                           &&  t > MaxSearchTime + ExtraSearchTime;

    bool noMoreTime =   t > AbsoluteMaxSearchTime
                     || stillAtFirstMove;

    if (Iteration >= 3 && UseTimeManagement && (noMoreTime || StopOnPonderhit))
        AbortSearch = true;
  }


  // print_current_line() prints the current line of search for a given
  // thread. Called when the UCI option UCI_ShowCurrLine is 'true'.

  void print_current_line(SearchStack ss[], int ply, int threadID) {

    assert(ply >= 0 && ply < PLY_MAX);
    assert(threadID >= 0 && threadID < ActiveThreads);

    if (!Threads[threadID].idle)
    {
        lock_grab(&IOLock);
        cout << "info currline " << (threadID + 1);
        for (int p = 0; p < ply; p++)
            cout << " " << ss[p].currentMove;

        cout << endl;
        lock_release(&IOLock);
    }
    Threads[threadID].printCurrentLine = false;
    if (threadID + 1 < ActiveThreads)
        Threads[threadID + 1].printCurrentLine = true;
  }


  // init_ss_array() does a fast reset of the first entries of a SearchStack array

  void init_ss_array(SearchStack ss[]) {

    for (int i = 0; i < 3; i++)
    {
        ss[i].init(i);
        ss[i].initKillers();
    }
  }


  // wait_for_stop_or_ponderhit() is called when the maximum depth is reached
  // while the program is pondering. The point is to work around a wrinkle in
  // the UCI protocol: When pondering, the engine is not allowed to give a
  // "bestmove" before the GUI sends it a "stop" or "ponderhit" command.
  // We simply wait here until one of these commands is sent, and return,
  // after which the bestmove and pondermove will be printed (in id_loop()).

  void wait_for_stop_or_ponderhit() {

    std::string command;

    while (true)
    {
        if (!std::getline(std::cin, command))
            command = "quit";

        if (command == "quit")
        {
            Quit = true;
            break;
        }
        else if (command == "ponderhit" || command == "stop")
            break;
    }
  }


  // idle_loop() is where the threads are parked when they have no work to do.
  // The parameter "waitSp", if non-NULL, is a pointer to an active SplitPoint
  // object for which the current thread is the master.

  void idle_loop(int threadID, SplitPoint* waitSp) {

    assert(threadID >= 0 && threadID < THREAD_MAX);

    Threads[threadID].running = true;

    while (true)
    {
        if (AllThreadsShouldExit && threadID != 0)
            break;

        // If we are not thinking, wait for a condition to be signaled
        // instead of wasting CPU time polling for work.
        while (threadID != 0 && (Idle || threadID >= ActiveThreads))
        {

#if !defined(_MSC_VER)
            pthread_mutex_lock(&WaitLock);
            if (Idle || threadID >= ActiveThreads)
                pthread_cond_wait(&WaitCond, &WaitLock);

            pthread_mutex_unlock(&WaitLock);
#else
            WaitForSingleObject(SitIdleEvent[threadID], INFINITE);
#endif
        }

      // If this thread has been assigned work, launch a search
      if (Threads[threadID].workIsWaiting)
      {
          assert(!Threads[threadID].idle);

          Threads[threadID].workIsWaiting = false;
          if (Threads[threadID].splitPoint->pvNode)
              sp_search_pv(Threads[threadID].splitPoint, threadID);
          else
              sp_search(Threads[threadID].splitPoint, threadID);

          Threads[threadID].idle = true;
      }

      // If this thread is the master of a split point and all threads have
      // finished their work at this split point, return from the idle loop.
      if (waitSp != NULL && waitSp->cpus == 0)
          return;
    }

    Threads[threadID].running = false;
  }


  // init_split_point_stack() is called during program initialization, and
  // initializes all split point objects.

  void init_split_point_stack() {

    for (int i = 0; i < THREAD_MAX; i++)
        for (int j = 0; j < ACTIVE_SPLIT_POINTS_MAX; j++)
        {
            SplitPointStack[i][j].parent = NULL;
            lock_init(&(SplitPointStack[i][j].lock), NULL);
        }
  }


  // destroy_split_point_stack() is called when the program exits, and
  // destroys all locks in the precomputed split point objects.

  void destroy_split_point_stack() {

    for (int i = 0; i < THREAD_MAX; i++)
        for (int j = 0; j < ACTIVE_SPLIT_POINTS_MAX; j++)
            lock_destroy(&(SplitPointStack[i][j].lock));
  }


  // thread_should_stop() checks whether the thread with a given threadID has
  // been asked to stop, directly or indirectly. This can happen if a beta
  // cutoff has occurred in the thread's currently active split point, or in
  // some ancestor of the current split point.

  bool thread_should_stop(int threadID) {

    assert(threadID >= 0 && threadID < ActiveThreads);

    SplitPoint* sp;

    if (Threads[threadID].stop)
        return true;
    if (ActiveThreads <= 2)
        return false;
    for (sp = Threads[threadID].splitPoint; sp != NULL; sp = sp->parent)
        if (sp->finished)
        {
            Threads[threadID].stop = true;
            return true;
        }
    return false;
  }


  // thread_is_available() checks whether the thread with threadID "slave" is
  // available to help the thread with threadID "master" at a split point. An
  // obvious requirement is that "slave" must be idle. With more than two
  // threads, this is not by itself sufficient:  If "slave" is the master of
  // some active split point, it is only available as a slave to the other
  // threads which are busy searching the split point at the top of "slave"'s
  // split point stack (the "helpful master concept" in YBWC terminology).

  bool thread_is_available(int slave, int master) {

    assert(slave >= 0 && slave < ActiveThreads);
    assert(master >= 0 && master < ActiveThreads);
    assert(ActiveThreads > 1);

    if (!Threads[slave].idle || slave == master)
        return false;

    // Make a local copy to be sure doesn't change under our feet
    int localActiveSplitPoints = Threads[slave].activeSplitPoints;

    if (localActiveSplitPoints == 0)
        // No active split points means that the thread is available as
        // a slave for any other thread.
        return true;

    if (ActiveThreads == 2)
        return true;

    // Apply the "helpful master" concept if possible. Use localActiveSplitPoints
    // that is known to be > 0, instead of Threads[slave].activeSplitPoints that
    // could have been set to 0 by another thread leading to an out of bound access.
    if (SplitPointStack[slave][localActiveSplitPoints - 1].slaves[master])
        return true;

    return false;
  }


  // idle_thread_exists() tries to find an idle thread which is available as
  // a slave for the thread with threadID "master".

  bool idle_thread_exists(int master) {

    assert(master >= 0 && master < ActiveThreads);
    assert(ActiveThreads > 1);

    for (int i = 0; i < ActiveThreads; i++)
        if (thread_is_available(i, master))
            return true;

    return false;
  }


  // split() does the actual work of distributing the work at a node between
  // several threads at PV nodes. If it does not succeed in splitting the
  // node (because no idle threads are available, or because we have no unused
  // split point objects), the function immediately returns false. If
  // splitting is possible, a SplitPoint object is initialized with all the
  // data that must be copied to the helper threads (the current position and
  // search stack, alpha, beta, the search depth, etc.), and we tell our
  // helper threads that they have been assigned work. This will cause them
  // to instantly leave their idle loops and call sp_search_pv(). When all
  // threads have returned from sp_search_pv (or, equivalently, when
  // splitPoint->cpus becomes 0), split() returns true.

  bool split(const Position& p, SearchStack* sstck, int ply,
             Value* alpha, Value* beta, Value* bestValue, const Value futilityValue,
             Depth depth, int* moves, MovePicker* mp, int master, bool pvNode) {

    assert(p.is_ok());
    assert(sstck != NULL);
    assert(ply >= 0 && ply < PLY_MAX);
    assert(*bestValue >= -VALUE_INFINITE && *bestValue <= *alpha);
    assert(!pvNode || *alpha < *beta);
    assert(*beta <= VALUE_INFINITE);
    assert(depth > Depth(0));
    assert(master >= 0 && master < ActiveThreads);
    assert(ActiveThreads > 1);

    SplitPoint* splitPoint;

    lock_grab(&MPLock);

    // If no other thread is available to help us, or if we have too many
    // active split points, don't split.
    if (   !idle_thread_exists(master)
        || Threads[master].activeSplitPoints >= ACTIVE_SPLIT_POINTS_MAX)
    {
        lock_release(&MPLock);
        return false;
    }

    // Pick the next available split point object from the split point stack
    splitPoint = SplitPointStack[master] + Threads[master].activeSplitPoints;
    Threads[master].activeSplitPoints++;

    // Initialize the split point object
    splitPoint->parent = Threads[master].splitPoint;
    splitPoint->finished = false;
    splitPoint->ply = ply;
    splitPoint->depth = depth;
    splitPoint->alpha = pvNode ? *alpha : (*beta - 1);
    splitPoint->beta = *beta;
    splitPoint->pvNode = pvNode;
    splitPoint->bestValue = *bestValue;
    splitPoint->futilityValue = futilityValue;
    splitPoint->master = master;
    splitPoint->mp = mp;
    splitPoint->moves = *moves;
    splitPoint->cpus = 1;
    splitPoint->pos = &p;
    splitPoint->parentSstack = sstck;
    for (int i = 0; i < ActiveThreads; i++)
        splitPoint->slaves[i] = 0;

    Threads[master].idle = false;
    Threads[master].stop = false;
    Threads[master].splitPoint = splitPoint;

    // Allocate available threads setting idle flag to false
    for (int i = 0; i < ActiveThreads && splitPoint->cpus < MaxThreadsPerSplitPoint; i++)
        if (thread_is_available(i, master))
        {
            Threads[i].idle = false;
            Threads[i].stop = false;
            Threads[i].splitPoint = splitPoint;
            splitPoint->slaves[i] = 1;
            splitPoint->cpus++;
        }

    assert(splitPoint->cpus > 1);

    // We can release the lock because master and slave threads are already booked
    lock_release(&MPLock);

    // Tell the threads that they have work to do. This will make them leave
    // their idle loop. But before copy search stack tail for each thread.
    for (int i = 0; i < ActiveThreads; i++)
        if (i == master || splitPoint->slaves[i])
        {
            memcpy(splitPoint->sstack[i] + ply - 1, sstck + ply - 1, 3 * sizeof(SearchStack));
            Threads[i].workIsWaiting = true; // This makes the slave to exit from idle_loop()
        }

    // Everything is set up. The master thread enters the idle loop, from
    // which it will instantly launch a search, because its workIsWaiting
    // slot is 'true'.  We send the split point as a second parameter to the
    // idle loop, which means that the main thread will return from the idle
    // loop when all threads have finished their work at this split point
    // (i.e. when splitPoint->cpus == 0).
    idle_loop(master, splitPoint);

    // We have returned from the idle loop, which means that all threads are
    // finished. Update alpha, beta and bestValue, and return.
    lock_grab(&MPLock);

    if (pvNode)
        *alpha = splitPoint->alpha;

    *beta = splitPoint->beta;
    *bestValue = splitPoint->bestValue;
    Threads[master].stop = false;
    Threads[master].idle = false;
    Threads[master].activeSplitPoints--;
    Threads[master].splitPoint = splitPoint->parent;

    lock_release(&MPLock);
    return true;
  }


  // wake_sleeping_threads() wakes up all sleeping threads when it is time
  // to start a new search from the root.

  void wake_sleeping_threads() {

    if (ActiveThreads > 1)
    {
        for (int i = 1; i < ActiveThreads; i++)
        {
            Threads[i].idle = true;
            Threads[i].workIsWaiting = false;
        }

#if !defined(_MSC_VER)
      pthread_mutex_lock(&WaitLock);
      pthread_cond_broadcast(&WaitCond);
      pthread_mutex_unlock(&WaitLock);
#else
      for (int i = 1; i < THREAD_MAX; i++)
          SetEvent(SitIdleEvent[i]);
#endif
    }
  }


  // init_thread() is the function which is called when a new thread is
  // launched. It simply calls the idle_loop() function with the supplied
  // threadID. There are two versions of this function; one for POSIX
  // threads and one for Windows threads.

#if !defined(_MSC_VER)

  void* init_thread(void *threadID) {

    idle_loop(*(int*)threadID, NULL);
    return NULL;
  }

#else

  DWORD WINAPI init_thread(LPVOID threadID) {

    idle_loop(*(int*)threadID, NULL);
    return NULL;
  }

#endif

}
