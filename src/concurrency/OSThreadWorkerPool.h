#pragma once

#include "OSThread.h"
#include <functional>
#include <list>

namespace concurrency
{

/**
 * @brief A worker pool that executes lambda-based workers incrementally in round-robin fashion
 * 
 * This allows long-running tasks to be broken up into smaller chunks that execute cooperatively
 * without blocking the main thread. Each worker is a lambda that returns true when complete,
 * or false if it needs more time to continue processing.
 * 
 * Workers can capture state via lambda captures and can spawn new workers during execution.
 * 
 * Example usage:
 * ```cpp
 * auto results = std::make_shared<std::vector<std::string>>();
 * pool->addWorker([results, this]() mutable -> bool {
 *     // Do some work
 *     results->push_back("data");
 *     if (moreWorkToDo) {
 *         return false; // Continue on next cycle
 *     } else {
 *         // Spawn follow-up worker
 *         pool->addWorker([results]() -> bool {
 *             // Process results
 *             return true; // Done
 *         });
 *         return true; // This worker is done
 *     }
 * });
 * ```
 */
class OSThreadWorkerPool : public OSThread
{
  public:
    /**
     * Create a worker pool
     * @param name Thread name for debugging
     * @param period Period in milliseconds between worker executions (default 10ms)
     */
    OSThreadWorkerPool(const char *name, uint32_t period = 10);
    
    virtual ~OSThreadWorkerPool();
    
    /**
     * Add a worker to the pool
     * 
     * @param worker Lambda function that returns true when complete, false to continue
     * 
     * Workers are executed round-robin, one per runOnce() cycle. When a worker returns
     * true, it is removed from the pool. Workers can call addWorker() to spawn new workers.
     */
    void addWorker(std::function<bool()> worker);
    
    /**
     * Get the number of active workers
     */
    size_t getWorkerCount() const { return workers.size(); }
    
    /**
     * Check if the pool has any active workers
     */
    bool hasWorkers() const { return !workers.empty(); }
    
  protected:
    /**
     * Execute one worker per cycle in round-robin fashion
     */
    virtual int32_t runOnce() override;
    
  private:
    std::list<std::function<bool()>> workers;
    bool firstTime = true;
};

} // namespace concurrency

