#include "timer.h"
#include <unistd.h>
#include <iostream>

using namespace john;

// 定义一个简单的回调函数，输出一个整数
void func(int i)
{
    std::cout << "i: " << i << std::endl;
}

int main(int argc, char const *argv[])
{
    // 创建一个TimerManager的共享指针，管理所有定时器
    std::shared_ptr<TimerManager> manager(new TimerManager());
    std::vector<std::function<void()>> cbs;  // 用于存储超时的回调函数

    // 测试listExpiredCb超时功能
    {
        // 设置10个定时器，每个定时器的超时时间递增1秒
        for(int i = 0; i < 10; i++)
        {
            // 每个定时器的超时间分别是1s, 2s, ..., 10s，不重复触发（false表示不重复）
            manager->addTimer((i + 1) * 1000, std::bind(&func, i), false);
        }
        std::cout << "all timers have been set up" << std::endl;

        // 等待5秒钟，模拟定时器触发
        sleep(5);
        
        // 获取所有已过期的定时器回调，并执行
        manager->listExpiredTimerCb(cbs);
        while(!cbs.empty())
        {
            std::function<void()> cb = *cbs.begin();  // 获取第一个回调函数
            cbs.erase(cbs.begin());  // 删除已处理的回调
            cb();  // 执行回调函数
        }

        // 再等5秒钟，模拟更多定时器触发
        sleep(5);
        
        // 获取所有已过期的定时器回调，并执行
        manager->listExpiredTimerCb(cbs);
        while(!cbs.empty())
        {
            std::function<void()> cb = *cbs.begin();
            cbs.erase(cbs.begin());
            cb();
        }
    }
        
    // 测试定时器的重复触发功能
    {
        // 设置一个每1秒触发的定时器，并且是重复触发（true表示重复）
        manager->addTimer(1000, std::bind(&func, 1000), true);
        
        int j = 10;  // 设置定时器触发次数为10次
        while(j-- > 0)
        {
            sleep(1);  // 每次触发间隔1秒
            manager->listExpiredTimerCb(cbs);  // 获取已过期的定时器回调
            std::function<void()> cb = *cbs.begin();  // 获取第一个回调函数
            cbs.erase(cbs.begin());  // 删除已处理的回调
            cb();  // 执行回调函数
        }
    }

    return 0;
}
