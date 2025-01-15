#include "scheduler.h"

/*关键思路：多线程结合多协程。
多线程通过互斥锁取任务，利用线程局部变量让线程各自调用自己的子协程执行任务。
从而实现互不干扰的并发的执行任务。
*/

static bool debug = true;

namespace john {

static thread_local Scheduler* t_scheduler = nullptr;

//返回t_scheduler调度器线程
Scheduler* Scheduler::getThis() {
    return t_scheduler;
}

//设置t_scheduler为当前线程
void Scheduler::setThis() {
    t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name):
m_use_caller(use_caller), m_name(name) {
    assert(threads > 0 && Scheduler::getThis() == nullptr);

    setThis();
    Thread::setName(m_name);

    //use_caller为true表示使用主线程作为工作线程
    if(use_caller) {
        threads--;

        Fiber::getThis(); //创建主协程

        //创建调度协程
        m_scheduler_fiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

        Fiber::setSchedulerFiber(m_scheduler_fiber.get());

        m_main_thread = Thread::getThreadID();
        m_thread_id.push_back(m_main_thread);
    }

    m_main_thread = threads;
    if(debug) std::cout << "Scheduler::Scheduler() success\n";
}

Scheduler::~Scheduler() {
    assert(stopping() == true);

    if(getThis() == this) {
        t_scheduler = nullptr;
    }

    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
}

//初始化线程池，并让线程运行到run函数中
void Scheduler::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_stopping) {
        std::cerr << "Scheduler is stopping" << std::endl;
        return;
    }

    assert(m_threads.empty());

    m_threads.resize(m_thread_count);
    for(size_t i = 0; i < m_thread_count; ++i) {
        //reset函数会销毁之前持有的对象，并将指针指向新的对象。
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_thread_id.push_back(m_threads[i]->getID());
    }

    if(debug) std::cout << "Scheduler::start() success\n";
}

//调度器核心，主线程负责从任务队列中取出任务，并通过协程执行任务
void Scheduler::run() {
    int thread_id = Thread::getThreadID(); //获取当前线程
    if(debug) std::cout << "Scheduler::run() starts in thread: " << thread_id << std::endl;

    //set_hook_enable(true);

    setThis(); //设置调度器对象

    //如果运行新创建的线程或者不是主线程，则需要创建主协程
    if(thread_id != m_main_thread) {
        Fiber::getThis(); //分配线程的主协程和调度协程
    }
    
    //创建空闲协程
    std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
    ScheduleTask task;

    while(true) {
        task.reset();
        bool tickle_me = false; //是否唤醒其他线程进行任务调度

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.begin();

            //1.遍历任务队列
            while(it != m_tasks.end()) {
                //跳过那些指定给其他线程执行的任务
                if(it->thread != -1 && it->thread != thread_id) {
                    it++;
                    tickle_me = true;
                    continue;
                }

                //2.线程取出任务
                assert(it->fiber || it->cb);
                task = *it;
                m_tasks.erase(it);
                m_active_thread_count++;
                break; //取到任务的线程直接break
            }
            tickle_me = tickle_me || (it != m_tasks.end());
        }

        if(tickle_me) {
            tickle();
        }

        //3.协程执行任务
        if(task.fiber) {
            //确保互斥锁仅在这一小段代码中保持有效
            {
                std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
                if(task.fiber->getState() != Fiber::TERM) {
                    task.fiber->resume();
                }
            }
            m_active_thread_count--; //线程完成调度任务后即认为不再活跃，并进入空闲状态
            task.reset();
        } else if(task.cb) {
            //将函数封装成协程进行执行
            std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
            {
                std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
                cb_fiber->resume();
            }
            m_active_thread_count--;
            task.reset();
        //没有任务，则执行空闲协程
        } else {
            if(idle_fiber->getState() == Fiber::TERM) {
                if(debug) std::cout << "Scheduler::run() end in thread: " << thread_id << std::endl;
                break;
            }
            m_idle_thread_count++;
            idle_fiber->resume();
            m_idle_thread_count--;
        }
    }
}
/*
当m_use_caller为true时，主线程和调度线程会作为工作线程，这种情况下运行到start()时，
因为没有创建调度线程，故此时调度任务不会立即执行。对于该情况，该项目中设计在运行到stop()中时，
调度器才在caller线程中运行。故主线程在这种情况下需要执行任务分配、程序退出以及任务调度三个工作。
*/
void Scheduler::stop() {
    if(debug) std::cout << "Scheduler::stop() starts in thread: " << Thread::getThreadID() << std::endl;

    if(stopping()) {
        return;
    } 

    m_stopping = true;

    if(m_use_caller) {
        assert(getThis() == this);
    }else {
        assert(getThis() != this);
    }

    //防止m_scheduler或其他线程永久处于阻塞等待任务的状态中
    for(size_t i = 0; i < m_thread_count; ++i) {
        tickle(); //唤醒空闲线程
    }

    //唤醒可能处于挂起状态而等待任务调度的线程
    if(m_scheduler_fiber) tickle();

    //m_use_caller为true时，从这里开始任务调度
    if(m_scheduler_fiber) {
        m_scheduler_fiber->resume(); //开始任务调度
        if(debug) std::cout << "m_scheduler_fiber ends in thread: " << Thread::getThreadID() <<std::endl;
    }

    std::vector<std::shared_ptr<Thread>> threads;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        //通过swap获取此时的线程加入到threads中，这种方式不会增加线程引用计数，方便通过join方法保持线程正常退出
        threads.swap(m_threads);
    }

    for(auto& t : threads) {
        t->join();
    }
    if(debug) std::cout << "Scheduler::stop() ends in thread: " << Thread::getThreadID() << std::endl;
}

void Scheduler::tickle() {

}

void Scheduler::idle() {
    while(!stopping()) {
        if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::getThreadID() <<std::endl;
        sleep(1); //降低空闲线程在无调度任务时对CPU的占用率，避免空转浪费资源
        Fiber::getThis()->yield();
    }
}

bool Scheduler::stopping() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_active_thread_count == 0;
}


}