#ifndef _TIMER_H_
#define _TIMER_H_

#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <assert.h>
#include <functional>
#include <mutex>

namespace john {

//前向声明,在类定义之前告诉编译器这个类的存在。
//为了提高编译效率、减少依赖、避免循环依赖等。
//可以让编译器知道它是一个类型，可以使用它的指针或引用，但不需要定义完整的类。
class TimerManager; 

//public继承，用于返回智能指针对象timer的this值
class Timer : public std::enable_shared_from_this<Timer> {
    //友元访问，用于访问管理器类的成员
    friend class TimerManager;
public:
    bool cancel(); // 从时间堆删除timer

    bool refresh(); //刷新timer

    //重设timer超时时间，ms为定时器执行间隔时间，from_now表示是否从当前时间开始计算
    bool reset(uint64_t ms, bool from_now);

private:    
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);

private:
    bool m_recurring = false; //是否循环

    uint64_t m_ms = 0; //超时时间

    //绝对超时时间,即定时器下一次触发的时间点。
    std::chrono::time_point<std::chrono::system_clock> m_next;

    std::function<void()> m_cb; //超时触发的回调函数

    TimerManager* m_manager = nullptr; //管理这个timer的管理器

private:
    //最小堆的比较函数 比较两个timer的绝对超时时间 
    struct Comparator {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };

};

class TimerManager {
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();

    //添加timer
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    //添加条件timer
    std::shared_ptr<Timer> addConidtionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

    //拿到堆中最近的超时时间
    uint64_t getNextTimer();

    //处理所有已经超时的定时器的回调函数，处理定时器的循环逻辑
    void listExpiredTimerCb(std::vector<std::function<void()>>& cbs);

    //堆中是否有timer
    bool hasTimer();

protected:
    //当最早的timer加入到堆中后，调用该函数通知IOmanager更新当前的epoll_wait超时
    virtual void timerInsertedAtFront() {};

    //添加timer
    void addTimer(std::shared_ptr<Timer> timer);

private:
    //当系统时间改变时，调用该函数检测系统时间是否出现时间校对问题
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex;

    //时间堆
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

    //时间器是否被唤醒的标志位
    bool m_tickled = false;

    //上次检查系统时间是否回退的绝对时间
    std::chrono::time_point<std::chrono::system_clock> m_preTime;
};

}

#endif