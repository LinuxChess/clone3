# Stockfish, a UCI chess playing engine derived from Glaurung 2.1
# Copyright (C) 2004-2007 Tord Romstad
# Copyright (C) 2008 Marco Costalba

# This file is part of Stockfish.
#
# Stockfish is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Stockfish is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.


### Executable name. Do not change
EXE = stockfish


### ==========================================================================
### Compiler speed switches for both GCC and ICC. These settings are generally
### fast on a broad range of systems, but may be changed experimentally
###
### NOTE: Some versions of gcc miscompile value.h with -O2 or -O3, this is the
### safe setup, try changing to -O3 or -O2 and verify it works for you.
### ==========================================================================
GCCFLAGS = -O1 -msse
ICCFLAGS = -fast -msse
ICCFLAGS-OSX = -fast -mdynamic-no-pic


### ==========================================================================
### Enable/disable debugging, disabled by default
### ==========================================================================
GCCFLAGS += -DNDEBUG
ICCFLAGS += -DNDEBUG
ICCFLAGS-OSX += -DNDEBUG


### ==========================================================================
### Remove below comments to compile for a big-endian machine
### ==========================================================================
#GCCFLAGS += -DBIGENDIAN
#ICCFLAGS += -DBIGENDIAN
#ICCFLAGS-OSX += -DBIGENDIAN


### ==========================================================================
### Run built-in benchmark for pgo-builds with:  32MB hash  1 thread  10 depth
### These settings are generally fast, but may be changed experimentally
### ==========================================================================
PGOBENCH = ./$(EXE) bench 32 1 10 default depth


### General compiler settings. Do not change
GCCFLAGS += -g -Wall -fno-exceptions -fno-rtti
ICCFLAGS += -g -Wall -fno-exceptions -fno-rtti -wd383,869,981,10187,10188,11505,11503
ICCFLAGS-OSX += -g -Wall -fno-exceptions -fno-rtti -wd383,869,981,10187,10188,11505,11503


### General linker settings. Do not change
LDFLAGS  = -lpthread


### Object files. Do not change
OBJS = application.o bitboard.o pawns.o material.o endgame.o evaluate.o main.o \
	misc.o move.o movegen.o history.o movepick.o search.o piece.o \
	position.o direction.o tt.o value.o uci.o ucioption.o \
	mersenne.o book.o bitbase.o san.o benchmark.o


### General rules. Do not change
default:
	$(MAKE) gcc

help:
	@echo ""
	@echo "Makefile options:"
	@echo ""
	@echo "make                >  Default: Compiler = g++"
	@echo "make icc            >  Compiler = icpc"
	@echo "make icc-profile    >  Compiler = icpc + automatic pgo-build"
	@echo "make icc-profile-popcnt >  Compiler = icpc + automatic pgo-build + popcnt-support"
	@echo "make osx-ppc32      >  PPC-Mac OS X 32 bit. Compiler = g++"
	@echo "make osx-ppc64      >  PPC-Mac OS X 64 bit. Compiler = g++"
	@echo "make osx-x86        >  x86-Mac OS X 32 bit. Compiler = g++"
	@echo "make osx-x86_64     >  x86-Mac OS X 64 bit. Compiler = g++"
	@echo "make osx-icc32      >  x86-Mac OS X 32 bit. Compiler = icpc"
	@echo "make osx-icc64      >  x86-Mac OS X 64 bit. Compiler = icpc"
	@echo "make osx-icc32-profile > OSX 32 bit. Compiler = icpc + automatic pgo-build"
	@echo "make osx-icc64-profile > OSX 64 bit. Compiler = icpc + automatic pgo-build"
	@echo "make strip          >  Strip executable"
	@echo "make clean          >  Clean up"
	@echo ""

all: $(EXE) .depend

clean:
	$(RM) *.o .depend *~ $(EXE)


### Possible targets. You may add your own ones here
gcc:
	$(MAKE) \
	CXX='g++' \
	CXXFLAGS="$(GCCFLAGS)" \
	all

icc:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS)" \
	all

icc-profile-make:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS)" \
	CXXFLAGS+='-prof-gen=srcpos -prof_dir ./profdir' \
	all

icc-profile-use:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS)" \
	CXXFLAGS+='-prof_use -prof_dir ./profdir' \
	all

icc-profile:
	@rm -rf profdir
	@mkdir profdir
	@touch *.cpp *.h
	$(MAKE) icc-profile-make
	@echo ""
	@echo "Running benchmark for pgo-build ..."
	@$(PGOBENCH) > /dev/null
	@echo "Benchmark finished. Build final executable now ..."
	@echo ""
	@touch *.cpp *.h
	$(MAKE) icc-profile-use
	@rm -rf profdir bench.txt

icc-profile-make-with-popcnt:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS) -DUSE_POPCNT" \
	CXXFLAGS+='-prof-gen=srcpos -prof_dir ./profdir' \
	all

icc-profile-use-with-popcnt:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS) -DUSE_POPCNT" \
	CXXFLAGS+='-prof_use -prof_dir ./profdir' \
	all

icc-profile-popcnt:
	@rm -rf profdir
	@mkdir profdir
	@touch *.cpp *.h
	$(MAKE) icc-profile-make
	@echo ""
	@echo "Running benchmark for pgo-build (popcnt disabled)..."
	@$(PGOBENCH) > /dev/null
	@touch *.cpp *.h
	$(MAKE) icc-profile-make-with-popcnt
	@echo ""
	@echo "Running benchmark for pgo-build (popcnt enabled)..."
	@$(PGOBENCH) > /dev/null
	@echo "Benchmarks finished. Build final executable now ..."
	@echo ""
	@touch *.cpp *.h
	$(MAKE) icc-profile-use-with-popcnt
	@rm -rf profdir bench.txt


osx-ppc32:
	$(MAKE) \
	CXX='g++' \
	CXXFLAGS="$(GCCFLAGS)" \
	CXXFLAGS+='-arch ppc' \
	LDFLAGS+='-arch ppc' \
	all

osx-ppc64:
	$(MAKE) \
	CXX='g++' \
	CXXFLAGS="$(GCCFLAGS)" \
	CXXFLAGS+='-arch ppc64' \
	LDFLAGS+='-arch ppc64' \
	all

osx-x86:
	$(MAKE) \
	CXX='g++' \
	CXXFLAGS="$(GCCFLAGS)" \
	CXXFLAGS+='-arch i386' \
	LDFLAGS+='-arch i386' \
	all

osx-x86_64:
	$(MAKE) \
	CXX='g++' \
	CXXFLAGS="$(GCCFLAGS)" \
	CXXFLAGS+='-arch x86_64' \
	LDFLAGS+='-arch x86_64' \
	all
	
osx-icc32:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS-OSX)" \
	CXXFLAGS+='-arch i386' \
	LDFLAGS+='-arch i386' \
	all

osx-icc64:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS-OSX)" \
	CXXFLAGS+='-arch x86_64' \
	LDFLAGS+='-arch x86_64' \
	all

osx-icc32-profile-make:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS-OSX)" \
	CXXFLAGS+='-arch i386' \
	CXXFLAGS+='-prof_gen -prof_dir ./profdir' \
	LDFLAGS+='-arch i386' \
	all

osx-icc32-profile-use:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS-OSX)" \
	CXXFLAGS+='-arch i386' \
	CXXFLAGS+='-prof_use -prof_dir ./profdir' \
	LDFLAGS+='-arch i386' \
	all

osx-icc32-profile:
	@rm -rf profdir
	@mkdir profdir
	@touch *.cpp *.h
	$(MAKE) osx-icc32-profile-make
	@echo ""
	@echo "Running benchmark for pgo-build ..."
	@$(PGOBENCH) > /dev/null
	@echo "Benchmark finished. Build final executable now ..."
	@echo ""
	@touch *.cpp *.h
	$(MAKE) osx-icc32-profile-use
	@rm -rf profdir bench.txt

osx-icc64-profile-make:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS-OSX)" \
	CXXFLAGS+='-arch x86_64' \
	CXXFLAGS+='-prof_gen -prof_dir ./profdir' \
	LDFLAGS+='-arch x86_64' \
	all

osx-icc64-profile-use:
	$(MAKE) \
	CXX='icpc' \
	CXXFLAGS="$(ICCFLAGS-OSX)" \
	CXXFLAGS+='-arch x86_64' \
	CXXFLAGS+='-prof_use -prof_dir ./profdir' \
	LDFLAGS+='-arch x86_64' \
	all

osx-icc64-profile:
	@rm -rf profdir
	@mkdir profdir
	@touch *.cpp *.h
	$(MAKE) osx-icc64-profile-make
	@echo ""
	@echo "Running benchmark for pgo-build ..."
	@$(PGOBENCH) > /dev/null
	@echo "Benchmark finished. Build final executable now ..."
	@echo ""
	@touch *.cpp *.h
	$(MAKE) osx-icc64-profile-use
	@rm -rf profdir bench.txt



strip:
	strip $(EXE)


### Compilation. Do not change
$(EXE): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)


### Dependencies. Do not change
.depend:
	$(CXX) -msse -MM $(OBJS:.o=.cpp) > $@

include .depend
