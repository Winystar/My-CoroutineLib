#include "fiber.h"

static bool debug = false;

namespace john {

 //正在运行的协程
 //thread_local存储每个线程中正在运行的协程,每个线程有独立的数据副本
static thread_local Fiber* t_fiber = nullptr;
//线程中的主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
//线程中的调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

//协程id 原子操作保证无锁的情况下实现线程安全
static std::atomic<uint64_t> s_fiber_id{0};
//协程计数器
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::setThis(Fiber* f) {
    t_fiber = f;
}

//首先运行getThis()函数用于创建主协程
std::shared_ptr<Fiber> Fiber::getThis() {
    if(t_fiber) {
        return t_fiber->shared_from_this();
    }

    std::shared_ptr<Fiber> main_fiber(new Fiber());
    t_thread_fiber = main_fiber;
    t_scheduler_fiber = main_fiber.get(); //除非主动设置，主协程默认是调度协程

    assert(t_fiber == main_fiber.get()); //一开始线程中的协程就是主协程
    return t_fiber->shared_from_this();
}

void Fiber::setSchedulerFiber(Fiber* f) {
    t_scheduler_fiber = f;
}

uint64_t Fiber::getFiberID() {
    if(t_fiber) {
        return t_fiber->getID();
    }
    return (uint64_t)-1;
}

Fiber::Fiber() {
    setThis(this);
    m_state = RUNING;

    if(getcontext(&m_ctx)) {
        std::cerr << "FIber() failed\n";
        pthread_exit(NULL);
    }

    m_id = s_fiber_id++;
    s_fiber_count++;
    if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler) {
    m_state = READY;
    
    m_stacksize = stacksize ? stacksize : 128000;
    m_stack = malloc(m_stacksize);

    //获取上下文
    if(getcontext(&m_ctx)) {
        std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
        pthread_exit(NULL);
    }
    //设置上下文
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    //将上下文指向协程函数，完成协程创建（函数+函数状态）
    makecontext(&m_ctx, &Fiber::fiberFunc, 0);

    m_id = s_fiber_id++;
    s_fiber_count++;
    if(debug) std::cout << "Fiber() child id = " << m_id << std::endl;
}

Fiber::~Fiber() {
    s_fiber_count--;
    if(m_stack) {
        free(m_stack);
    }
    if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;
}

void Fiber::reset(std::function<void()> cb) {
    assert(m_stack != nullptr && m_state == TERM);

    m_state = READY;
    m_cb = cb;

    if(getcontext(&m_ctx)) {
        std::cerr << "reset failed\n";
        pthread_exit(NULL);
    }

    m_ctx.uc_link = nullptr;
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::fiberFunc, 0);
}

void Fiber::resume() {
    assert(m_state == READY);

    m_state = RUNING;

    if(m_runInScheduler) {
        setThis(this);
        if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx)) {
            std::cerr << "resume to t_scheduler failed\n";
            pthread_exit(NULL);
        }
    } else {
        setThis(this);
        if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
            std::cerr << "resume to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

void Fiber::yield() {
    assert(m_state == RUNING || m_state == TERM);

    if(m_state != TERM) {
        m_state= READY;
    }

    if(m_runInScheduler) {
        //调度协程默认是主协程，已通过getThis创建了，故不需要get()方法
        setThis(t_scheduler_fiber);
        if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx))) {
            std::cerr << "yield to t_scheduler failed\n";
            pthread_exit(NULL);
        }
    } else {
        setThis(t_thread_fiber.get());
        if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            std::cerr << "yield to t_thread_fiber failed\n";
            pthread_exit(NULL);
        }
    }
}

void Fiber::fiberFunc() {
    std::shared_ptr<Fiber> cur = getThis();
    assert(cur != nullptr);

    //开始运行函数
    cur->m_cb();
    cur->m_cb = nullptr;
    cur->m_state = TERM;

    //运行结束，让出执行权
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield();
}

}