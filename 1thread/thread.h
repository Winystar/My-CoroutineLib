#ifndef _THREAD_H_
#define _THREAD_H_

#include <mutex>
#include <condition_variable>
#include <functional>

namespace john {

//用于线程间的同步
class Semaphore {
private:
    std::mutex mtx;
    std::condition_variable cv;
    int count;

public:
    //初始化信号量 explicit确保构造函数只能通过显式传递一个整数来初始化 Semaphore 对象
    //而不能通过隐式类型转换（例如 Semaphore sem = 10;）进行初始化。
    explicit Semaphore(int count_ = 0) : count(count_) {}

    //P操作
    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        while(count == 0) {
            cv.wait(lock);//通过lock等待唤醒信号
        }
        count--;
    }

    //V操作
    void signal() {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        cv.notify_one();
    }
};

class Thread {
public:
    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();

    //？???
    pid_t getID() const { return m_id; }
    const std::string& getName() const { return m_name;}

    void join();

public:
    //static关键字主要是为了方便通过限定符直接调用类中的函数，同时代表生命周期随着程序运行结束而销毁
    //获取系统分配的线程id
    static pid_t getThreadID();
    //获取当前的线程
    static Thread* getThis();
    //获取当前线程的名字
    static const std::string& getThreadName();
    //设置当前线程的名字
    static void setName(const std::string& name);

private:
    //线程函数
    static void* run(void* arg);

private:
    //线程ID
    pid_t m_id = -1;
    //线程计数
    pthread_t m_thread = 0;

    //线程运行的函数
    std::function<void()> m_cb;
    //线程名称
    std::string m_name;

    //引入信号量类完成线程同步
    Semaphore m_semaphore;
};

}

#endif 