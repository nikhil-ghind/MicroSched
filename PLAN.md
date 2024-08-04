# Micro Sched

## Project Overview

A preemptive, priority-based task scheduler written in C++ targeting bare-metal embedded microcontrollers (ARM Cortex-M class, simulated on Linux via POSIX signals for development). Implements core RTOS primitives: tasks with fixed priorities, round-robin among equal-priority tasks, cooperative-yield, mutex and binary/counting semaphore synchronization, and a memory pool allocator for deterministic, heap-free task-control-block (TCB) allocation. Interrupt-driven I/O is simulated via POSIX `SIGALRM`-based tick interrupts on the development host.

**Key Goals:**
- Schedule up to 32 tasks across 8 priority levels with < 10 µs context-switch overhead (on Linux simulation).
- Provide mutex/semaphore primitives with priority-inheritance to prevent priority inversion.
- Use a fixed-size memory pool for all internal allocations — no `new`/`malloc` after init.
- Validate all scheduling invariants with Google Test.

---

## Tech Stack

| Layer | Tool / Library | Version |
|---|---|---|
| Core language | C++ | C++17 |
| Build system | CMake | 3.22+ |
| Unit testing | Google Test | 1.14.0 |
| Simulation platform | Linux (POSIX threads + signals) | Ubuntu 22.04+ |
| Target hardware model | ARM Cortex-M4 (simulated) | — |
| Compiler | GCC / G++ | 12+ |
| Debugging | GDB, Valgrind | system package |

---

## Architecture Overview

```
+-------------------+     +-------------------+
|   Application     |     |  I/O Interrupt     |
|   Tasks (T1..TN)  |     |  Handlers          |
+--------+----------+     +--------+-----------+
         |                         |
         v                         v
+--------+-------------------------+-----------+
|              Scheduler Core                   |
|  RunQueue (per-priority ready lists)          |
|  ContextSwitcher (saves/restores task stack)  |
|  TickHandler (ISR tick -> preemption check)   |
+--------+----------+-----------+--------------+
         |          |           |
         v          v           v
   [MutexManager] [SemManager] [MemoryPool]
         |          |           |
         +----------+-----------+
                    |
                    v
           [TCB — TaskControlBlock]
           (stack, priority, state, name)
```

**Execution model on Linux dev host:**
- Each task runs in a POSIX thread with signals masked.
- `SIGALRM` fires at configurable tick rate (default 1 kHz), driving the scheduler tick handler.
- Context switch is simulated by `pthread_cond_wait` / `pthread_cond_signal` per TCB.

---

## Phase 1 — Project Scaffold and Build System

**Goal:** Compilable skeleton with CMake, Google Test wired up, and zero warnings.

### Tasks

1. Create directory layout:
   ```
   rtosEmbeddedTaskScheduler/
   ├── CMakeLists.txt
   ├── include/
   │   └── rtos/
   │       ├── task.h
   │       ├── scheduler.h
   │       ├── mutex.h
   │       ├── semaphore.h
   │       ├── memory_pool.h
   │       ├── tick_handler.h
   │       └── types.h
   ├── src/
   │   ├── task.cpp
   │   ├── scheduler.cpp
   │   ├── mutex.cpp
   │   ├── semaphore.cpp
   │   ├── memory_pool.cpp
   │   ├── tick_handler.cpp
   │   └── main.cpp
   ├── tests/
   │   ├── CMakeLists.txt
   │   ├── test_scheduler.cpp
   │   ├── test_mutex.cpp
   │   ├── test_semaphore.cpp
   │   └── test_memory_pool.cpp
   └── examples/
       ├── producer_consumer.cpp
       └── priority_inversion_demo.cpp
   ```

2. **`CMakeLists.txt`** (root):
   - `cmake_minimum_required(VERSION 3.22)`, `project(RTOSScheduler CXX)`, `set(CMAKE_CXX_STANDARD 17)`.
   - Compile flags: `-Wall -Wextra -O2 -pthread`.
   - `FetchContent` for GoogleTest 1.14.0.
   - Library target `rtos_lib` from `src/*.cpp` (excluding `main.cpp`).
   - Executable `rtos_demo` linking `rtos_lib -lpthread`.
   - Add `tests/` subdirectory.

3. **`tests/CMakeLists.txt`**: one executable per test file, link `rtos_lib GTest::gtest_main -lpthread`.

4. Build and verify:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j$(nproc)
   ```

---

## Phase 2 — Core Types and Memory Pool

**Goal:** Define all shared types and implement the fixed-size memory pool.

### Tasks

1. **`include/rtos/types.h`** — define:
   - `using TaskHandle = uint8_t;` (0 = invalid).
   - `enum class TaskState : uint8_t { READY, RUNNING, BLOCKED, SUSPENDED, DELETED };`
   - `using TaskFunc = void(*)(void* arg);`
   - `constexpr uint8_t MAX_TASKS = 32;`
   - `constexpr uint8_t NUM_PRIORITIES = 8;`
   - `constexpr size_t DEFAULT_STACK_SIZE = 4096;`

2. **`include/rtos/memory_pool.h`** — declare `template<typename T, size_t N> class MemoryPool`:
   - Storage: `alignas(T) uint8_t storage_[N * sizeof(T)]` — no dynamic allocation.
   - Free bitmap: `std::bitset<N> free_map_` initialized to all-set.
   - Methods: `T* allocate()`, `void deallocate(T* ptr)`, `size_t available() const`.

3. **`src/memory_pool.cpp`** — implement:
   - `allocate()`: find first set bit in `free_map_`, clear it, return pointer to that slot via placement new.
   - `deallocate()`: compute slot index from pointer arithmetic, call destructor explicitly, set bit in `free_map_`.
   - Both methods must be O(N) worst case but O(1) amortized via a cached `next_free_` hint.

4. **`tests/test_memory_pool.cpp`**:
   - `TEST(MemPool, AllocateAndFreeN)` — allocate N objects, verify all non-null and distinct, free all, verify available() == N.
   - `TEST(MemPool, ExhaustionReturnsNull)` — allocate N+1, verify last is nullptr.
   - `TEST(MemPool, ReuseAfterFree)` — allocate, free one, allocate again; verify pointer is reused.

---

## Phase 3 — Task Control Block and Task API

**Goal:** Implement TCB structure and the public task creation/deletion API.

### Tasks

1. **`include/rtos/task.h`** — declare `class Task`:
   - Private fields: `TaskHandle handle_`, `TaskState state_`, `uint8_t priority_`, `uint8_t base_priority_` (for priority inheritance), `TaskFunc func_`, `void* arg_`, `std::string name_`, `pthread_t thread_`, `pthread_mutex_t cond_mutex_`, `pthread_cond_t run_cond_`, `bool run_signal_`.
   - Static factory: `static Task* create(TaskFunc f, void* arg, uint8_t priority, const char* name, MemoryPool<Task,MAX_TASKS>& pool)`.
   - Methods: `void suspend()`, `void resume()`, `void yield()`, `TaskState state() const`, `uint8_t priority() const`, `void setPriority(uint8_t p)`.

2. **`src/task.cpp`** — implement:
   - `create()`: call `pool.allocate()`, construct Task, spawn POSIX thread running a static trampoline `threadEntry(void* self)`.
   - `threadEntry()`: wait on `run_cond_` until `run_signal_` is true, then call `func_(arg_)` in a loop.
   - `suspend()`: set state to `SUSPENDED`, notify scheduler.
   - `resume()`: set state to `READY`, notify scheduler.
   - `yield()`: voluntarily call back into scheduler's `yield()` entry point.

---

## Phase 4 — Scheduler Core

**Goal:** Implement the run queue, tick-driven preemption, and context switch logic.

### Tasks

1. **`include/rtos/scheduler.h`** — declare `class Scheduler` (singleton via `getInstance()`):
   - Private: `std::array<std::list<Task*>, NUM_PRIORITIES> ready_queues_` — one list per priority.
   - Private: `Task* current_task_` and `Task* idle_task_`.
   - Private: `pthread_mutex_t sched_lock_`.
   - Public: `void addTask(Task* t)`, `void removeTask(Task* t)`, `void start()`, `void yield()`, `void taskBlocked(Task* t)`, `void taskUnblocked(Task* t)`.
   - Private: `Task* selectNext()`, `void contextSwitch(Task* from, Task* to)`.

2. **`src/scheduler.cpp`** — implement:
   - `selectNext()`: iterate priority levels 7 (highest) down to 0; return front of first non-empty ready queue; return idle task if all empty.
   - `contextSwitch(from, to)`: signal `to->run_cond_` (set `run_signal_=true`), then wait on `from->run_cond_` (clear `from->run_signal_`). This suspends `from` and wakes `to`.
   - `start()`: create the idle task (infinite loop doing `sched_yield()`), install tick handler, run the scheduler loop.
   - `yield()`: called by running task; pick next task; if different, perform context switch.

3. **`tests/test_scheduler.cpp`**:
   - `TEST(Scheduler, HighPriorityRunsFirst)` — create low-prio and high-prio tasks each incrementing a counter; run for 100 ticks; verify high-prio ran more.
   - `TEST(Scheduler, RoundRobinEqualPriority)` — create 3 equal-priority tasks; verify each gets scheduled at least once per 3 ticks.
   - `TEST(Scheduler, SuspendedTaskNotScheduled)` — suspend a task; run 50 ticks; verify its counter did not increment.

---

## Phase 5 — Tick Handler and Interrupt Simulation

**Goal:** Drive preemption from a periodic POSIX signal.

### Tasks

1. **`include/rtos/tick_handler.h`** — declare:
   - `void tickHandlerInit(uint32_t tick_rate_hz)` — installs `SIGALRM` handler and starts interval timer.
   - `void tickHandlerStop()` — disables timer, resets signal handler.
   - `uint64_t getTickCount()` — returns monotonically increasing tick counter.

2. **`src/tick_handler.cpp`** — implement:
   - `tickHandlerInit()`: use `struct sigaction` with `SA_RESTART` to install `onTick()`.
   - `onTick()` signal handler: increment `tick_count_`, call `Scheduler::getInstance().yield()` if a higher-priority task is now ready (check `preemption_pending_` flag).
   - Use `setitimer(ITIMER_REAL, ...)` to fire at `1 000 000 / tick_rate_hz` microseconds.
   - Mask `SIGALRM` during critical sections using `sigprocmask`.

---

## Phase 6 — Mutex with Priority Inheritance

**Goal:** Implement a mutex that raises a blocking task's priority to prevent inversion.

### Tasks

1. **`include/rtos/mutex.h`** — declare `class Mutex`:
   - Fields: `Task* owner_`, `uint8_t owner_saved_priority_`, `std::queue<Task*> wait_queue_`, `pthread_mutex_t internal_lock_`.
   - Methods: `void lock()`, `void unlock()`, `bool tryLock()`.

2. **`src/mutex.cpp`** — implement:
   - `lock()`: if unowned, set owner; if owned by another task, push caller onto `wait_queue_`, optionally raise owner's priority to caller's if higher (priority inheritance), call `Scheduler::taskBlocked(caller)`.
   - `unlock()`: restore owner's `base_priority_`, pop first waiter from queue, unblock it, transfer ownership.
   - Protect all operations with `internal_lock_`.

3. **`tests/test_mutex.cpp`**:
   - `TEST(Mutex, BasicLockUnlock)` — two tasks alternately lock/unlock; verify no deadlock after 100 iterations.
   - `TEST(Mutex, PriorityInheritance)` — low-prio task holds mutex; high-prio task blocks on it; verify low-prio task's effective priority raised.
   - `TEST(Mutex, TryLockFailsIfHeld)` — one task holds mutex; another calls `tryLock()`; verify false returned.

---

## Phase 7 — Semaphore

**Goal:** Implement binary and counting semaphores.

### Tasks

1. **`include/rtos/semaphore.h`** — declare `class Semaphore`:
   - Constructor: `Semaphore(int initial_count, int max_count)`.
   - Methods: `void wait()` (P operation — decrement or block), `void signal()` (V operation — increment or unblock).

2. **`src/semaphore.cpp`** — implement:
   - `wait()`: decrement `count_`; if `count_ < 0`, push task to wait queue, call `Scheduler::taskBlocked`.
   - `signal()`: increment `count_`; if `count_ <= 0`, pop a waiter and unblock it.
   - Protect with `pthread_mutex_t internal_lock_`.

3. **`tests/test_semaphore.cpp`**:
   - `TEST(Semaphore, BinaryProducerConsumer)` — producer signals every 10 ticks; consumer waits; verify consumer runs exactly as many times as producer signals.
   - `TEST(Semaphore, CountingCapacityLimit)` — signal a semaphore initialized to max_count; verify it does not exceed max_count.

---

## Phase 8 — Example Applications

**Goal:** Validate the complete system with two realistic demo programs.

### Tasks

1. **`examples/producer_consumer.cpp`**:
   - Create a shared circular buffer of 8 slots protected by a mutex.
   - Producer task (priority 3): writes integers 0–99, signals a semaphore after each write.
   - Consumer task (priority 2): waits on semaphore, reads and prints values.
   - Main: init scheduler, add both tasks, start, join after 100 items.

2. **`examples/priority_inversion_demo.cpp`**:
   - Low (P1), medium (P2), high (P3) tasks.
   - P1 acquires mutex, sleeps; P3 tries to acquire — should inherit P1's priority to P3 level.
   - Verify P2 does not run before P3 gets the mutex.
   - Print timing log proving priority inheritance worked.

3. Build examples:
   ```bash
   cmake --build build --target rtos_demo
   ```

4. Run full test suite:
   ```bash
   cd build && ctest --output-on-failure
   ```
