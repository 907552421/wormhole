# Makefile for wormhole project
ifneq ("$(wildcard ./config.mk)","")
	config = $(CURDIR)/config.mk
else
	config = $(CURDIR)/make/config.mk
endif

# number of threads
# NT=4

# the directory where deps are installed
DEPS_PATH=$(CURDIR)/deps

ROOTDIR = $(CURDIR)
REPOS = dmlc-core repo/xgboost
# BIN = $(addprefix bin/, xgboost.dmlc kmeans.dmlc linear.dmlc
# CPBIN = xgboost.dmlc kmeans.dmlc

.PHONY: clean all test pull

all: xgboost kmeans linear fm

### repos and deps

# dmlc-core
repo/dmlc-core:
	git clone https://github.com/dmlc/dmlc-core $@
	ln -s repo/dmlc-core/tracker .

repo/dmlc-core/libdmlc.a: | repo/dmlc-core glog
	+	$(MAKE) -C repo/dmlc-core libdmlc.a config=$(config) DEPS_PATH=$(DEPS_PATH)

core: repo/dmlc-core/libdmlc.a

# ps-lite
repo/ps-lite:
	git clone https://github.com/dmlc/ps-lite $@

repo/ps-lite/build/libps.a: | repo/ps-lite deps
	+	$(MAKE) -C repo/ps-lite ps config=$(config) DEPS_PATH=$(DEPS_PATH)

ps-lite: repo/ps-lite/build/libps.a

# rabit
repo/rabit:
	git clone https://github.com/dmlc/rabit $@

repo/rabit/lib/librabit.a:  | repo/rabit
	+	$(MAKE) -C repo/rabit

rabit: repo/rabit/lib/librabit.a

# deps
include make/deps.mk

deps: gflags glog protobuf zmq lz4 cityhash

### toolkits

# xgboost
repo/xgboost:
	git clone https://github.com/dmlc/xgboost $@

repo/xgboost/xgboost: repo/dmlc-core/libdmlc.a | repo/xgboost
	+	$(MAKE) -C repo/xgboost config=$(config)

bin/xgboost.dmlc: repo/xgboost/xgboost
	cp $+ $@

xgboost: bin/xgboost.dmlc

# kmeans
learn/kmeans/kmeans.dmlc: learn/kmeans/kmeans.cc | repo/rabit/lib/librabit.a repo/dmlc-core/libdmlc.a
	+	$(MAKE) -C learn/kmeans kmeans.dmlc

bin/kmeans.dmlc: learn/kmeans/kmeans.dmlc
	cp $+ $@

kmeans: bin/kmeans.dmlc

learn/base/base.a:
	$(MAKE) -C learn/base DEPS_PATH=$(DEPS_PATH)

# linear
learn/linear/build/linear.dmlc: repo/ps-lite/build/libps.a repo/dmlc-core/libdmlc.a learn/base/base.a
	$(MAKE) -C learn/linear config=$(config) DEPS_PATH=$(DEPS_PATH)

bin/linear.dmlc: learn/linear/build/linear.dmlc
	cp $+ $@

linear: bin/linear.dmlc

# FM
learn/factorization_machine/build/fm.dmlc: repo/ps-lite/build/libps.a repo/dmlc-core/libdmlc.a learn/base/base.a
	make -C learn/factorization_machine config=$(config) DEPS_PATH=$(DEPS_PATH)

bin/fm.dmlc: learn/factorization_machine/build/fm.dmlc
	cp $+ $@

fm: bin/fm.dmlc



pull:
	for prefix in $(REPOS); do \
		if [ -d $$prefix ]; then \
			cd $$prefix; git pull; cd $(ROOTDIR); \
		fi \
	done

clean:
	for prefix in $(REPOS); do \
		if [ -d $$prefix ]; then \
			cd $$prefix; make clean; cd $(ROOTDIR); \
		fi \
	done
	rm -rf $(BIN) *~ */*~
