#include "blocks/parallel_executor.h"

#include <stdexcept>

namespace forevertas::blocks {

ParallelExecutor::~ParallelExecutor() {
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
  ready_.notify_all();
  for (auto &thread : threads_) thread.join();
}

void ParallelExecutor::work(std::size_t index) {
  std::unique_lock<std::mutex> lock(mutex_);
  // A new thread is created while run() holds this mutex, before the next
  // generation is published. It must join that first generation too.
  std::size_t seen = 0;
  for (;;) {
    ready_.wait(lock, [&] { return stopping_ || seen != generation_; });
    if (stopping_) return;
    seen = generation_;
    if (index >= active_) continue;
    const auto task = task_;
    lock.unlock();
    std::exception_ptr error;
    try { task(index); } catch (...) { error = std::current_exception(); }
    lock.lock();
    errors_[index] = error;
    if (--pending_ == 0) done_.notify_one();
  }
}

void ParallelExecutor::run(std::size_t workers, const std::function<void(std::size_t)> &task) {
  if (!workers || workers > 256) throw std::invalid_argument("Invalid worker count.");
  std::unique_lock<std::mutex> lock(mutex_);
  if (pending_) throw std::logic_error("A parallel executor cannot run overlapping batches.");
  while (threads_.size() + 1 < workers) {
    const auto index = threads_.size() + 1;
    threads_.emplace_back([this, index] { work(index); });
  }
  errors_.assign(workers, {});
  task_ = task;
  active_ = workers;
  pending_ = workers - 1;
  ++generation_;
  ready_.notify_all();
  lock.unlock();
  std::exception_ptr error;
  try { task(0); } catch (...) { error = std::current_exception(); }
  lock.lock();
  done_.wait(lock, [&] { return pending_ == 0; });
  task_ = {};
  active_ = 0;
  errors_[0] = error;
  for (auto failure : errors_) if (failure) std::rethrow_exception(failure);
}

} // namespace forevertas::blocks
