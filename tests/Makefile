
profile = no

ifeq ($(profile),yes)
PROF_FLAGS = -pg
endif

CCFLAGS = -g -O0 -std=gnu99 $(PROF_FLAGS)

LIB = ../libmimemagic.a
INCLUDE = -I..


run_test: run_test.c $(LIB)
	$(CC) $(CCFLAGS) $(INCLUDE) -o $@ run_test.c $(LIB)

run_libmagic: run_libmagic.c $(LIB)
	$(CC) $(CCFLAGS) $(INCLUDE) -o $@ run_libmagic.c -lmagic


check:  run_test
	@for f in test*; do ./run_test -f $$f; done

perf:   run_test
	@for f in test*; do ./run_test -p -f $$f; done

oldcheck: run_libmagic
	@for f in test*; do ./run_libmagic -f $$f; done

oldperf: run_libmagic
	@for f in test*; do ./run_libmagic -p -f $$f; done

clean:
	$(RM) run_test
