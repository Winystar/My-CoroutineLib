#ifndef _FIBER_H_
#define _FIBER_H_

#include <iostream>
#include <memory>
#include <atomic>
#include <functional>   
#include <cassert>      
#include <ucontext.h>   
#include <unistd.h>
#include <mutex>

namespace john {

/*std::enable_shared_from_this 提供了一个成员函数 shared_from_this()，
通过它获取当前对象的 std::shared_ptr。*/
class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    enum State {
        READY,
        RUNING,
        TERM
    };

private:
    //私有构造函数，仅由getThis()调用，实现单例模式，用于创建主协程
    Fiber();

public:
    //私有无参构造确保主协程的唯一性，公共有参构造灵活创建子协程
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
    ~Fiber();

    //重复使用协程
    void reset(std::function<void()> cb);

    //协程恢复执行
    void resume();
    //协程让出执行权
    void yield();

    uint64_t getID() const {return m_id;}
    State getState() const {return m_state;}

public:
    //设置当前运行的协程
    static void setThis(Fiber* f);
    //获得当前运行的协程
    static std::shared_ptr<Fiber> getThis();
    //设置调度协程（默认为主协程）
    static void setSchedulerFiber(Fiber* f);
    //获得当前运行协程的ID
    static uint64_t getFiberID();
    //协程函数
    static void fiberFunc();

private:
    uint64_t m_id = 0; //协程ID
    State m_state = READY;//协程状态
    ucontext_t m_ctx; //协程上下文

    uint32_t m_stacksize = 0; //栈大小
    void* m_stack = nullptr; //协程栈指针

    std::function<void()> m_cb; //协程函数

    bool m_runInScheduler; // 是否让出执行权交给调度协程
public:
    std::mutex m_mutex;
};

}

#endif