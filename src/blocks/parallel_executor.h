#ifndef FOREVERTAS_BLOCKS_PARALLEL_EXECUTOR_H
#define FOREVERTAS_BLOCKS_PARALLEL_EXECUTOR_H

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace forevertas::blocks {

// One bounded executor per running program. Nested maps stay on their current
// worker; a new batch reuses these threads instead of creating another pool.
class ParallelExecutor {
public:
  ParallelExecutor() = default;
  ParallelExecutor(const ParallelExecutor &) = delete;
  ParallelExecutor &operator=(const ParallelExecutor &) = delete;
  ~ParallelExecutor();
  void run(std::size_t workers, const std::function<void(std::size_t)> &task);

private:
  void work(std::size_t index);
  std::vector<std::thread> threads_;
  std::vector<std::exception_ptr> errors_;
  std::mutex mutex_;
  std::condition_variable ready_, done_;
  std::function<void(std::size_t)> task_;
  std::size_t generation_ = 0, active_ = 0, pending_ = 0;
  bool stopping_ = false;
};

} // namespace forevertas::blocks
#endif
