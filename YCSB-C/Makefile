CC=g++
CFLAGS=-std=c++14 -g -Wall -pthread -I./ -march=native
LDFLAGS= -lpthread -ltbb -lhiredis
SUBDIRS=core db redis
SUBSRCS=$(wildcard core/*.cc) $(wildcard db/*.cc)
OBJECTS=$(SUBSRCS:.cc=.o)

WRERT_SRCS=wrert/wrert.cpp wrert/wrert_node.cpp
WRERT_OBJS=$(WRERT_SRCS:.cpp=.o)

ERT_SRCS=ert/ERT_int.cpp ert/ERT_node_int.cpp
ERT_OBJS=$(ERT_SRCS:.cpp=.o)

ROART_SRCS=roart/roart.cpp roart/roart_node.cpp roart/roart_key.cpp
ROART_OBJS=$(ROART_SRCS:.cpp=.o)

FASTALLOC_SRCS=fastalloc/fastalloc.cpp
FASTALLOC_OBJS=$(FASTALLOC_SRCS:.cpp=.o)

RNG_SRCS=$(wildcard rng/*.cpp)
RNG_OBJS=$(RNG_SRCS:.cpp=.o)

EXEC=ycsbc

all: $(SUBDIRS) $(EXEC)

$(SUBDIRS):
	$(MAKE) -C $@

$(EXEC): $(wildcard *.cc) $(OBJECTS) $(WRERT_OBJS) $(ERT_OBJS) $(ROART_OBJS) $(FASTALLOC_OBJS) $(RNG_OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

wrert/%.o: wrert/%.cpp
	$(CC) $(CFLAGS) -c $< -o $@

ert/%.o: ert/%.cpp
	$(CC) $(CFLAGS) -c $< -o $@

roart/%.o: roart/%.cpp
	$(CC) $(CFLAGS) -c $< -o $@

fastalloc/%.o: fastalloc/%.cpp
	$(CC) $(CFLAGS) -c $< -o $@

rng/%.o: rng/%.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $@; \
	done
	find . -name "*.o" -delete
	$(RM) $(EXEC)

.PHONY: $(SUBDIRS) $(EXEC)

