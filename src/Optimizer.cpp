#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/BlockSource.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace tps_item_optimizer {

// 全局
static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool debugTaskRunning = false;

static std::unordered_map<ActorUniqueID, std::uint64_t> lastItemTick;
static int           processedThisTick = 0;
static std::uint64_t lastTickId        = 0;
static int           cleanupCounter    = 0;

// 动态参数
static int dynMaxPerTick    = 20;
static int dynCooldownTicks = 2;

// 调试统计
static size_t totalProcessed       = 0;
static size_t totalCooldownSkipped = 0;
static size_t totalThrottleSkipped = 0;
static size_t totalDespawnCleaned  = 0;
static size_t totalExpiredCleaned  = 0;

static ll::io::Logger& getLogger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("TpsItemOptimizer");
    }
    return *log;
}

Config& getConfig() { return config; }

bool loadConfig() {
    auto path   = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    bool loaded = ll::config::loadConfig(config, path);
    if (config.cleanupIntervalTicks < 1)  config.cleanupIntervalTicks = 100;
    if (config.maxExpiredAge        < 1)  config.maxExpiredAge        = 600;
    if (config.initialMapReserve   == 0)  config.initialMapReserve    = 500;
    if (config.maxPerTickStep       < 1)  config.maxPerTickStep       = 1;
    if (config.cooldownTicksStep    < 1)  config.cooldownTicksStep    = 1;
    if (config.targetTickMs         < 1)  config.targetTickMs         = 50;
    return loaded;
}

bool saveConfig() {
    auto path = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

static void resetStats() {
    totalProcessed = totalCooldownSkipped = totalThrottleSkipped = 0;
    totalDespawnCleaned = totalExpiredCleaned = 0;
}

static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t total = totalProcessed + totalCooldownSkipped + totalThrottleSkipped;
                double skipRate = total > 0
                    ? (100.0 * (totalCooldownSkipped + totalThrottleSkipped) / total)
                    : 0.0;
                getLogger().info(
                    "Item stats (5s): dynMaxPerTick={}, dynCooldown={} | "
                    "processed={}, cooldownSkip={}, throttleSkip={}, "
                    "skipRate={:.1f}%, despawnClean={}, expiredClean={}, tracked={}",
                    dynMaxPerTick, dynCooldownTicks,
                    totalProcessed, totalCooldownSkipped, totalThrottleSkipped,
                    skipRate, totalDespawnCleaned, totalExpiredCleaned,
                    lastItemTick.size()
                );
                resetStats();
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

static void stopDebugTask() { debugTaskRunning = false; }

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        getLogger().warn("Failed to load config, using defaults and saving");
        saveConfig();
    }
    lastItemTick.reserve(config.initialMapReserve);
    getLogger().info(
        "Loaded. enabled={}, debug={}, targetTickMs={}",
        config.enabled, config.debug, config.targetTickMs
    );
    return true;
}

bool Optimizer::enable() {
    dynMaxPerTick    = config.maxPerTickStep    * 10;
    dynCooldownTicks = config.cooldownTicksStep * 2;

    if (config.debug) startDebugTask();
    getLogger().info(
        "Enabled. initMaxPerTick={}, initCooldown={}",
        dynMaxPerTick, dynCooldownTicks
    );
    return true;
}

bool Optimizer::disable() {
    stopDebugTask();
    lastItemTick.clear();
    processedThisTick = 0;
    lastTickId        = 0;
    cleanupCounter    = 0;
    resetStats();
    getLogger().info("Disabled");
    return true;
}

} // namespace tps_item_optimizer

// ── Actor::$tick Hook（掉落物过滤）────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ItemActorTickHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$tick,
    bool,
    BlockSource& region
) {
    using namespace tps_item_optimizer;

    if (!config.enabled || !this->isType(ActorType::Item)) {
        return origin(region);
    }

    std::uint64_t currentTick = this->getLevel().getCurrentServerTick().tickID;

    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;

        if (++cleanupCounter >= config.cleanupIntervalTicks) {
            cleanupCounter = 0;
            for (auto it = lastItemTick.begin(); it != lastItemTick.end();) {
                if (currentTick - it->second >
                    static_cast<std::uint64_t>(config.maxExpiredAge))
                {
                    it = lastItemTick.erase(it);
                    ++totalExpiredCleaned;
                } else {
                    ++it;
                }
            }
        }
    }

    if (processedThisTick >= dynMaxPerTick) {
        ++totalThrottleSkipped;
        return true;
    }

    auto [it, inserted] = lastItemTick.emplace(this->getOrCreateUniqueID(), 0);
    if (!inserted &&
        currentTick - it->second <
            static_cast<std::uint64_t>(dynCooldownTicks))
    {
        ++totalCooldownSkipped;
        return true;
    }

    ++processedThisTick;
    bool result = origin(region);
    it->second  = currentTick;
    ++totalProcessed;
    return result;
}

// ── Level::$tick Hook：测耗时动态调整 ────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    using namespace tps_item_optimizer;

    auto tickStart = std::chrono::steady_clock::now();
    origin();

    if (!config.enabled) return;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tickStart
    ).count();

    if (elapsed > config.targetTickMs) {
        dynMaxPerTick    = std::max(8,   dynMaxPerTick    - config.maxPerTickStep);
        dynCooldownTicks = std::min(10,  dynCooldownTicks + config.cooldownTicksStep);
    } else {
        dynMaxPerTick    = std::min(200, dynMaxPerTick    + config.maxPerTickStep);
        dynCooldownTicks = std::max(1,   dynCooldownTicks - config.cooldownTicksStep);
    }
}

// ── 清理 Hook ─────────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    using namespace tps_item_optimizer;
    if (config.enabled) {
        if (lastItemTick.erase(this->getOrCreateUniqueID()) > 0)
            ++totalDespawnCleaned;
    }
    origin();
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$remove,
    void
) {
    using namespace tps_item_optimizer;
    if (config.enabled) {
        if (lastItemTick.erase(this->getOrCreateUniqueID()) > 0)
            ++totalDespawnCleaned;
    }
    origin();
}

LL_REGISTER_MOD(tps_item_optimizer::Optimizer, tps_item_optimizer::Optimizer::getInstance());
