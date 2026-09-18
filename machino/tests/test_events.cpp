// EventBus tests: bounded queues, overflow flag, unsubscribe, no blocking.
#include "core/events.hpp"
#include <cstdio>
#include <thread>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define ECHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void run_event_tests() {
    EventBus bus;
    ECHECK(bus.subscribers() == 0);
    auto a = bus.subscribe(3);
    auto b = bus.subscribe(100);
    ECHECK(bus.subscribers() == 2);
    for (int i = 0; i < 10; ++i) bus.publish("telemetry", "{\"i\":" + std::to_string(i) + "}");
    // slow subscriber a: bounded to 3, oldest dropped, flagged
    ECHECK(a->pending() == 3 && a->overflowed());
    Event e; ECHECK(a->pop(e) && e.data == "{\"i\":7}" && e.type == "telemetry");
    // fast subscriber b: everything kept, not flagged
    ECHECK(b->pending() == 10 && !b->overflowed());
    int n = 0; while (b->pop(e)) ++n; ECHECK(n == 10 && !b->pop(e));
    bus.unsubscribe(a);
    ECHECK(bus.subscribers() == 1);
    bus.publish("lifecycle", "{}");
    ECHECK(a->pending() == 2 && b->pending() == 1);          // a no longer receives
    // producer never blocks even with a dead subscriber that never pops
    auto dead = bus.subscribe(4);
    std::thread t([&] { for (int i = 0; i < 10000; ++i) bus.publish("telemetry", "x"); });
    t.join();
    ECHECK(dead->pending() == 4 && dead->overflowed());
    bus.unsubscribe(dead); bus.unsubscribe(b);
    ECHECK(bus.subscribers() == 0);
}
