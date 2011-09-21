include common.mk

fiber2.$(plat): fiber2.$(plat).s sched.$(plat).s io.$(plat).s worker_tls_ppc.cpp
	$(GCC) $^ -o $@ -lpthread -lrt

#	clang++ actortest.cpp -lpthread -o fiber2 -ggdb sched.cpp

llqtest.$(plat): llqtest.$(plat).s
	$(GCC) $^ -o $@
