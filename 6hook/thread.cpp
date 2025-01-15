#include <sys/syscall.h>
#include <iostream>
#include <unistd.h>

#include "thread.h"

namespace john {

static thread_local Thread* t_thread = nullptr;//当前线程对象的指针
static thread_local  std::string t_thread_name = "UNKOWN";//当前线程名称

pid_t Thread::getThreadID() {
    return syscall(SYS_gettid);
}

Thread* Thread::getThis() {
    return t_thread;
}

const std::string& Thread::getThreadName() {
    return t_thread_name;
}

void Thread::setName(const std::string &name) {
    if(t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string& name):
m_cb(cb), m_name(name) {
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if(rt) {
        std::cerr << "pthread_creat thread failed, rt=" << rt << ", name=" << name;
        throw std::logic_error("pthread_create error");
    }

    m_semaphore.wait();
}

Thread::~Thread() {
    if(m_thread) {
        pthread_detach(m_thread);
        m_thread = 0;
    }
}

void Thread::join() {
    if(m_thread) {
        int rt = pthread_join(m_thread, nullptr);
        if(rt) {
            std::cerr << "pthread_join failed , rt=" << rt << ", name=" << m_name << std::endl;
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg) {
    Thread* thread = (Thread*)arg;

    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = getThreadID();
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(thread->m_cb);//直接交换cb和m_cb两个对象的状态，避免昂贵的拷贝。

    thread->m_semaphore.signal();

    cb();//真正地执行函数

    return 0;
}

}