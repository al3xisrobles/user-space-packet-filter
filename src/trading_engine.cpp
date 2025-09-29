#include "trading_engine.h"

#include <chrono>
#include <iostream>
#include <thread>
#ifdef __unix__
#include <sched.h>
#endif
#include <cstring>

namespace {
// TODO: Replace this constant price mean reversion logic to something
//       more dynamic, like inventory driven fitting + vol fitting, etc.
constexpr double PRICE_MEAN = 100.0;

std::string instr_name(int instr_id) {
    switch (instr_id) {
        case 0:
            return "UNDERLYING";
        case 1:
            return "OPTION";
        case 2:
            return "FUTURE";
        default:
            return "UNKNOWN";
    }
}
}  // namespace

TradingEngine::TradingEngine(std::shared_ptr<Ring> ring) : ring_(std::move(ring)) {
}

TradingEngine::~TradingEngine() {
    stop();
}

void TradingEngine::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread(&TradingEngine::thread_main, this);
}

void TradingEngine::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (worker_.joinable()) worker_.join();
}

// Consumer
void TradingEngine::run_once() {
    Tick t;
    while (ring_->pop(t)) {
        std::string name = instr_name(t.instr_id);

        std::cout << "Received tick with name: " << name;
        if (t.px < PRICE_MEAN) {
            std::cout << "[BUY ] " << name << " qty=" << t.qty << " @ " << t.px << "\n";
        } else if (t.px > PRICE_MEAN) {
            std::cout << "[SELL] " << name << " qty=" << t.qty << " @ " << t.px << "\n";
        } else {
            std::cout << "[HOLD] " << name << " qty=" << t.qty << " @ " << t.px << "\n";
        }
    }
}

void TradingEngine::run_loop() {
    running_.store(true, std::memory_order_relaxed);
    thread_main();
}

void TradingEngine::thread_main() {
    while (running_.load(std::memory_order_relaxed)) {
        run_once();
        engine_yield();
    }
}
