#ifndef _IOMANAGER_H_
#define _IOMANAGER_H_

#include "scheduler.h"
#include "timer.h"

namespace john {

class IOManager : public Scheduler, public TimerManager{
public:
    enum Event {
        NONE = 0x0, //表示没有事件
        //READ == EPOLLIN
        READ = 0x1, //表示读事件
        //WRITE == EPOLLOUT
        WRITE = 0x4 //表示写事件
    };

private:
    //用于描述一个文件描述符的事件上下文
    //每个socket fd都对应一个FdContext，包括fd值(句柄整数值)，fd上的事件，以及fd的读写事件上下文
    struct FdContext {
        //描述一个事件的上下文
        struct EventContext {
            //事件关联的调度器、协程或回调函数
            Scheduler* scheduler = nullptr; 
            std::shared_ptr<Fiber> fiber;
            std::function<void()> cb;
        };

        EventContext read; //读的上下文
        EventContext write; //写的上下文

        int fd = 0; // 事件关联的句柄
        Event events = NONE; //当前注册的事件

        std::mutex mutex;

        //根据事件类型获取事件上下文
        EventContext& getEventContext(Event event);
        //重置事件上下文
        void resetEventContext(EventContext& ctx);
        //根据事件类型调用对应的调度器去调度协程或者函数
        void triggerEvent(Event event);    
    };

public:
    IOManager(size_t threads = -1, bool use_caller = true, const std::string& name = "IOManager");
    ~IOManager();

    // add one event at a time to a fd, and link to a cb
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // delete event on a fd
    bool delEvent(int fd, Event event);
    // delete the event on a fd and trigger its callback
    bool cancelEvent(int fd, Event event);
    // delete all events and trigger its callback
    bool cancelAll(int fd);
    // get current scheduler object
    static IOManager* getThis();

protected:
    //通知调度器有任务需要进行调度
    void tickle() override;
    //判断调度器是否停止，没有IO事件时才能停止
    bool stopping() override;
    //实际的idle协程只是负责收集已经触发的fd的回调函数，并将其加入调度器的任务队列
    //真正的执行是发生在idle协程退出后，调度器在下一轮调度再执行。
    void idle() override;

    void timerInsertedAtFront() override;
    //调整文件描述符上下文数组的大小
    void contextResize(size_t size);

private:
    int m_epfd = 0; //epoll文件描述符
    // fd[0] read，fd[1] write
    int m_tickleFds[2]; //线程通信的管道的文件描述符
    //atomic该变量的操作是原子性的，不会被多线程影响
    std::atomic<size_t> m_pendingEventCount = {0}; //原子计数器，记录待处理的事件
    std::shared_mutex m_mutex; //读写锁
    // store fdcontexts for each fd
    std::vector<FdContext *> m_fdContexts; //文件描述符上下文数组，储存每个文件描述符的FdContext
};

}

#endif