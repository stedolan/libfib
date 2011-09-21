export PATH:= /home/stephen/llvm/Debug+Asserts+Checks/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games


LLVM_BINDIR := /home/stephen/llvm/Debug+Asserts+Checks/bin


CLANG_BIN = $(LLVM_BINDIR)/clang
ifeq ($(plat),x86_64)
  TARGET = 
  PP = $(TARGET) gcc -E
  GCC_C = $(TARGET) gcc
  GCC = $(TARGET) g++
  CLANG = $(CLANG_BIN)
endif
ifeq ($(plat),ppc64)
  # oh jesus
  TARGET = sh -c 'ssh -qt sdolan@wispa "cd" "'\''/tmp/franny$(CURDIR)'\'';" "$$(shell-quote "$$@")"' ''
  PP = $(TARGET) gcc -E -m64
  GCC_C = $(TARGET) gcc -m64
  GCC = $(TARGET) g++ -m64
  CLANG = $(CLANG_BIN) -fomit-frame-pointer -ccc-host-triple powerpc64-unknown-linux-gnu
endif




ifeq ($(plat),)
.PHONY: allplats
allplats:	
	$(MAKE) plat=x86_64
	$(MAKE) plat=ppc64

%.x86_64:
	$(MAKE) plat=x86_64 $@
%.ppc64:
	$(MAKE) plat=ppc64 $@
%:
	$(MAKE) plat=x86_64 $@.x86_64
	$(MAKE) plat=ppc64 $@.ppc64
endif



.SECONDARY:
.ONESHELL:


%.$(plat).i: %.c
	$(PP) -DNDEBUG $(CDEFINES) $< -o $@

%.$(plat).ii: %.cpp # $(dir %)/*.h
	$(PP) -DNDEBUG $(CDEFINES) $< -o $@


%.$(plat).s: %.$(plat).ii $(CLANG_BIN)
	$(CLANG) $(CFLAGS) $(EXTRA_CFLAGS) -ggdb -S $< -o $@

%.$(plat).s: %.$(plat).i $(CLANG_BIN)
	$(CLANG) $(CFLAGS) $(EXTRA_CFLAGS) -ggdb -S $< -o $@
