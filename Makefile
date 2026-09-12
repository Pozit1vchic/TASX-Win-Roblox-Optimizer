CC  = gcc
CXX = g++

CFLAGS   = -std=c11 -O2 -m64 -Wall -Wextra
CXXFLAGS = -std=c++23 -O2 -m64 -static -Wall -Wextra

LDFLAGS = -lwbemuuid -lpsapi -lpowrprof -luser32 -lkernel32 -lole32 -loleaut32 -luuid -lcomctl32 -ladvapi32 -liphlpapi

HEADERS = Infra/CPU.h Infra/WMI.h Infra/trimmer.h Infra/winhook.h \
          Infra/config.h Infra/ntsys.h Infra/tweaks.h Infra/fflags.h \
          Infra/jobs.h Infra/stats.h Infra/audio.h Infra/warm.h \
          Infra/desktop.h Infra/netcache.h Infra/hotkey.h Infra/respawn.h \
          Infra/log.h Infra/lograte.h

OBJS = master.o CPU.o WMI.o trimmer.o winhook.o hotkey.o config.o ntsys.o tweaks.o fflags.o jobs.o stats.o audio.o warm.o desktop.o netcache.o respawn.o
RES  = tasx.res

all: TASX.exe

# Console build for debugging (stdout visible)
TASX.exe: $(OBJS) $(RES)
	$(CXX) $(CXXFLAGS) -DTASX_CONSOLE $(OBJS) $(RES) -o TASX.exe $(LDFLAGS)

tasx.res: tasx.rc tasx.manifest
	windres tasx.rc -O coff -o tasx.res

# Silent GUI-subsystem build shipped with ScheduledTaskInstaller.bat
windows: $(OBJS) $(RES)
	$(CXX) $(CXXFLAGS) -DTASX_GUI -mwindows $(OBJS) $(RES) -o TASX.exe $(LDFLAGS)

master.o: Infra/master.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c Infra/master.cpp -o master.o

CPU.o: Infra/CPU.cc Infra/CPU.h Infra/config.h Infra/ntsys.h
	$(CXX) $(CXXFLAGS) -c Infra/CPU.cc -o CPU.o

WMI.o: Infra/WMI.cc Infra/WMI.h
	$(CXX) $(CXXFLAGS) -c Infra/WMI.cc -o WMI.o

trimmer.o: Infra/trimmer.cc Infra/trimmer.h Infra/config.h Infra/ntsys.h Infra/stats.h
	$(CXX) $(CXXFLAGS) -c Infra/trimmer.cc -o trimmer.o

winhook.o: Infra/winhook.cc Infra/winhook.h
	$(CXX) $(CXXFLAGS) -c Infra/winhook.cc -o winhook.o

hotkey.o: Infra/hotkey.cc Infra/hotkey.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/hotkey.cc -o hotkey.o

jobs.o: Infra/jobs.cc Infra/jobs.h Infra/CPU.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/jobs.cc -o jobs.o

stats.o: Infra/stats.cc Infra/stats.h Infra/ntsys.h
	$(CXX) $(CXXFLAGS) -c Infra/stats.cc -o stats.o

audio.o: Infra/audio.cc Infra/audio.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/audio.cc -o audio.o

warm.o: Infra/warm.cc Infra/warm.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/warm.cc -o warm.o

desktop.o: Infra/desktop.cc Infra/desktop.h
	$(CXX) $(CXXFLAGS) -c Infra/desktop.cc -o desktop.o

netcache.o: Infra/netcache.cc Infra/netcache.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/netcache.cc -o netcache.o

respawn.o: Infra/respawn.cc Infra/respawn.h Infra/config.h Infra/ntsys.h
	$(CXX) $(CXXFLAGS) -c Infra/respawn.cc -o respawn.o

# The low-level core is real C: compiled by gcc, linked into the C++ binary
config.o: Infra/config.c Infra/config.h
	$(CC) $(CFLAGS) -c Infra/config.c -o config.o

ntsys.o: Infra/ntsys.c Infra/ntsys.h
	$(CC) $(CFLAGS) -c Infra/ntsys.c -o ntsys.o

tweaks.o: Infra/tweaks.cc Infra/tweaks.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/tweaks.cc -o tweaks.o

fflags.o: Infra/fflags.cc Infra/fflags.h Infra/config.h
	$(CXX) $(CXXFLAGS) -c Infra/fflags.cc -o fflags.o

clean:
	@echo [clean] removing build outputs (works under cmd.exe and sh)
	-@rm -f TASX.exe *.o *.res
	-@del /q TASX.exe *.o *.res
	-@rm -rf tests/.build
	-@rmdir /s /q tests/.build

# --- Unit tests (self-contained, no external dependencies) ------------

# tests/.build is created lazily. The leading '-' plus the missing '-p' make
# this line valid under BOTH cmd.exe (mingw32-make's default shell) and an
# MSYS shell: `mkdir -p` was the old recipe and it failed on Windows with
# "The syntax of the command is incorrect." so `make test` never built.
tests/.build:
	-@mkdir "tests/.build"

test: test_config test_respawn
	@echo == All tests passed ==

test_config: tests/.build/test_config.exe
	@tests/.build/test_config.exe

test_respawn: tests/.build/test_respawn.exe
	@tests/.build/test_respawn.exe

tests/.build/test_config.exe: tests/run_tests.c tests/test_config.c Infra/config.c Infra/config.h | tests/.build
	$(CC) $(CFLAGS) -IInfra tests/run_tests.c tests/test_config.c Infra/config.c -o $@

tests/.build/test_respawn.exe: tests/test_respawn.cpp Infra/respawn.cc Infra/respawn.h Infra/config.c Infra/config.h Infra/ntsys.c Infra/ntsys.h | tests/.build
	$(CC) $(CFLAGS) -IInfra -c Infra/config.c -o tests/.build/config.o
	$(CC) $(CFLAGS) -IInfra -c Infra/ntsys.c -o tests/.build/ntsys.o
	$(CXX) $(CXXFLAGS) -IInfra -c Infra/respawn.cc -o tests/.build/respawn.o
	$(CXX) $(CXXFLAGS) -IInfra tests/test_respawn.cpp tests/.build/respawn.o tests/.build/config.o tests/.build/ntsys.o -o $@ $(LDFLAGS)
