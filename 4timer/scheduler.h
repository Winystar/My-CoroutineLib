#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

//#include "hook.h"
#include <mutex>
#include <vector>
#include "fiber.h"
#include "thread.h"

namespace john {

class Scheduler {
public:
    Scheduler(size_t thread = 1, bool use_caller = true, const std::string& name = "Scheduler");
    virtual ~Scheduler(); //虚析构函数确保基类指针删除子类对象时，正确调用子类析构函数释放资源

    const std::string& getName() const {return m_name;}

public:
    static Scheduler* getThis(); //获取正在运行的调度器

protected:
    void setThis(); //设置正在运行的调度器

public:
    //添加任务到任务队列
    template <class FiberOrCb>
    void schedulerLock(FiberOrCb fc, int thread = -1) {
        bool need_tickle;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            need_tickle = m_tasks.empty();

            ScheduleTask task(fc, thread); //通过传入不同的参数调用不同的任务构造函数
            if(task.fiber || task.cb) {
                m_tasks.push_back(task);
            }
        }

        if(need_tickle) {
            tickle();
        }
    }

    virtual void start(); //启动线程池
    virtual void stop(); //关闭线程池

//protected表示其中的成员只能在当前类和子类进行访问，外部代码不能直接调用
protected:
//虚函数允许在子类中根据需求进行覆盖实现多态
    virtual void tickle(); //通知协程调度器有任务来啦

    virtual void run(); // 线程函数

    virtual void idle(); // 空闲协程函数

    virtual bool stopping(); //是否关闭调度器

    bool hasIdleThreads() {return m_idle_thread_count > 0;}

private:
    //任务结构体
    struct ScheduleTask {
        std::shared_ptr<Fiber> fiber;
        std::function<void()> cb;
        int thread;

        //默认构造
        ScheduleTask() {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }

        ScheduleTask(std::shared_ptr<Fiber> f, int threads) {
            fiber = f;
            thread = threads;
        }

        ScheduleTask(std::shared_ptr<Fiber>* f, int threads) {
            fiber.swap(*f);
            thread = threads;
        }

        ScheduleTask(std::function<void()> f, int threads) {
            cb = f;
            thread = threads;
        }

        ScheduleTask(std::function<void()>* f, int threads) {
            cb.swap(*f);
            thread = threads;
        }

        void reset() {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };

private:
    std::string m_name;

    std::mutex m_mutex; // 互斥锁 保护共享资源-任务队列

    std::vector<std::shared_ptr<Thread>> m_threads; //线程池

    std::vector<ScheduleTask> m_tasks; //任务队列

    std::vector<int> m_thread_id; //线程ID

    size_t m_thread_count = 0; //需要额外创建的线程数

    std::atomic<size_t> m_active_thread_count = {0}; //活跃线程数

    std::atomic<size_t> m_idle_thread_count = {0}; //空闲线程数

    bool m_use_caller; //主线程是否用作工作线程

    //如果主线程用作工作线程，需要额外创建调度协程
    std::shared_ptr<Fiber> m_scheduler_fiber;
    //记录主线程ID
    int m_main_thread = -1;

    bool m_stopping = false;//是否正在关闭
};

}

#endif