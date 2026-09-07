#ifndef PALALLOC_H
#define PALALLOC_H

#include <stdint.h>
#include <stdlib.h>

typedef struct Palalloc
{
    uint8_t** pools;
    uint8_t** heads;
    uint32_t* sizeClasses;
    uint32_t poolCount;
    uint32_t poolCap;
    uint32_t classCount;
    uint32_t classCap;
    bool initialized;
} Palalloc;

Palalloc pal_create ()
{
    Palalloc pool;
    pool.initialized = false;
    return pool;
}

uint32_t pal_clz (uint32_t x)
{
    #if defined(_MSC_VER)
        unsigned long index;
        if (_BitScanReverse(&index, x))
            return 31u - index;
        return 32;
    #else
        if (x) return (uint32_t)__builtin_clz(x);
        return 32;
    #endif
}

uint32_t pal_max (uint32_t a, uint32_t b)
{
    if (a > b) return a;
    return b;
}

uint32_t pal_nextPow2 (uint32_t x)
{
    if ((x & (x - 1)) == 0) return x;
    return 1 << (32 - pal_clz(x));
}

void pal_ensureClassCap (Palalloc* poolObject)
{
    if (poolObject->classCount <= poolObject->classCap)
        return;
        
    poolObject->classCap = pal_max(poolObject->classCount, poolObject->classCap * 2);

    uint8_t** headsBuffer = (uint8_t**)malloc(poolObject->classCap * sizeof(uint8_t*));
    uint32_t* sizeClassesBuffer = (uint32_t*)malloc(poolObject->classCap * sizeof(uint32_t));
    
    for (int i = 0; i < poolObject->classCount - 1; ++i)
    {
        headsBuffer[i] = poolObject->heads[i];
        sizeClassesBuffer[i] = poolObject->sizeClasses[i];
    }

    free(poolObject->heads);
    free(poolObject->sizeClasses);

    poolObject->heads = headsBuffer;
    poolObject->sizeClasses = sizeClassesBuffer;
}

void pal_ensurePoolCap (Palalloc* poolObject)
{
    if (poolObject->poolCount <= poolObject->poolCap)
        return;

    poolObject->poolCap = pal_max(poolObject->poolCount, poolObject->poolCap * 2);
    
    uint8_t** poolsBuffer = (uint8_t**)malloc(poolObject->poolCap * sizeof(uint8_t*));

    for (int i = 0; i < poolObject->poolCount - 1; ++i)
    {
        poolsBuffer[i] = poolObject->pools[i];
    }

    free(poolObject->pools);

    poolObject->pools = poolsBuffer;
}

uint32_t pal_findIdx (Palalloc* poolObject, uint32_t idx, uint32_t size)
{
    for (; idx < poolObject->classCount; ++idx)
    {
        if (size == poolObject->sizeClasses[idx])
            break;
    }

    return idx;
}

void pal_newPool (Palalloc* poolObject, uint8_t** resPtr, uint32_t size, uint32_t idx)
{
    uint32_t poolSize = pal_max(4096, size * 16);
    ++poolObject->poolCount;
    pal_ensurePoolCap(poolObject);
    poolObject->pools[poolObject->poolCount - 1] = (uint8_t*)malloc(poolSize);

    *resPtr = (uint8_t*)poolObject->pools[poolObject->poolCount - 1];
    poolObject->heads[idx] = (uint8_t*)(poolObject->pools[poolObject->poolCount - 1] + size);
    poolObject->sizeClasses[idx] = size;
    
    for (int i = 0; i < poolSize - size; i += size)
    {
        *(uint8_t**)(poolObject->heads[idx] + i) = (uint8_t*)(poolObject->heads[idx] + i + size);
    }

    *(uint8_t**)(poolObject->pools[poolObject->poolCount - 1] + poolSize - size) = NULL;
}

void pal_split (Palalloc* poolObject, uint8_t** resPtr, uint32_t idx, uint32_t size)
{
    *resPtr = NULL;
    int split = 1;
    int nextIdx = idx;
    int nextSize = size;
    while (true)
    {
        split <<= 1;
        nextSize <<= 1;
        nextIdx = pal_findIdx(poolObject, ++nextIdx, nextSize);

        if (nextIdx >= poolObject->classCount)
            return;

        if (poolObject->heads[nextIdx] != NULL)
        {
            *resPtr = poolObject->heads[nextIdx];
            poolObject->heads[idx] = poolObject->heads[nextIdx] + size;
            poolObject->heads[nextIdx] = *(uint8_t**)poolObject->heads[nextIdx];
            int sumPoolCount = 0;
            for (int i = 1; i < split - 1; ++i, sumPoolCount += size)
            {
                *(uint8_t**)(poolObject->heads[idx] + sumPoolCount) = (uint8_t*)(poolObject->heads[idx] + sumPoolCount + size);
            }
            *(uint8_t**)(poolObject->heads[idx] + sumPoolCount) = NULL;
            return;
        }
    }
}

void pal_init (Palalloc* poolObject)
{
    if (poolObject->initialized) return;
    poolObject->poolCap = 1;
    poolObject->poolCount = 0;
    poolObject->classCap = 1;
    poolObject->classCount = 0;
    poolObject->pools = (uint8_t**)malloc(sizeof(uint8_t*));
    poolObject->heads = (uint8_t**)malloc(sizeof(uint8_t*));
    poolObject->sizeClasses = (uint32_t*)malloc(sizeof(uint32_t));
    poolObject->initialized = true;
}

void pal_destroy (Palalloc* poolObject)
{
    if (!poolObject->initialized) return;

    for (int i = 0; i < poolObject->poolCount; ++i)
    {
        free(poolObject->pools[i]);
    }

    free(poolObject->pools);
    free(poolObject->sizeClasses);
    free(poolObject->heads);

    poolObject->initialized = false;
}

void* pal_alloc (Palalloc* poolObject, uint32_t size)
{
    if (size < 8) size = 8;

    uint32_t idx = pal_findIdx(poolObject, 0, size);

    uint8_t *resPtr = NULL;

    if (idx < poolObject->classCount)
    {
        if (size == poolObject->sizeClasses[idx])
        {
            if (poolObject->heads[idx] != NULL)
            {
                resPtr = poolObject->heads[idx];
                poolObject->heads[idx] = *(uint8_t**)poolObject->heads[idx];
            }
            else
            {
                pal_split(poolObject, &resPtr, idx, size);

                if (resPtr == NULL) pal_newPool(poolObject, &resPtr, size, idx);
            }
        }
        else
        {
            ++poolObject->classCount;
            pal_ensureClassCap(poolObject);

            for (int i = poolObject->classCount - 1; i > idx; --i)
            {
                poolObject->heads[i] = poolObject->heads[i - 1];
                poolObject->sizeClasses[i] = poolObject->sizeClasses[i - 1];
            }

            pal_newPool(poolObject, &resPtr, size, idx);
        }
    }
    else
    {
        ++poolObject->classCount;
        pal_ensureClassCap(poolObject);
        
        pal_newPool(poolObject, &resPtr, size, idx);
    }

    return (void*)resPtr;
}

void pal_free (Palalloc* poolObject, void* _memory, uint32_t size)
{
    uint32_t idx = pal_findIdx(poolObject, 0, size);

    *(uint8_t**)_memory = poolObject->heads[idx];
    poolObject->heads[idx] = (uint8_t*)_memory;
}

#endif