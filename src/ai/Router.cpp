#include "ai/Router.h"

#include <algorithm>

namespace aegis::ai {

Router::Router(std::vector<UpstreamConfig> upstreams, int failure_threshold,
               std::chrono::seconds cooldown)
    : failure_threshold_(failure_threshold < 1 ? 1 : failure_threshold), cooldown_(cooldown) {
    upstreams_.reserve(upstreams.size());
    for (auto& config : upstreams) {
        if (config.weight < 1) config.weight = 1;
        if (config.weight > 16) config.weight = 16;
        upstreams_.push_back(State{std::move(config)});
    }
}

bool Router::eligible(const State& state, TimePoint now) const noexcept {
    if (state.consecutive_failures < failure_threshold_) return true;
    return now >= state.unhealthy_until;
}

Result<UpstreamTarget> Router::pick(std::string_view model, int attempt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (attempt < 0) attempt = 0;
    const TimePoint now = steady_now();

    std::vector<std::size_t> slots;
    std::vector<std::size_t> fallback;
    for (std::size_t i = 0; i < upstreams_.size(); ++i) {
        const auto& state = upstreams_[i];
        bool serves = state.config.models.empty();
        for (const auto& served : state.config.models) {
            if (served == model) serves = true;
        }
        if (!serves) continue;

        if (!eligible(state, now)) {
            fallback.push_back(i);  // only used when nothing healthy is left
            continue;
        }
        const int weight = std::clamp(state.config.weight, 1, 16);
        for (int slot = 0; slot < weight; ++slot) slots.push_back(i);
    }

    if (slots.empty()) slots = fallback;
    if (slots.empty()) {
        return fail(errc::kNotFound, "no upstream configured for model '" + std::string(model) + "'");
    }

    // Attempt 0 advances the rotation; later attempts reuse the same base so that
    // `attempt` really walks the candidate list instead of re-rolling it.
    const std::uint64_t base = attempt == 0 ? rotation_++ : (rotation_ == 0 ? 0 : rotation_ - 1);
    const std::size_t start = static_cast<std::size_t>(base % slots.size());

    std::size_t index = slots[start];
    if (attempt > 0) {
        // Failover walks distinct upstreams: the weighted slot list contains the
        // same upstream several times, which must not be retried against itself.
        std::vector<std::size_t> unique;
        unique.reserve(slots.size());
        for (const std::size_t slot : slots) {
            if (std::find(unique.begin(), unique.end(), slot) == unique.end()) {
                unique.push_back(slot);
            }
        }
        if (unique.empty()) unique = slots;

        std::size_t position = 0;
        for (; position < unique.size(); ++position) {
            if (unique[position] == index) break;
        }
        index = unique[(position + static_cast<std::size_t>(attempt)) % unique.size()];
    }

    State& chosen = upstreams_[index];
    ++chosen.picks;
    return UpstreamTarget{chosen.config.name, chosen.config.base_url, chosen.config.api_key};
}

void Router::report_success(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& state : upstreams_) {
        if (state.config.name == name) {
            state.consecutive_failures = 0;
            state.unhealthy_until = TimePoint{};
            return;
        }
    }
}

void Router::report_failure(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& state : upstreams_) {
        if (state.config.name != name) continue;
        ++state.failures;
        ++state.consecutive_failures;
        if (state.consecutive_failures >= failure_threshold_) {
            state.unhealthy_until = steady_now() + cooldown_;
        }
        return;
    }
}

bool Router::has_model(std::string_view model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& state : upstreams_) {
        if (state.config.models.empty()) return true;
        for (const auto& served : state.config.models) {
            if (served == model) return true;
        }
    }
    return false;
}

std::vector<std::string> Router::models() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& state : upstreams_) {
        for (const auto& model : state.config.models) {
            if (std::find(names.begin(), names.end(), model) == names.end()) {
                names.push_back(model);
            }
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<Router::Health> Router::health() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint now = steady_now();
    std::vector<Health> report;
    report.reserve(upstreams_.size());
    for (const auto& state : upstreams_) {
        Health entry;
        entry.name = state.config.name;
        entry.healthy = eligible(state, now);
        entry.consecutive_failures = state.consecutive_failures;
        entry.picks = state.picks;
        entry.failures = state.failures;
        report.push_back(std::move(entry));
    }
    return report;
}

std::size_t Router::upstream_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return upstreams_.size();
}

}  // namespace aegis::ai
