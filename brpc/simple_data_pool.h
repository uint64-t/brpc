// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2015 Baidu.com, Inc. All Rights Reserved

// Author: The baidu-rpc authors (pbrpc@baidu.com)
// Date: Tue Oct 13 18:44:28 CST 2015

#ifndef BRPC_SIMPLE_DATA_POOL_H
#define BRPC_SIMPLE_DATA_POOL_H

#include <pthread.h>
#include "brpc/data_factory.h"


namespace brpc {

// As the name says, this is a simple unbounded dynamic-size pool for
// reusing void* data. We're assuming that data consumes considerable
// memory and should be reused as much as possible, thus unlike the
// multi-threaded allocator caching objects thread-locally, we just
// put everything in a global list to maximize sharing. It's currently
// used by Server to reuse session-local data. 
class SimpleDataPool {
public:
    struct Stat {
        unsigned nfree;
        unsigned ncreated;
    };

    explicit SimpleDataPool(const DataFactory* factory);
    ~SimpleDataPool();
    void Reset(const DataFactory* factory);
    void Reserve(unsigned n);
    void* Borrow();
    void Return(void*);
    Stat stat() const;
    
private:
    pthread_mutex_t _mutex;
    unsigned _capacity;
    unsigned _size;
    base::atomic<unsigned> _ncreated;
    void** _pool;
    const DataFactory* _factory;
};

inline SimpleDataPool::SimpleDataPool(const DataFactory* factory)
    : _capacity(0)
    , _size(0)
    , _ncreated(0)
    , _pool(NULL)
    , _factory(factory) {
    pthread_mutex_init(&_mutex, NULL);
}

inline SimpleDataPool::~SimpleDataPool() {
    Reset(NULL);
    pthread_mutex_destroy(&_mutex);
}

inline void SimpleDataPool::Reset(const DataFactory* factory) {
    unsigned saved_size = 0;
    void** saved_pool = NULL;
    const DataFactory* saved_factory = NULL;
    {
        BAIDU_SCOPED_LOCK(_mutex);
        saved_size = _size;
        saved_pool = _pool;
        saved_factory = _factory;
        _capacity = 0;
        _size = 0;
        _ncreated.store(0, base::memory_order_relaxed);
        _pool = NULL;
        _factory = factory;
    }
    if (saved_pool) {
        if (saved_factory) {
            for (unsigned i = 0; i < saved_size; ++i) {
                saved_factory->DestroyData(saved_pool[i]);
            }
        }
        free(saved_pool);
    }
}

inline void SimpleDataPool::Reserve(unsigned n) {
    if (_capacity >= n) {
        return;
    }
    BAIDU_SCOPED_LOCK(_mutex);
    if (_capacity >= n) {
        return;
    }
    // Resize.
    const unsigned new_cap = std::max(_capacity * 3 / 2, n);
    void** new_pool = (void**)malloc(new_cap * sizeof(void*));
    if (NULL == new_pool) {
        return;
    }
    if (_pool) {
        memcpy(new_pool, _pool, _capacity * sizeof(void*));
        free(_pool);
    }
    unsigned i = _capacity;
    _capacity = new_cap;
    _pool = new_pool;

    for (; i < n; ++i) {
        void* data = _factory->CreateData();
        if (data == NULL) {
            break;
        }
        _ncreated.fetch_add(1,  base::memory_order_relaxed);
        _pool[_size++] = data;
    }
}

inline void* SimpleDataPool::Borrow() {
    if (_size) {
        BAIDU_SCOPED_LOCK(_mutex);
        if (_size) {
            return _pool[--_size];
        }
    }
    void* data = _factory->CreateData();
    if (data) {
        _ncreated.fetch_add(1,  base::memory_order_relaxed);
    }
    return data;
}

inline void SimpleDataPool::Return(void* data) {
    if (data == NULL) {
        return;
    }
    pthread_mutex_lock(&_mutex);
    if (_capacity == _size) {
        const unsigned new_cap = (_capacity == 0 ? 128 : (_capacity * 3 / 2));
        void** new_pool = (void**)malloc(new_cap * sizeof(void*));
        if (NULL == new_pool) {
            pthread_mutex_unlock(&_mutex);
            return _factory->DestroyData(data);
        }
        if (_pool) {
            memcpy(new_pool, _pool, _capacity * sizeof(void*));
            free(_pool);
        }
        _capacity = new_cap;
        _pool = new_pool;
    }
    _pool[_size++] = data;
    pthread_mutex_unlock(&_mutex);
}

inline SimpleDataPool::Stat SimpleDataPool::stat() const {
    Stat s = { _size, _ncreated.load(base::memory_order_relaxed) };
    return s;
}

} // namespace brpc


#endif  // BRPC_SIMPLE_DATA_POOL_H