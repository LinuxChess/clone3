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

#include <iostream>

#include "search.h"
#include "thread.h"
#include "ucioption.h"

ThreadsManager Threads; // Global object definition

namespace { extern "C" {

 // start_routine() is the C function which is called when a new thread
 // is launched. It simply calls idle_loop() of the supplied thread. The
 // last two threads are dedicated to read input from GUI and to mimic a
 // timer, so they run in listener_loop() and timer_loop() respectively.

#if defined(_MSC_VER)
  DWORD WINAPI start_routine(LPVOID thread) {
#else
  void* start_routine(void* thread) {
#endif

    if (((Thread*)thread)->threadID == 0)
        ((Thread*)thread)->main_loop();

    else if (((Thread*)thread)->threadID == MAX_THREADS)
        ((Thread*)thread)->timer_loop();
    else
        ((Thread*)thread)->idle_loop(NULL);

    return 0;
  }

} }


// wake_up() wakes up the thread, normally at the beginning of the search or,
// if "sleeping threads" is used, when there is some work to do.

void Thread::wake_up() {

  lock_grab(&sleepLock);
  cond_signal(&sleepCond);
  lock_release(&sleepLock);
}


// cutoff_occurred() checks whether a beta cutoff has occurred in the current
// active split point, or in some ancestor of the split point.

bool Thread::cutoff_occurred() const {

  for (SplitPoint* sp = splitPoint; sp; sp = sp->parent)
      if (sp->is_betaCutoff)
          return true;
  return false;
}


// is_available_to() checks whether the thread is available to help the thread with
// threadID "master" at a split point. An obvious requirement is that thread must be
// idle. With more than two threads, this is not by itself sufficient: If the thread
// is the master of some active split point, it is only available as a slave to the
// threads which are busy searching the split point at the top of "slave"'s split
// point stack (the "helpful master concept" in YBWC terminology).

bool Thread::is_available_to(int master) const {

  if (is_searching)
      return false;

  // Make a local copy to be sure doesn't become zero under our feet while
  // testing next condition and so leading to an out of bound access.
  int localActiveSplitPoints = activeSplitPoints;

  // No active split points means that the thread is available as a slave for any
  // other thread otherwise apply the "helpful master" concept if possible.
  if (   !localActiveSplitPoints
      || splitPoints[localActiveSplitPoints - 1].is_slave[master])
      return true;

  return false;
}


// read_uci_options() updates number of active threads and other internal
// parameters according to the UCI options values. It is called before
// to start a new search.

void ThreadsManager::read_uci_options() {

  maxThreadsPerSplitPoint = Options["Maximum Number of Threads per Split Point"].value<int>();
  minimumSplitDepth       = Options["Minimum Split Depth"].value<int>() * ONE_PLY;
  useSleepingThreads      = Options["Use Sleeping Threads"].value<bool>();

  set_size(Options["Threads"].value<int>());
}


// set_size() changes the number of active threads and raises do_sleep flag for
// all the unused threads that will go immediately to sleep.

void ThreadsManager::set_size(int cnt) {

  assert(cnt > 0 && cnt <= MAX_THREADS);

  activeThreads = cnt;

  for (int i = 1; i < MAX_THREADS; i++) // Ignore main thread
      if (i < activeThreads)
      {
          // Dynamically allocate pawn and material hash tables according to the
          // number of active threads. This avoids preallocating memory for all
          // possible threads if only few are used as, for instance, on mobile
          // devices where memory is scarce and allocating for MAX_THREADS could
          // even result in a crash.
          threads[i].pawnTable.init();
          threads[i].materialTable.init();

          threads[i].do_sleep = false;
      }
      else
          threads[i].do_sleep = true;
}


// init() is called during startup. Initializes locks and condition variables
// and launches all threads sending them immediately to sleep.

void ThreadsManager::init() {

  // Initialize sleep condition used to block waiting for end of searching
  cond_init(&sleepCond);

  // Initialize threads lock, used when allocating slaves during splitting
  lock_init(&threadsLock);

  // Initialize sleep and split point locks
  for (int i = 0; i <= MAX_THREADS; i++)
  {
      lock_init(&threads[i].sleepLock);
      cond_init(&threads[i].sleepCond);

      for (int j = 0; j < MAX_ACTIVE_SPLIT_POINTS; j++)
          lock_init(&(threads[i].splitPoints[j].lock));
  }

  // Initialize main thread's associated data
  threads[0].pawnTable.init();
  threads[0].materialTable.init();

  // Create and launch all the threads, threads will go immediately to sleep
  for (int i = 0; i <= MAX_THREADS; i++)
  {
      threads[i].is_searching = false;
      threads[i].do_sleep = true;
      threads[i].threadID = i;

#if defined(_MSC_VER)
      threads[i].handle = CreateThread(NULL, 0, start_routine, (LPVOID)&threads[i], 0, NULL);
      bool ok = (threads[i].handle != NULL);
#else
      bool ok = (pthread_create(&threads[i].handle, NULL, start_routine, (void*)&threads[i]) == 0);
#endif

      if (!ok)
      {
          std::cerr << "Failed to create thread number " << i << std::endl;
          ::exit(EXIT_FAILURE);
      }
  }
}


// exit() is called to cleanly terminate the threads when the program finishes

void ThreadsManager::exit() {

  for (int i = 0; i <= MAX_THREADS; i++)
  {
      threads[i].do_terminate = true;
      threads[i].wake_up();

      // Wait for slave termination
#if defined(_MSC_VER)
      WaitForSingleObject(threads[i].handle, 0);
      CloseHandle(threads[i].handle);
#else
      pthread_join(threads[i].handle, NULL);
#endif

      // Now we can safely destroy locks and wait conditions
      lock_destroy(&threads[i].sleepLock);
      cond_destroy(&threads[i].sleepCond);

      for (int j = 0; j < MAX_ACTIVE_SPLIT_POINTS; j++)
          lock_destroy(&(threads[i].splitPoints[j].lock));
  }

  lock_destroy(&threadsLock);
  cond_destroy(&sleepCond);
}


// available_slave_exists() tries to find an idle thread which is available as
// a slave for the thread with threadID "master".

bool ThreadsManager::available_slave_exists(int master) const {

  assert(master >= 0 && master < activeThreads);

  for (int i = 0; i < activeThreads; i++)
      if (i != master && threads[i].is_available_to(master))
          return true;

  return false;
}


// split_point_finished() checks if all the slave threads of a given split
// point have finished searching.

bool ThreadsManager::split_point_finished(SplitPoint* sp) const {

  for (int i = 0; i < activeThreads; i++)
      if (sp->is_slave[i])
          return false;

  return true;
}


// split() does the actual work of distributing the work at a node between
// several available threads. If it does not succeed in splitting the
// node (because no idle threads are available, or because we have no unused
// split point objects), the function immediately returns. If splitting is
// possible, a SplitPoint object is initialized with all the data that must be
// copied to the helper threads and we tell our helper threads that they have
// been assigned work. This will cause them to instantly leave their idle loops and
// call search().When all threads have returned from search() then split() returns.

template <bool Fake>
Value ThreadsManager::split(Position& pos, SearchStack* ss, Value alpha, Value beta,
                            Value bestValue, Depth depth, Move threatMove,
                            int moveCount, MovePicker* mp, int nodeType) {
  assert(pos.pos_is_ok());
  assert(bestValue >= -VALUE_INFINITE);
  assert(bestValue <= alpha);
  assert(alpha < beta);
  assert(beta <= VALUE_INFINITE);
  assert(depth > DEPTH_ZERO);
  assert(pos.thread() >= 0 && pos.thread() < activeThreads);
  assert(activeThreads > 1);

  int i, master = pos.thread();
  Thread& masterThread = threads[master];

  // If we already have too many active split points, don't split
  if (masterThread.activeSplitPoints >= MAX_ACTIVE_SPLIT_POINTS)
      return bestValue;

  // Pick the next available split point object from the split point stack
  SplitPoint* sp = masterThread.splitPoints + masterThread.activeSplitPoints;

  // Initialize the split point object
  sp->parent = masterThread.splitPoint;
  sp->master = master;
  sp->is_betaCutoff = false;
  sp->depth = depth;
  sp->threatMove = threatMove;
  sp->alpha = alpha;
  sp->beta = beta;
  sp->nodeType = nodeType;
  sp->bestValue = bestValue;
  sp->mp = mp;
  sp->moveCount = moveCount;
  sp->pos = &pos;
  sp->nodes = 0;
  sp->ss = ss;
  for (i = 0; i < activeThreads; i++)
      sp->is_slave[i] = false;

  // If we are here it means we are not available
  assert(masterThread.is_searching);

  int workersCnt = 1; // At least the master is included

  // Try to allocate available threads and ask them to start searching setting
  // the state to Thread::WORKISWAITING, this must be done under lock protection
  // to avoid concurrent allocation of the same slave by another master.
  lock_grab(&threadsLock);

  for (i = 0; !Fake && i < activeThreads && workersCnt < maxThreadsPerSplitPoint; i++)
      if (i != master && threads[i].is_available_to(master))
      {
          workersCnt++;
          sp->is_slave[i] = true;
          threads[i].splitPoint = sp;

          // This makes the slave to exit from idle_loop()
          threads[i].is_searching = true;

          if (useSleepingThreads)
              threads[i].wake_up();
      }

  lock_release(&threadsLock);

  // We failed to allocate even one slave, return
  if (!Fake && workersCnt == 1)
      return bestValue;

  masterThread.splitPoint = sp;
  masterThread.activeSplitPoints++;

  // Everything is set up. The master thread enters the idle loop, from which
  // it will instantly launch a search, because its is_searching flag is set.
  // We pass the split point as a parameter to the idle loop, which means that
  // the thread will return from the idle loop when all slaves have finished
  // their work at this split point.
  masterThread.idle_loop(sp);

  // In helpful master concept a master can help only a sub-tree, and
  // because here is all finished is not possible master is booked.
  assert(!masterThread.is_searching);

  // We have returned from the idle loop, which means that all threads are
  // finished. Note that changing state and decreasing activeSplitPoints is done
  // under lock protection to avoid a race with Thread::is_available_to().
  lock_grab(&threadsLock);

  masterThread.is_searching = true;
  masterThread.activeSplitPoints--;

  lock_release(&threadsLock);

  masterThread.splitPoint = sp->parent;
  pos.set_nodes_searched(pos.nodes_searched() + sp->nodes);

  return sp->bestValue;
}

// Explicit template instantiations
template Value ThreadsManager::split<false>(Position&, SearchStack*, Value, Value, Value, Depth, Move, int, MovePicker*, int);
template Value ThreadsManager::split<true>(Position&, SearchStack*, Value, Value, Value, Depth, Move, int, MovePicker*, int);


// Thread::timer_loop() is where the timer thread waits maxPly milliseconds
// and then calls do_timer_event().

void Thread::timer_loop() {

  while (!do_terminate)
  {
      lock_grab(&sleepLock);
      timed_wait(&sleepCond, &sleepLock, maxPly ? maxPly : INT_MAX);
      lock_release(&sleepLock);
      do_timer_event();
  }
}


// ThreadsManager::set_timer() is used to set the timer to trigger after msec
// milliseconds. If msec is 0 then timer is stopped.

void ThreadsManager::set_timer(int msec) {

  Thread& timer = threads[MAX_THREADS];

  lock_grab(&timer.sleepLock);
  timer.maxPly = msec;
  cond_signal(&timer.sleepCond); // Wake up and restart the timer
  lock_release(&timer.sleepLock);
}


// Thread::main_loop() is where the main thread is parked waiting to be started
// when there is a new search. Main thread will launch all the slave threads.

void Thread::main_loop() {

  while (true)
  {
      lock_grab(&sleepLock);

      do_sleep = true; // Always return to sleep after a search

      is_searching = false;

      while (do_sleep && !do_terminate)
      {
          cond_signal(&Threads.sleepCond); // Wake up UI thread if needed
          cond_wait(&sleepCond, &sleepLock);
      }

      is_searching = true;

      lock_release(&sleepLock);

      if (do_terminate)
          return;

      Search::think();
  }
}


// ThreadsManager::wait_end_of_search() blocks UI thread until main thread has
// returned to sleep in main_loop(). It is needed becuase xboard sends immediately
// new position to search after a "stop" due to ponder miss.

void ThreadsManager::wait_end_of_search() {

  Thread& main = threads[0];

  lock_grab(&main.sleepLock);

  while (!main.do_sleep)
      cond_wait(&sleepCond, &main.sleepLock);

  lock_release(&main.sleepLock);
}


// ThreadsManager::start_thinking() is used by UI thread to wake up the main
// thread parked in main_loop() and starting a new search. If asyncMode is true
// then function returns immediately, otherwise caller is blocked waiting for
// the search to finish.

void ThreadsManager::start_thinking(bool asyncMode) {

  Thread& main = threads[0];

  // Wait main thread has finished before to launch a new search
  wait_end_of_search();

  lock_grab(&main.sleepLock);

  // Reset signals before to start the search
  memset((void*)&Search::Signals, 0, sizeof(Search::Signals));

  main.do_sleep = false;
  cond_signal(&main.sleepCond); // Wake up main thread

  if (!asyncMode)
      cond_wait(&sleepCond, &main.sleepLock);

  lock_release(&main.sleepLock);
}


// ThreadsManager::wait_for_stop_or_ponderhit() is called when the maximum depth
// is reached while the program is pondering. The point is to work around a wrinkle
// in the UCI protocol: When pondering, the engine is not allowed to give a
// "bestmove" before the GUI sends it a "stop" or "ponderhit" command.
// We simply wait here until one of these commands (that raise StopRequest) is
// sent, and return, after which the bestmove and pondermove will be printed.

void ThreadsManager::wait_for_stop_or_ponderhit() {

  Search::Signals.stopOnPonderhit = true;

  Thread& main = threads[0];

  lock_grab(&main.sleepLock);

  while (!Search::Signals.stop)
      cond_wait(&main.sleepCond, &main.sleepLock);

  lock_release(&main.sleepLock);
}
