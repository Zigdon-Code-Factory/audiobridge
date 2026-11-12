#pragma once
#include "update_support.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

class AppUpdater {
public:
    explicit AppUpdater(std::function<void()> restart);
    ~AppUpdater();
    void check();
    void install();
    std::string stateJson() const;
private:
    void state(const std::string& phase, const std::string& message, double progress = 0);
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> closing_{false};
    std::function<void()> restart_;
    std::optional<app_update::Release> release_;
    std::string phase_ = "idle";
    std::string message_ = "Check the official GitHub release for a newer version.";
    double progress_ = 0;
};
