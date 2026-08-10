.PHONY: all msdos sys tests amiga amiga-tests clean

all: msdos amiga

msdos:
	$(MAKE) -C msdos all

sys:
	$(MAKE) -C msdos sys

tests:
	$(MAKE) -C msdos/tests test
	$(MAKE) -C amiga/tests test

amiga:
	$(MAKE) -C amiga all

amiga-tests:
	$(MAKE) -C amiga tests

clean:
	$(MAKE) -C msdos clean
	$(MAKE) -C amiga clean
