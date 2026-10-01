.PHONY: all msdos sys tests amiga amiga-tests clean

AMIGA_PROFILES := wb13 wb31 wb32
AMIGA_PROFILE ?=

all: msdos amiga

msdos:
	$(MAKE) -C msdos all

sys:
	$(MAKE) -C msdos sys

tests:
	$(MAKE) -C msdos/tests test
	$(MAKE) -C amiga/tests test

amiga:
ifeq ($(AMIGA_PROFILE),)
	@for profile in $(AMIGA_PROFILES); do \
		$(MAKE) -C amiga profile-artifacts \
			BUILD_DIR=../build/amiga/$$profile \
			AMIGA_WB13=$$(if [ "$$profile" = wb13 ]; then echo 1; else echo 0; fi) \
			NIO_TEST_CRT=$$(if [ "$$profile" = wb13 ]; then echo nix13; else echo clib2; fi) || exit $$?; \
	done
else
	$(MAKE) -C amiga profile-artifacts \
		BUILD_DIR=../build/amiga/$(AMIGA_PROFILE) \
		AMIGA_WB13=$(if $(filter wb13,$(AMIGA_PROFILE)),1,0) \
		NIO_TEST_CRT=$(if $(filter wb13,$(AMIGA_PROFILE)),nix13,clib2)
endif

amiga-tests:
	$(MAKE) -C amiga tests

clean:
	$(MAKE) -C msdos clean
	$(MAKE) -C amiga clean
