# 在 demo 目录下执行 make，会链接上级目录的 tick_processor 与 singlethreadpool
# demo_tick：多生产者压测；demo_sin：方案 A，sin 信号 + 累加，与预期比较验证
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -I..

.PHONY: all clean

all: demo_tick demo_sin

demo_tick: main.cpp ../tick_processor.cpp ../singlethreadpool.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

demo_sin: main_sin.cpp ../tick_processor.cpp ../singlethreadpool.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f demo_tick demo_sin
