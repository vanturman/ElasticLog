TARGET=lib/libelasticlog.a
CXX=g++
CFLAGS=-g -O2 -Wall -fPIC
SRC=src
INC=-Isrc
OBJS = $(addsuffix .o, $(basename $(wildcard $(SRC)/*.cc)))

$(TARGET): $(OBJS)
	-mkdir -p lib
	ar cqs $@ $^

%.o: %.cc
	$(CXX) $(CFLAGS) -c -o $@ $< $(INC)

.PHONY: clean

clean:
	-rm -f src/*.o $(TARGET)
