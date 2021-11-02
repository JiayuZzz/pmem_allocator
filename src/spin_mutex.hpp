class SpinMutex {
private:
    std::atomic_flag locked = ATOMIC_FLAG_INIT;
    //  int owner = -1;

public:
    void lock() {
        while (locked.test_and_set(std::memory_order_acquire)) {
            asm volatile("pause");
        }
        //    owner = access_thread.id;
    }

    void unlock() {
        //    owner = -1;
        locked.clear(std::memory_order_release);
    }

    bool try_lock() {
        if (locked.test_and_set(std::memory_order_acquire)) {
            return false;
        }
        //    owner = access_thread.id;
        return true;
    }

    //  bool hold() { return owner == access_thread.id; }

    SpinMutex(const SpinMutex &s) : locked(ATOMIC_FLAG_INIT) {}

    SpinMutex(const SpinMutex &&s) : locked(ATOMIC_FLAG_INIT) {}

    SpinMutex() : locked(ATOMIC_FLAG_INIT) {}
};