/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef STDEX_COROUTINE_H_
#define STDEX_COROUTINE_H_

#ifndef STACK_LIMIT
#define STACK_LIMIT (1024*1024)
#endif

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cassert>

#include <string>
#include <vector>
#include <list>
#include <thread>
#include <memory>
#include <future>

using ::std::string;
using ::std::wstring;

#ifdef _MSC_VER
#include <Windows.h>
#else
#if defined(__APPLE__) && defined(__MACH__)
#define _XOPEN_SOURCE
#include <ucontext.h>
#else

#include <ucontext.h>

#endif
#endif

namespace coroutine {

using routine_t = unsigned int;

#ifdef _MSC_VER

struct Routine{
    std::function<void()> func;
    bool finished;
    LPVOID fiber;

    Routine(std::function<void()> f)
        :func(std::move(f))
        ,finished(false)
        ,fiber(nullptr){
    }

    ~Routine(){
        DeleteFiber(fiber);
    }
};

struct Ordinator
{
    std::vector<std::shared_ptr<Routine>> routines;
    std::list<routine_t> indexes;
    routine_t current;
    size_t stack_size;
    LPVOID fiber;

    Ordinator(size_t ss = STACK_LIMIT)
        : current(0)
        , stack_size(ss)
        , fiber(ConvertThreadToFiber(nullptr)){
    }

    ~Ordinator() = default;
};

thread_local static Ordinator ordinator;

inline routine_t create(std::function<void()> f){
    auto routine = std::make_shared<Routine>(std::move(f));

    if (ordinator.indexes.empty()){
        ordinator.routines.push_back(routine);
        return ordinator.routines.size();
    } else {
        routine_t id = ordinator.indexes.front();
        ordinator.indexes.pop_front();
        assert(ordinator.routines[id-1] == nullptr);
        ordinator.routines[id-1] = routine;
        return id;
    }
}

inline void destroy(routine_t id) {
    auto routine = ordinator.routines[id-1];
    assert(routine != nullptr);
    
    ordinator.routines[id-1].reset();
    ordinator.indexes.push_back(id);
}

inline void __stdcall entry(LPVOID lpParameter) {
    auto id = ordinator.current;
    auto routine = ordinator.routines[id-1];
    assert(routine != nullptr);

    routine->func();

    routine->finished = true;
    ordinator.current = 0;

    SwitchToFiber(ordinator.fiber);
}

inline int resume(routine_t id) {
    assert(ordinator.current == 0);

    auto routine = ordinator.routines[id-1];
    if (routine == nullptr)
        return -1;

    if (routine->finished)
        return -2;

    if (routine->fiber == nullptr) {
        routine->fiber = CreateFiber(ordinator.stack_size, entry, 0);
        ordinator.current = id;
        SwitchToFiber(routine->fiber);
    } else {
        ordinator.current = id;
        SwitchToFiber(routine->fiber);
    }

    return 0;
}

inline void yield() {
    routine_t id = ordinator.current;
    auto routine = ordinator.routines[id-1];
    assert(routine != nullptr);

    ordinator.current = 0;
    SwitchToFiber(ordinator.fiber);
}

inline routine_t current() {
    return ordinator.current;
}


template<typename Function, typename ... Args>
decltype(auto) await(Function &&func, Args && ... args){
    auto future = std::async(std::launch::async, func, std::forward<Args>(args)...);
    std::future_status status = future.wait_for(std::chrono::milliseconds(0));

    while (status == std::future_status::timeout)
    {
        if (ordinator.current != 0)
            yield();

        status = future.wait_for(std::chrono::milliseconds(0));
    }
    return future.get();
}

#else

struct Routine {
    std::function<void()> func;
    char *stack;
    bool finished;
    ucontext_t ctx;
    
    Routine(std::function<void()> f)
        : func(std::move(f))
        , stack(nullptr)
        , finished(false) {
    }
    
    ~Routine() {
        delete[] stack;
    }
};

struct Ordinator {
    std::vector<std::shared_ptr<Routine>> routines;
    std::list<routine_t> indexes;
    routine_t current;
    size_t stack_size;
    ucontext_t ctx;
    
    inline Ordinator(size_t ss = STACK_LIMIT)
        : current(0)
        , stack_size(ss) {
    }
    
    ~Ordinator() = default;
};

thread_local static Ordinator ordinator;

inline routine_t create(std::function<void()> f) {
    auto routine = std::make_shared<Routine>(std::move(f));
    if (ordinator.indexes.empty()) {
        ordinator.routines.push_back(routine);
        return ordinator.routines.size();
    } else {
        routine_t id = ordinator.indexes.front();
        ordinator.indexes.pop_front();
        assert(ordinator.routines[id - 1] == nullptr);
        ordinator.routines[id - 1] = routine;
        return id;
    }
}

inline void destroy(routine_t id) {
    auto routine = ordinator.routines[id - 1];
    assert(routine != nullptr);
    
    ordinator.routines[id - 1].reset();
    ordinator.indexes.push_back(id);
}

inline void entry() {
    routine_t id = ordinator.current;
    auto routine = ordinator.routines[id - 1];
    routine->func();
    
    routine->finished = true;
    ordinator.current = 0;
    ordinator.indexes.push_back(id);
}

inline int resume(routine_t id) {
    assert(ordinator.current == 0);
    
    auto routine = ordinator.routines[id - 1];
    if (routine == nullptr) {
        return -1;
    }
    
    if (routine->finished) {
        return -2;
    }
    
    if (routine->stack == nullptr) {
        getcontext(&routine->ctx);
        
        routine->stack = new char[ordinator.stack_size];
        routine->ctx.uc_stack.ss_sp = routine->stack;
        routine->ctx.uc_stack.ss_size = ordinator.stack_size;
        routine->ctx.uc_link = &ordinator.ctx;
        ordinator.current = id;
        
        makecontext(&routine->ctx, reinterpret_cast<void (*)(void)>(entry), 0);
        swapcontext(&ordinator.ctx, &routine->ctx);
    } else {
        ordinator.current = id;
        swapcontext(&ordinator.ctx, &routine->ctx);
    }
    
    return 0;
}

inline void yield() {
    routine_t id = ordinator.current;
    auto routine = ordinator.routines[id - 1];
    assert(routine != nullptr);
    
    char *stack_top = routine->stack + ordinator.stack_size;
    char stack_bottom = 0;
    assert(size_t(stack_top - &stack_bottom) <= ordinator.stack_size);
    
    ordinator.current = 0;
    swapcontext(&routine->ctx, &ordinator.ctx);
}

inline routine_t current() {
    return ordinator.current;
}

template<typename Function, typename ... Args>
decltype(auto) await(Function&& func, Args&& ... args) {
    auto future = std::async(std::launch::async, func, std::forward<Args>(args)...);
    std::future_status status = future.wait_for(std::chrono::milliseconds(0));
    
    while (status == std::future_status::timeout) {
        if (ordinator.current != 0) {
            yield();
        }
        
        status = future.wait_for(std::chrono::milliseconds(0));
    }
    return future.get();
}

#endif

template<typename Type>
class Channel {
public:
    Channel()
        : taker_(0) {
        taker_ = 0;
    }
    
    Channel(routine_t id)
        : taker_(id) {
    }
    
    inline void consume(routine_t id) {
        taker_ = id;
    }
    
    inline void push(const Type& obj) {
        list_.push_back(obj);
        if (taker_ && taker_ != current()) {
            resume(taker_);
        }
    }
    
    inline void push(Type&& obj) {
        list_.push_back(std::move(obj));
        if (taker_ && taker_ != current()) {
            resume(taker_);
        }
    }
    
    inline Type pop() {
        if (!taker_) {
            taker_ = current();
        }
        
        while (list_.empty())
            yield();
        
        Type obj = std::move(list_.front());
        list_.pop_front();
        return std::move(obj);
    }
    
    inline void clear() {
        list_.clear();
    }
    
    inline void touch() {
        if (taker_ && taker_ != current()) {
            resume(taker_);
        }
    }
    
    inline size_t size() {
        return list_.size();
    }
    
    inline bool empty() {
        return list_.empty();
    }

private:
    std::list<Type> list_;
    routine_t taker_;
};

}
#endif //STDEX_COROUTINE_H_
