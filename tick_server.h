#ifndef TICK_SERVER_H
#define TICK_SERVER_H

#include "tick_processor.h"
#include <atomic>

extern TickQueue* g_tick_queue;
extern TickState* g_tick_state;
extern std::atomic<int> g_tick_step_id;

#endif
