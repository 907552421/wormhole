#!/bin/bash

# make -C ../.. dmlc-core
# make -C ../.. repo/ps-lite
# ../../repo/ps-lite/make/install_deps.sh

# set the config.mk
cd ../../
deps_path=`pwd`/repo/ps-lite/deps
cat <<< "# sample config for learn/linear
CC = gcc
CXX = g++
USE_GLOG = 1
DEPS_PATH = $deps_path
STATIC_DEPS = 1
USE_HDFS = 0
USE_S3 = 0
USE_KEY32 = 1" >config.mk
cd learn/linear

make
