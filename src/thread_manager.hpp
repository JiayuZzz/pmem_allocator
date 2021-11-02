#include <unordered_set>

class ThreadManager;

struct Thread {
public:
    Thread() : id(-1), thread_manager(nullptr) {}

    ~Thread();

    void Release();

    int id;
    std::shared_ptr<ThreadManager> thread_manager;
};

class ThreadManager : public std::enable_shared_from_this<ThreadManager> {
public:
    ThreadManager(uint32_t max_threads) : max_threads_(max_threads), ids_(0) {}

    bool MaybeInitThread(Thread &t) {
        if (t.id < 0) {
            if (!usable_id_.empty()) {
                std::lock_guard<SpinMutex> lg(spin_);
                if (!usable_id_.empty()) {
                    auto it = usable_id_.begin();
                    t.id = *it;
                    usable_id_.erase(it);
                    t.thread_manager = shared_from_this();
                    return true;
                }
            }
            int id = ids_.fetch_add(1, std::memory_order_relaxed);
            if (id >= max_threads_) {
                return false;
            }
            t.id = id;
            t.thread_manager = shared_from_this();
        }
        return true;
    }

    void Release(const Thread &t) {
        std::lock_guard<SpinMutex> lg(spin_);
        usable_id_.insert(t.id);
    }

private:
    std::atomic<uint32_t> ids_;
    std::unordered_set<uint32_t> usable_id_;
    uint32_t max_threads_;
    SpinMutex spin_;
};

extern thread_local Thread access_thread;