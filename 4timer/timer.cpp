#include "timer.h"

namespace john {

bool Timer::cancel() {
    //写锁互斥锁
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if(m_cb == nullptr) 
    {
        return false;
    }
    else
    {
        m_cb = nullptr;
    }

    auto it = m_manager->m_timers.find(shared_from_this());
    if(it!=m_manager->m_timers.end())
    {
        m_manager->m_timers.erase(it); //删除定时器
    }
    return true;
}

//刷新定时器将会使下次触发延后
bool Timer::refresh() {
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if(!m_cb) 
    {
        return false;
    }

    auto it = m_manager->m_timers.find(shared_from_this());
    if(it==m_manager->m_timers.end())
    {
        return false;
    }

    //删除当前定时器并更新超时时间
    m_manager->m_timers.erase(it);
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
    //然后将新定时器加入到管理器中
    m_manager->m_timers.insert(shared_from_this());

    return true;
}

bool Timer::reset(uint64_t ms, bool from_now) {
    if(ms==m_ms && !from_now)
    {
        return true; //代表不需要重置
    }

    //需要重置
    //删除当前定时器，然后重新计算超时时间，并重新插入定时器
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);
    
        if(!m_cb) 
        {
            return false;
        }
        
        auto it = m_manager->m_timers.find(shared_from_this());
        if(it==m_manager->m_timers.end())
        {
            return false; //没找到
        }   
        m_manager->m_timers.erase(it); //找到了就删除
    }

    // reInsert
    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);
    m_ms = ms;
    m_next = start + std::chrono::milliseconds(m_ms);
    m_manager->addTimer(shared_from_this()); // insert with lock
    return true;
}

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager):
m_ms(ms), m_cb(cb), m_recurring(recurring),m_manager(manager) {
    auto now = std::chrono::system_clock::now();
    m_next = now + std::chrono::milliseconds(m_ms);
}

bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const {
    assert(lhs!=nullptr&&rhs!=nullptr);
    return lhs->m_next < rhs->m_next;
}

TimerManager::TimerManager() {
    //初始化当前系统时间，为后续检查系统时间错误时提供校对时间基准
    m_preTime = std::chrono::system_clock::now();
}

TimerManager::~TimerManager() {}

std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    addTimer(timer);
    return timer;
}

void TimerManager::addTimer(std::shared_ptr<Timer> timer) {
    bool at_front = false;//标识插入的是最早超时的定时器

    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        //将当前定时器插入set,取排序第一个
        auto it = m_timers.insert(timer).first; 
        //判断该定时器是否是set中最早超时的定时器
        at_front = (it == m_timers.begin()) && !m_tickled;
        
        // only tickle once till one thread wakes up and runs getNextTime()
        if(at_front) //防止重复唤醒，只允许一个定时任务运行
        {
            m_tickled = true;
        }
    }
   
    if(at_front)
    {
        // wake up 
        timerInsertedAtFront();
    }
}

//使用弱指针不增加对象的引用计数，避免循环引用
//如果条件成立，执行cb
static void onTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock(); //确保当前条件的对象仍然存在
    if(tmp)
    {
        cb();
    }
}

std::shared_ptr<Timer> TimerManager::addConidtionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring) {
    //将onTimer指向第一个addTimer,然后创建timer对象
    return addTimer(ms, std::bind(&onTimer, weak_cond, cb), recurring);
}

uint64_t TimerManager::getNextTimer() {
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    
    // reset m_tickled
    //指示在定时器插入到时间堆时是否需要触发额外操作，比如唤醒一个等待线程
    m_tickled = false;
    
    if (m_timers.empty())
    {
        // 返回最大值
        return ~0ull;
    }

    //当前系统时间
    auto now = std::chrono::system_clock::now();
    //时间堆中第一个定时器的下一个定时器的超时时间
    auto time = (*m_timers.begin())->m_next;

    //判断当前时间是否已经超过时间堆中下一个定时器的超时时间
    if(now>=time) 
    {
        // 已经有timer超时则直接返回
        return 0;
    }
    else
    {
        //没有timer超时，则计算当前时间到下一个超时时间的时间差
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
        //将时间差转化位毫秒
        return static_cast<uint64_t>(duration.count());            
    }
}

void TimerManager::listExpiredTimerCb(std::vector<std::function<void()>>& cbs) {
    auto now = std::chrono::system_clock::now();

    std::unique_lock<std::shared_mutex> write_lock(m_mutex); 

    bool rollover = detectClockRollover();//判断系统时间是否出错(是否回滚)
    
    // 时间堆不为空 && (回退(则表示需要时间校准）-> 清理所有timer || 超时 -> 清理超时timer）
    while (!m_timers.empty() && (rollover || (*m_timers.begin())->m_next <= now))
    {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());
        
        cbs.push_back(temp->m_cb); 

        if (temp->m_recurring)
        {
            // 当前时间+定时器间隔，重新加入时间堆
            temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        }
        else
        {
            // 清理cb
            temp->m_cb = nullptr;
        }
    }
}

//检测系统时间时间是否回滚
bool TimerManager::detectClockRollover() {
    bool rollover = false;
    auto now = std::chrono::system_clock::now();
    //比较标准：比较当前时间与上一次记录的时间减去一个小时的时间量，小于则判定回滚了
    if(now < (m_preTime - std::chrono::milliseconds(60 * 60 * 1000))) 
    {
        rollover = true;
    }
    m_preTime = now;
    return rollover;
}

//检测时间堆是否为空
bool TimerManager::hasTimer() {
    std::shared_lock<std::shared_mutex> resd_lock(m_mutex);
    return !m_timers.empty();
}

}