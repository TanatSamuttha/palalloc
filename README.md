# pool-adaptive-linking memory-allocator (palalloc)

**Palalloc** is a pool-based memory allocator implemented in C. It organizes allocated memory into dynamically created **size classes** and uses an intrusive free-list to provide fast allocation and deallocation.

Instead of requiring a fixed set of size classes, Palalloc dynamically creates a new pool whenever a requested size is not currently supported. Existing larger size classes can also be split to satisfy smaller allocation requests.

## Implementation Details

* Memory is organized into separate pools for each supported allocation size.

* Each pool contains blocks of the same size.

* Free blocks are linked together using an intrusive singly linked list. The first bytes of each free block store a pointer to the next free block.

* Free blocks are returned in **Last-In, First-Out (LIFO)** order.

* Palalloc does not pre-create or pre-register size classes during initialization. Size classes are created dynamically when the first allocation of a particular size is requested.

* Each newly created pool contains at least 4096 bytes and normally allocates space for 16 blocks:

  ```cpp
  poolSize = max(4096, size * 16);
  ```

* When a new pool is created, all of its blocks except the block immediately returned to the caller are linked into a free-list.

* Allocation has several possible paths:

  1. **Fast path:** If a free block exists in the requested size class, remove the head of the free-list and return it.
  2. **Split path:** If no free block exists in the requested size class, Palalloc searches larger size classes. If a larger free block is available, it is split into the requested size.
  3. **New-pool path:** If no suitable larger free block exists, Palalloc creates a new pool for the requested size.
  4. **New-size-class path:** If the requested size does not exist among the current size classes, a new size class is inserted and a new pool is created for it.

* Size classes stored in `heads` and `sizeClasses` are kept in ascending order.

* When a new size class is inserted, the existing size-class metadata is shifted to preserve ordering.

* The `pools` array stores pool pointers in pool creation order and is not shifted when a size class is inserted.

* The metadata arrays grow dynamically when their corresponding capacity is exceeded.

* The allocator does not store allocation metadata for individual pointers. Therefore, `pal_free()` requires the allocation size to identify the corresponding size class.

### Minimum Allocation Size

Palalloc does not support allocation sizes smaller than 8 bytes.

Requests smaller than 8 bytes are automatically rounded up to 8 bytes:

```cpp
if (size < 8) size = 8;
```

Therefore:

```cpp
pal_alloc(&pool, 4);
```

allocates an 8-byte block, and `pal_free()` must use the effective size class:

```cpp
pal_free(&pool, ptr, 8);
```

## Free-list splitting

When a requested size class has no available blocks, `pal_split()` iteratively searches for a larger size class by doubling the requested size on each iteration.

For example, if the requested size is 256 bytes and a free 1024-byte block is available:

```text
1024-byte block
      ↓
split
      ↓
256 + 256 + 256 + 256
```

The larger block is divided directly into multiple blocks of the requested size. The first block is returned to the caller, while the remaining blocks are linked together and assigned as the new free-list of the requested size class.

This allows unused blocks from larger size classes to be reused for smaller allocations.

## Initialization

Create a `Palalloc` object using `pal_create()` and initialize it with `pal_init()`:

```cpp
Palalloc pool = pal_create();

pal_init(&pool);
```

`pal_init()` allocates the metadata arrays but does not allocate the actual memory pool yet.

Memory pools are allocated on demand when `pal_alloc()` is called.

## Allocation

Allocate memory using:

```cpp
void* ptr = pal_alloc(&pool, 256);
```

If the requested size class already exists and has a free block, the head of its free-list is returned.

If no free block is available, Palalloc attempts to split a larger size class. If splitting is not possible, a new pool is allocated for the requested size.

Requests smaller than 8 bytes are automatically rounded up to 8 bytes.

The returned memory is obtained from memory allocated by `malloc()`. Palalloc does not explicitly enforce alignment for individual block sizes.

## Deallocation

Free memory using:

```cpp
pal_free(&pool, ptr, 256);
```

The allocation size **must match the effective size class used when the block was allocated**. In particular, allocations smaller than 8 bytes are rounded up to 8 bytes and must be freed using size `8`.

The block is inserted at the head of the corresponding free-list:

```text
Before:

HEAD → A → B → C

free(D)

After:

HEAD → D → A → B → C
```

This provides LIFO reuse, causing recently freed memory to be reused first.

Passing an invalid pointer, an already freed pointer, or an incorrect size results in undefined behavior.

## Dynamic Size Classes

Unlike allocators with a predefined set of size classes, Palalloc creates size classes dynamically.

For example:

```cpp
pal_alloc(&pool, 256);
pal_alloc(&pool, 512);
pal_alloc(&pool, 1024);
```

creates three size classes:

```text
256
512
1024
```

If a new size is requested:

```cpp
pal_alloc(&pool, 300);
```

a new size class is inserted:

```text
256
300
512
1024
```

A new pool is then created specifically for 300-byte blocks.

Therefore, Palalloc does **not** round unsupported allocation sizes to the nearest predefined size class, except that sizes below 8 bytes are rounded up to 8 bytes.

## Pool Management

Each size class is associated with one newly created pool:

```text
Size class 256
└── Pool
    └── Free list

Size class 512
└── Pool
    └── Free list

Size class 1024
└── Pool
    └── Free list
```

The allocator stores three pieces of metadata:

| Metadata         | Description                            |
| :--------------- | :------------------------------------- |
| `pools[i]`       | Beginning address of a memory pool     |
| `heads[i]`       | Head of the free-list for a size class |
| `sizeClasses[i]` | Block size represented by a size class |

`heads` and `sizeClasses` are kept in ascending size-class order. `pools` stores pool pointers in the order in which pools were created.

## Dynamic Metadata Capacity

The metadata arrays are dynamically resized when more capacity is required.

The initial capacities are:

```text
poolCap  = 1
classCap = 1
```

When more capacity is required, the corresponding capacity is increased to the larger of:

```text
current count
current capacity × 2
```

`poolCap` controls the storage for:

* `pools`

`classCap` controls the storage for:

* `heads`
* `sizeClasses`

## Destroying the Allocator

Call:

```cpp
pal_destroy(&pool);
```

to release all memory pools and metadata.

`pal_destroy()`:

1. Frees every memory pool.
2. Frees the `pools` metadata.
3. Frees the `sizeClasses` metadata.
4. Frees the `heads` metadata.
5. Marks the allocator as uninitialized.

After calling `pal_destroy()`, all previously allocated pointers become dangling pointers.

## API Reference

| Function      | Parameters                                          | Description                                                                                                                                        |
| :------------ | :-------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pal_create`  | `void`                                              | Creates a `Palalloc` object in an uninitialized state.                                                                                             |
| `pal_init`    | `Palalloc* poolObject`                              | Initializes the allocator metadata. Memory pools are allocated lazily.                                                                             |
| `pal_alloc`   | `Palalloc* poolObject, uint32_t size`               | Allocates a block of the requested size, rounded up to 8 bytes when necessary. May reuse a free block, split a larger block, or create a new pool. |
| `pal_free`    | `Palalloc* poolObject, void* memory, uint32_t size` | Returns a block to the free-list corresponding to the specified size class.                                                                        |
| `pal_destroy` | `Palalloc* poolObject`                              | Frees all pools and metadata and marks the allocator as uninitialized.                                                                             |

## Utility Functions

The implementation also contains several internal utility functions:

### `pal_clz`

Returns the number of leading zero bits in a 32-bit integer.

It uses:

* `_BitScanReverse` when compiled with MSVC.
* `__builtin_clz` on other compilers.

For zero, it returns `32`.

### `pal_max`

Returns the larger of two `uint32_t` values.

### `pal_nextPow2`

Returns the next power of two greater than or equal to the input.

It can be used to round allocation sizes up to a power of two, which helps maintain consistent **alignment** and simplifies size-class management.

For example:

```text
20 → 32
33 → 64
100 → 128
```

### `pal_findIdx`

(Internal use only)

Searches the size-class array starting from a specified index and returns the index of the requested size class or the end of the array if it is not found.

### `pal_ensurePoolCap`

(Internal use only)

Expands the `heads` and `sizeClasses` arrays when their required capacity exceeds `poolCap`.

### `pal_ensureClassCap`

(Internal use only)

Expands the `pools` array when its required capacity exceeds `classCap`.

### `pal_newPool`

(Internal use only)

Creates a new memory pool for a size class and links the remaining blocks into its free-list.

### `pal_split`

(Internal use only)

Searches larger size classes and splits a larger free block directly into blocks belonging to a smaller size class.

## Important Limitations

* `pal_free()` requires the caller to provide the allocation size.
* Allocations smaller than 8 bytes are automatically rounded up to 8 bytes and must be freed using size `8`.
* The allocator does not track whether a pointer has already been freed.
* Invalid pointers and incorrect sizes result in undefined behavior.
* `pal_alloc()` does not have a `malloc()` fallback. It creates a new pool when the requested allocation cannot be satisfied by an existing free block or a larger size class.
* If the underlying `malloc()` call fails, `pal_alloc()` does not explicitly handle the failure.
* There is no built-in tracking of allocations that are currently in use.
* The allocator is **not thread-safe**. Each `Palalloc` instance should be accessed by only one thread at a time unless external synchronization is provided.
* Pools are allocated using `malloc()`, so the allocator itself depends on the system allocator for obtaining new pools.
* The allocator is designed around exact requested sizes rather than a fixed predefined size-class table.
* Pool memory is not returned to the system when individual blocks are freed. It is released only when `pal_destroy()` is called.

## Example

```cpp
#include <stdio.h>
#include "palalloc.h"

int main()
{
    Palalloc pool = pal_create();

    pal_init(&pool);

    int* a = (int*)pal_alloc(&pool, sizeof(int));
    int* b = (int*)pal_alloc(&pool, sizeof(int));

    *a = 10;
    *b = 20;

    printf("%d %d\n", *a, *b);

    pal_free(&pool, a, 8);
    pal_free(&pool, b, 8);

    pal_destroy(&pool);

    return 0;
}
```

## Design Philosophy

Palalloc prioritizes **simple metadata, fast free-list operations, and reuse of recently freed memory** over the general-purpose flexibility of `malloc`.

The core design is based on three ideas:

* Dynamic Size Classes
* Intrusive LIFO Free Lists
* Adaptive Block Splitting

This makes Palalloc particularly suitable for workloads where allocation sizes are relatively predictable and where fast allocation/deallocation is more important than supporting arbitrary general-purpose allocation patterns.

## Benchmark

Palalloc is benchmarked against the standard `malloc` allocator using the same sequence of allocation and free operations.

### Benchmark Environment

| Configuration    | Value      |
| :--------------- | :--------- |
| **OS**           | Windows 11 |
| **Compiler**     | MinGW-w64  |
| **Architecture** | x86_64     |
| **C++ Standard** | C++20      |
| **Optimization** | `-O3`      |

### Performance

| Allocator    | Average Time | Relative Performance |
| :----------- | -----------: | -------------------: |
| **Palalloc** |    **19.07** |     **2.98× faster** |
| `malloc`     |        59.02 |                1.00× |

> **Result:** Palalloc achieves approximately **2.98× lower average execution time** than `malloc` for this workload.

---

### Workload

The benchmark consists of **1,000,000 allocation and free operations**.

| Metric            |         Count |
| :---------------- | ------------: |
| Total operations  | **1,000,000** |
| Allocations       |   **599,143** |
| Frees             |   **400,857** |
| Peak live objects |   **198,290** |

#### Allocation Distribution

| Allocation Size | Number of Allocations |
| --------------: | --------------------: |
|         8 bytes |               149,991 |
|        16 bytes |               149,848 |
|        32 bytes |               149,601 |
|        64 bytes |               149,703 |
|       **Total** |           **599,143** |

> Benchmark results are workload- and system-dependent. Actual performance may vary depending on CPU, operating system, compiler, optimization level, and system load.
