.PHONY: all sys tests clean

all sys:
	$(MAKE) -C msdos $@

tests:
	$(MAKE) -C msdos/tests test

clean:
	$(MAKE) -C msdos clean
