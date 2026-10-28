#ifndef LSM_SRC_MEMTABLE_ARENA_H_
#define LSM_SRC_MEMTABLE_ARENA_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace lsm {

// Lock-free concurrent Arena backed by an atomic Treiber stack of allocated memory blocks (Section 3.2 & DECISIONS.md).
// Zero mutexes exist in this file. All blocks are freed when the owning MemTable is destroyed.
class Arena {
 public:
  explicit Arena(std::size_t initial_block_size = 16 * 1024)
      : block_size_(initial_block_size == 0 ? 4096 : initial_block_size),
        blocks_head_(nullptr),
        memory_usage_(0) {}

  ~Arena() {
    BlockNode* cur = blocks_head_.load(std::memory_order_acquire);
    while (cur != nullptr) {
      BlockNode* next = cur->next;
      std::free(cur);
      cur = next;
    }
  }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  void* AllocateAligned(std::size_t bytes, std::size_t alignment = 16) {
    if (bytes == 0) bytes = 1;
    if (alignment < alignof(void*)) alignment = alignof(void*);

    const std::size_t header_size = sizeof(BlockNode);
    const std::size_t alloc_bytes = header_size + bytes + alignment;
    void* raw = std::malloc(alloc_bytes);
    if (raw == nullptr) return nullptr;

    BlockNode* node = static_cast<BlockNode*>(raw);
    node->next = blocks_head_.load(std::memory_order_relaxed);
    while (!blocks_head_.compare_exchange_weak(node->next, node,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
    }

    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(raw) + header_size;
    std::size_t pad = (alignment - (addr % alignment)) % alignment;
    void* out = reinterpret_cast<void*>(addr + pad);

    std::size_t aligned_bytes = (bytes + alignment - 1) & ~(alignment - 1);
    memory_usage_.fetch_add(aligned_bytes, std::memory_order_relaxed);
    return out;
  }

  std::size_t MemoryUsage() const noexcept {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  struct BlockNode {
    BlockNode* next;
  };

  std::size_t block_size_;
  std::atomic<BlockNode*> blocks_head_;
  std::atomic<std::size_t> memory_usage_;
};

}  // namespace lsm

#endif  // LSM_SRC_MEMTABLE_ARENA_H_
