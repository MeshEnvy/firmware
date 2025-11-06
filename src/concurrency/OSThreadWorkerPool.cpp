#include "OSThreadWorkerPool.h"
#include "configuration.h"

namespace concurrency
{

OSThreadWorkerPool::OSThreadWorkerPool(const char *name, uint32_t period) : OSThread(name, period)
{
    LOG_DEBUG("OSThreadWorkerPool %s created", name);
}

OSThreadWorkerPool::~OSThreadWorkerPool()
{
    workers.clear();
}

void OSThreadWorkerPool::addWorker(std::function<bool()> worker)
{
    if (!worker) {
        LOG_WARN("Attempted to add null worker to pool");
        return;
    }

    workers.push_back(worker);

    // Wake up the thread to process the new worker
    enabled = true;
    setInterval(0);

    LOG_DEBUG("Worker added to pool, total workers: %d", workers.size());
}

int32_t OSThreadWorkerPool::runOnce()
{
    if (firstTime) {
        firstTime = false;
        LOG_INFO("OSThreadWorkerPool %s initialized", ThreadName.c_str());
        // If no workers yet, go back to sleep
        if (workers.empty()) {
            return disable();
        }
    }

    // If no workers, disable until one is added
    if (workers.empty()) {
        LOG_DEBUG("No workers in pool, disabling");
        return disable();
    }

    // Execute one worker in round-robin fashion
    // Get the front worker
    auto worker = workers.front();
    workers.pop_front();

    // Execute it
    bool isComplete = worker();

    // If not complete, add it back to the end of the queue
    if (!isComplete) {
        workers.push_back(worker);
    } else {
        LOG_DEBUG("Worker completed, remaining workers: %d", workers.size());
    }

    // If we still have workers, run again soon
    // Otherwise disable until a new worker is added
    if (workers.empty()) {
        return disable();
    } else {
        // Continue processing workers at our configured period
        return RUN_SAME;
    }
}

} // namespace concurrency
