#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = false;

namespace john {
IOManager* IOManager::getThis() {
    //运行时安全检查基类需要有虚函数，能够实现函数重载，确保类型转换安全。
    return dynamic_cast<IOManager*>(Scheduler::getThis());
}

IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) {
    assert(event == READ || event == WRITE);
    switch (event) {
    case READ:
        return read;
    case WRITE:
        return write;
    }
    throw std::invalid_argument("unsuported event type");
}

void IOManager::FdContext::resetEventContext(EventContext& ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    assert(events & event); //确保event中有指定的事件，否则中断

    // delete event
    //按位与&，按位取反~，效果是将 events 中和 event 相同的位清零，即为删除该事件
    //清理该事件表示注册IO事件是一次性的，想要持续关注某个事件需要每次触发事件后重新添加
    events = (Event)(events & ~event);
    
    // trigger
    EventContext& ctx = getEventContext(event);
    //相当于取出任务放入任务队列，调度协程完成工作后切换回主协程，再调用run方法执行任务
    if (ctx.cb) 
    {
        // call ScheduleTask(std::function<void()>* f, int thr)
        ctx.scheduler->schedulerLock(&ctx.cb);
    } 
    else 
    {
        // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        ctx.scheduler->schedulerLock(&ctx.fiber);
    }

    // reset event context
    resetEventContext(ctx);
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string& name):
Scheduler(threads, use_caller, name), TimerManager() {
    // create epoll fd
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    // create pipe
    //管道通知机制能够保证epoll在等待 I/O 事件的同时，
    //也能及时响应其他线程或任务的通知，避免了长时间阻塞的情况，从而实现异步通知。
    int rt = pipe(m_tickleFds);
    assert(!rt);

    // add read event to epoll
    epoll_event event;
    event.events  = EPOLLIN | EPOLLET; // Edge Triggered
    event.data.fd = m_tickleFds[0];

    // non-blocked
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    contextResize(32);

    start();
}

IOManager::~IOManager() {
    stop(); //关闭scheduler中的线程池，让任务全部执行完成后线程安全退出
    close(m_epfd); //关闭epoll句柄。句柄就是指向资源的标识，通过句柄来管理和操作资源。
    //epoll句柄是一个整数值，代表一个epoll实例，内核通过这个句柄来管理和监控文件描述符的事件。
    close(m_tickleFds[0]); //关闭管道读端和写段
    close(m_tickleFds[1]);

    //将文件描述符m_FdContext一个个全部关闭
    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]) 
        {
            delete m_fdContexts[i];
        }
    }
}

void IOManager::contextResize(size_t size) {
    m_fdContexts.resize(size);

    //初始化未初始化的FdContext对象
    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]==nullptr) 
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i;
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    //fd在数组里则初始化FdContext对象
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    //不存在则重新分配数组大小并初始化
    else 
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }

    //找到或者创建FdContext对象后，加互斥锁保证其状态不会被其他线程修改
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    //事件如果已经存在，则返回-1，因为不能添加相同的事件
    if(fd_ctx->events & event) 
    {
        return -1;
    }

    // add new event
    //如果事件存在，则修改已有事件；事件不存在，则准备添加该事件。
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events   = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    //通过epoll_ctl执行添加事件操作，添加事件到epoll中,成功返回0
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    ++m_pendingEventCount; //原子计数器，待处理事件++

    // update fdcontext member events
    fd_ctx->events = (Event)(fd_ctx->events | event);

    // update event context
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    //确保事件上下文中没有其他正在执行的调度器、协程或者回调函数
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    //设置调度器为当前调度器实例
    event_ctx.scheduler = Scheduler::getThis();

    if (cb) 
    {
        //保存回调函数的上下文
        event_ctx.cb.swap(cb);
    } 
    else 
    {
        //保存协程的上下文，并确保协程状态为runing
        event_ctx.fiber = Fiber::getThis();
        assert(event_ctx.fiber->getState() == Fiber::RUNING);
    }
    return 0;
}

bool IOManager::delEvent(int fd, Event event) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // if event doesn't exist,return;else continue
    if (!(fd_ctx->events & event)) 
    {
        return false; //fd_ctx中没有注册该事件
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event); //移除事件标识(句柄)
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; //有事件则进行修改，否则删除
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events; //设置新事件的类型
    epevent.data.ptr = fd_ctx; //设置数据指针

    int rt = epoll_ctl(m_epfd, op, fd, &epevent); //调用epoll_ctl执行修改或删除事件操作
    if (rt) 
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    --m_pendingEventCount; //待处理事件--

    // update fdcontext
    fd_ctx->events = new_events;

    // update event context
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

bool IOManager::cancelEvent(int fd, Event event) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    --m_pendingEventCount;

    // update fdcontext event context and trigger
    //相比于删除，取消事件中删除事件后需要将删除的事件交给triggerEvent函数，
    //将事件放入对应的调度器中进行触发。
    fd_ctx->triggerEvent(event);    
    return true;
}

bool IOManager::cancelAll(int fd) {
    // attemp to find FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    // none of events exist
    if (!fd_ctx->events) 
    {
        return false;
    }

    // delete all events
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    // update fdcontext, event context and trigger
    if (fd_ctx->events & READ) 
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if (fd_ctx->events & WRITE) 
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

void IOManager::tickle() {
    // no idle threads
    if(!hasIdleThreads()) 
    {
        return;
    }
    //如果有空闲协程，在管道m_tickleFds[1]中写入字符"T",
    //向管道另一端m_tickleFds[0]发送该消息，通知有新任务可以处理了。
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping() {
    uint64_t timeout = getNextTimer();
    // no timers left and no pending events left with the Scheduler::stopping()
    //检查确保没有剩余的定时器、没有待处理的事件并且调度器正在停止
    //~为按位取反操作，0ull表示64位长长整型，~0null即为64位无符号整数的最大值，表示定时器已经完成
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

void IOManager::idle() {
    static const uint64_t MAX_EVNETS = 256;
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVNETS]);

    while (true) 
    {
        if(debug) std::cout << "IOManager::idle(),run in thread: " << Thread::getThreadID() << std::endl; 

        if(stopping()) 
        {
            if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::getThreadID() << std::endl;
            break;
        }

        // blocked at epoll_wait
        int rt = 0;
        while(true)
        {
            static const uint64_t MAX_TIMEOUT = 5000;
            uint64_t next_timeout = getNextTimer();
            next_timeout = std::min(next_timeout, MAX_TIMEOUT); //避免等待时间过长

            rt = epoll_wait(m_epfd, events.get(), MAX_EVNETS, (int)next_timeout);
            // EINTR -> retry
            if(rt < 0 && errno == EINTR) //rt<0表示无限阻塞，EINTR表示信号中断
            {
                continue;
            } 
            else 
            {
                break;
            }
        }; //end epoll_wait

        // collect all timers overdue
        std::vector<std::function<void()>> cbs; //储存超时定时器回调的容器
        listExpiredTimerCb(cbs); //获取所有超时定时器的回调
        if(!cbs.empty()) 
        {
            for(const auto& cb : cbs) 
            {
                schedulerLock(cb);
            }
            cbs.clear();
        }
        
        // collect all events ready
        for (int i = 0; i < rt; ++i) 
        {
            epoll_event& event = events[i];

            // tickle event
            if (event.data.fd == m_tickleFds[0]) 
            {
                uint8_t dummy[256];
                // edge triggered -> exhaust
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            // other events
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            // convert EPOLLERR or EPOLLHUP to -> read or write event
            if (event.events & (EPOLLERR | EPOLLHUP)) 
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            // events happening during this turn of epoll_wait
            int real_events = NONE;
            if (event.events & EPOLLIN) 
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) 
            {
                real_events |= WRITE;
            }

            if ((fd_ctx->events & real_events) == NONE) 
            {
                continue;
            }

            // delete the events that have already happened
            int left_events = (fd_ctx->events & ~real_events);
            int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events    = EPOLLET | left_events;

            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) 
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // schedule callback and update fdcontext and event context
            //触发事件，事件执行。这里的triggerEvent会将事件放入调度器开始调度并执行
            if (real_events & READ) 
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) 
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // end for
        //当前线程主动让出控制权，调度器可以选择执行其他任务或再次进入idle状态
        Fiber::getThis()->yield();
  
    } // end while(true)
}

void IOManager::timerInsertedAtFront() {
    tickle();
}

}
