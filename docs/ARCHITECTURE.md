# System Architecture (`ARCHITECTURE.md`)

## Block Diagram

The architecture is partitioned into three regions: **FOREGROUND (In-Memory, Lock-Free Path)**, **BACKGROUND (Parallel Path)**, and **ON-DISK**.

```mermaid
flowchart TB
  subgraph FG["FOREGROUND (In-Memory, Lock-Free Write Path)"]
    direction TB
    W["1. Synthetic Workload Driver\n(1 to 32 Concurrent Client Threads)"]
    WAL["2a. Write-Ahead Log (Durability)\nAtomic fetch_add Offset + pwrite()"]
    ACT["2b. Active MemTable\n(Lock-Free CAS SkipList + Lock-Free Arena)"]
    IMM["3. Immutable MemTable (Frozen)\nAtomic Pointer Swap (CAS) — Writers Never Block"]
    W --> WAL --> ACT
    ACT -- "Size Limit Reached:\nSingle Atomic CAS Swap" --> IMM
  end

  subgraph BG["BACKGROUND (Parallel Flush & Compaction Path)"]
    direction TB
    FW["4. Flush Worker\nWaits for In-Flight Writers == 0\nWrites L0 SSTable"]
    subgraph CP["5. Parallel Compaction Pool (N Workers + Lock-Free Disjoint Range Registry)"]
      W1["Worker 1:\nDisjoint Key Range"]
      W2["Worker 2:\nDisjoint Key Range"]
      WK["Worker k:\nDisjoint Key Range"]
    end
    VM["6. Versioned Manifest (Immutable Ref-Counted Version Snapshots)\nAtomic Pointer Swap + Append-Only MANIFEST Log"]
    IMM --> FW
    FW --> VM
    W1 & W2 & WK --> VM
  end

  subgraph DISK["ON-DISK (Multi-Level SSTable Hierarchy)"]
    direction TB
    L0["7a. Level 0 SSTables\n(Flushed from Immutable MemTable, Overlapping Key Ranges)"]
    L1["7b. Level 1 SSTables\n(Non-Overlapping Key Ranges)"]
    L2["7c. Level 2 SSTables\n(Non-Overlapping, 10x Capacity)"]
    LN["7d. Level n SSTables\n(Compaction-Merged, Non-Overlapping)"]
    L0 --> L1 --> L2 --> LN
  end

  FW --> L0
  L0 & L1 & L2 -. "Disjoint Range Inputs" .-> CP
  CP ==> L1 & L2 & LN

  style FG fill:#dbeafe,stroke:#1d4ed8,stroke-width:2px
  style BG fill:#dcfce7,stroke:#15803d,stroke-width:2px
  style DISK fill:#fef3c7,stroke:#b45309,stroke-width:2px
Prose DescriptionForeground Path (Lock-Free):Up to 32 concurrent client threads issue Put, Delete, and Get operations.Each write assigns a monotonically increasing sequence number (fetch_add), reserves space in the active WAL segment via atomic fetch_add on the file offset, writes its record with pwrite(), and inserts into the Active MemTable (LockFreeSkipList) using compare-and-swap (CAS) on level pointers.When the Active MemTable reaches memtable_max_size, a single thread wins TryClaimSwap() and atomically swaps active_mem_ via CAS for a fresh MemTable. Losing threads never block.Background Path (Parallel):The background flush worker waits until InFlightWriters() == 0 on the frozen Immutable MemTable and flushes it to a Level 0 SSTable.The parallel compaction pool runs $K$ workers concurrently. Before compacting, each job claims [min_key, max_key] in the lock-free DisjointRangeRegistry via CAS + sequential-consistency conflict detection, guaranteeing two overlapping key ranges never compact simultaneously.Completed flushes and compactions install a new immutable Version snapshot via atomic pointer swap and persist a VersionEdit to the append-only MANIFEST.Read Path:Get(key) searches strictly in order: Active MemTable > Immutable MemTable (newest first) > Level 0 SSTables (newest file first, filtered by Bloom filter) > Level 1 > ... > Level $n$, stopping at the first match.
#### `lsm/docs/DECISIONS.md`
```markdown
# Architectural & Engineering Decisions (`DECISIONS.md`)

1. **Internal Key Ordering (`include/lsm/types.h`)**:
   - `InternalKey` orders by `user_key` ascending (16 bytes), `seq` descending (`uint64_t`), and `type` descending. Because higher sequence numbers sort first, point lookups and compaction iterators always encounter the newest version of a key first.
2. **Lock-Free Arena Reclamation (`src/memtable/arena.h`)**:
   - Nodes are never unlinked from a live SkipList (deletes are `ValueType::kDelete` tombstones). All memory blocks are linked in a lock-free Treiber stack and freed in bulk when the owning `MemTable`'s reference count drops to zero.
3. **In-Flight Writer Tracking & Backpressure (`src/db/db_impl.cpp`, `src/db/flush.cpp`)**:
   - Writers call `BeginWrite()` / `EndWrite()` (atomic `fetch_add` / `fetch_sub`) around MemTable mutations. The flush worker—never the foreground writers—waits for `InFlightWriters() == 0` before writing the L0 SSTable.
   - To prevent unbounded memory growth if disk I/O falls behind, `Options::max_immutable_memtables` (default 4) throttles incoming writers when exceeded and records every stall in `DBStats::write_stall_count`.
4. **Lock-Free WAL Append (`src/wal/wal_writer.cpp`)**:
   - Writers reserve disjoint byte ranges in the WAL file via `log_offset_.fetch_add(record_size, std::memory_order_relaxed)` and write via POSIX `pwrite()`, eliminating any mutex on the WAL append path.
5. **Lock-Free Disjoint Compaction Range Registry (`src/compaction/compaction_picker.cpp`)**:
   - Uses a 64-slot atomic state machine (`kFree -> kInitializing -> kActive`) with a `std::memory_order_seq_cst` fence between writing candidate bounds and scanning all other slots, preventing symmetric Dekker races without a mutex.
