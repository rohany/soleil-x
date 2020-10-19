# Required paths
ifndef LEGION_DIR
  $(error LEGION_DIR is not set)
endif
ifndef SOLEIL_DIR
  $(error SOLEIL_DIR is not set)
endif

# OS-specific options
ifeq ($(shell uname),Darwin)
  DYNLINK_PATH := DYLD_LIBRARY_PATH
else
  DYNLINK_PATH := LD_LIBRARY_PATH
endif

# CUDA options
USE_CUDA ?= 1

# HDF options
export USE_HDF ?= 1
export HDF_HEADER ?= hdf5.h
HDF_LIBNAME ?= hdf5

# C compiler options
CFLAGS += -g -O2 -Wall -Werror -fno-strict-aliasing -I$(LEGION_DIR)/runtime -I$(LEGION_DIR)/bindings/regent
CXXFLAGS += -std=c++11 -g -O2 -Wall -Werror -fno-strict-aliasing -I$(LEGION_DIR)/runtime -I$(LEGION_DIR)/bindings/regent

# Regent options
export INCLUDE_PATH := .
ifdef HDF_ROOT
  export INCLUDE_PATH := $(INCLUDE_PATH);$(HDF_ROOT)/include
  export $(DYNLINK_PATH) := $($(DYNLINK_PATH)):$(HDF_ROOT)/lib
endif
REGENT := $(LEGION_DIR)/language/regent.py
ifeq ($(USE_CUDA), 1)
  REGENT_FLAGS := -fflow 0 -fopenmp 1 -foverride-demand-openmp 1 -finner 1 -fcuda 1 -fcuda-offline 1 -foverride-demand-cuda 1
else
  REGENT_FLAGS := -fflow 0 -fopenmp 1 -foverride-demand-openmp 1 -finner 1 -fcuda 0
endif

# Link flags
ifdef CRAYPE_VERSION
  LINK_FLAGS += -Bdynamic
  LINK_FLAGS += $(CRAY_UGNI_POST_LINK_OPTS) -lugni
  LINK_FLAGS += $(CRAY_UDREG_POST_LINK_OPTS) -ludreg
endif
LINK_FLAGS += -L$(LEGION_DIR)/bindings/regent -lregent
ifdef HDF_ROOT
  LINK_FLAGS += -L$(HDF_ROOT)/lib
endif
ifeq ($(USE_HDF), 1)
  LINK_FLAGS += -l$(HDF_LIBNAME)
endif
LINK_FLAGS += -lm
#LINK_FLAGS += -lautomated_mapper

.PHONY: default all clean

default: soleil_beam.exec

beam: soleil_beam.exec

ghc: soleil_ghc.exec

specified: soleil_specified.exec

standard: soleil_standard.exec

all: soleil.exec soleil_beam.exec soleil_ghc.exec soleil_specified.exec soleil_standard.exec dom_host.exec

clean:
	$(RM) *.exec *.o *-desugared.rg config_schema.h

%-desugared.rg: %.rg
	./desugar.py $< > $@

automated_mapper.o: ../../automated_mapper.cc ../../automated_mapper.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

beam_mapper.o: ../../beam_mapper.cc ../../beam_mapper.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

specified_mapper.o: ../../specified_mapper.cc ../../specified_mapper.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

soleil_ghc.o: soleil_ghc-desugared.rg soleil_mapper_ghc.h config_schema.h hdf_helper.rg dom_ghc-desugared.rg util-desugared.rg
	$(REGENT) soleil_ghc-desugared.rg $(REGENT_FLAGS)

soleil_beam.o: soleil_beam-desugared.rg soleil_mapper_beam.h config_schema.h hdf_helper.rg dom_beam-desugared.rg util-desugared.rg
	$(REGENT) soleil_beam-desugared.rg $(REGENT_FLAGS)

soleil_specified.o: soleil_specified-desugared.rg soleil_mapper_specified.h config_schema.h hdf_helper.rg dom_specified-desugared.rg util-desugared.rg
	$(REGENT) soleil_specified-desugared.rg $(REGENT_FLAGS)

soleil_standard.o: soleil_standard-desugared.rg soleil_mapper.h config_schema.h hdf_helper.rg dom_standard-desugared.rg util-desugared.rg
	$(REGENT) soleil_standard-desugared.rg $(REGENT_FLAGS)

soleil.exec: soleil.o soleil_mapper.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

soleil_ghc.exec: soleil_ghc.o soleil_mapper_ghc.o automated_mapper.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

soleil_beam.exec: soleil_beam.o soleil_mapper_beam.o beam_mapper.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

soleil_specified.exec: soleil_specified.o soleil_mapper_specified.o specified_mapper.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

soleil_standard.exec: soleil_standard.o soleil_mapper.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

soleil.o: soleil-desugared.rg soleil_mapper.h config_schema.h hdf_helper.rg dom-desugared.rg util-desugared.rg
	$(REGENT) soleil-desugared.rg $(REGENT_FLAGS)

dom_host.exec: dom_host.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

dom_ghc_host.exec: dom_ghc_host.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

dom_beam_host.exec: dom_beam_host.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

dom_specified_host.exec: dom_specified_host.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

dom_standard_host.exec: dom_standard_host.o config_schema.o json.o
	$(CXX) -o $@ $^ $(LINK_FLAGS)

dom_host.o: dom_host.rg config_schema.h dom-desugared.rg util-desugared.rg
	$(REGENT) dom_host.rg $(REGENT_FLAGS)

dom_ghc_host.o: dom_ghc_host.rg config_schema.h dom_ghc-desugared.rg util-desugared.rg
	$(REGENT) dom_ghc_host.rg $(REGENT_FLAGS)

dom_beam_host.o: dom_beam_host.rg config_schema.h dom_beam-desugared.rg util-desugared.rg
	$(REGENT) dom_beam_host.rg $(REGENT_FLAGS)

dom_specified_host.o: dom_specified_host.rg config_schema.h dom_specified-desugared.rg util-desugared.rg
	$(REGENT) dom_specified_host.rg $(REGENT_FLAGS)

dom_standard_host.o: dom_standard_host.rg config_schema.h dom_standard-desugared.rg util-desugared.rg
	$(REGENT) dom_standard_host.rg $(REGENT_FLAGS)

soleil_mapper.o: soleil_mapper.cc soleil_mapper.h config_schema.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

soleil_mapper_ghc.o: soleil_mapper_ghc.cc soleil_mapper_ghc.h config_schema.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

soleil_mapper_beam.o: soleil_mapper_beam.cc soleil_mapper_beam.h config_schema.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

soleil_mapper_specified.o: soleil_mapper_specified.cc soleil_mapper_specified.h config_schema.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

soleil_mapper_standard.o: soleil_mapper_standard.cc soleil_mapper_standard.h config_schema.h
	$(CXX) $(CXXFLAGS) -c -o  $@ $<

config_schema.o config_schema.h: process_schema.rg config_schema.lua json.h util-desugared.rg
	$(REGENT) process_schema.rg config_schema.lua $(REGENT_FLAGS)

json.o: json.c json.h
	$(CC) $(CFLAGS) -c -o $@ $<
