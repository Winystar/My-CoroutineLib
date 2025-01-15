#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>
#include "thread.h"

using namespace john;

void testFunc() {
    std::cout << "thread id:" << Thread::getThreadID() << ", threadname:" << Thread::getThreadName();
    std::cout << ", this id:" << Thread::getThis()->getID() << ", this name: " << Thread::getThis()->getName() << std::endl;

    sleep(60);
}

int main() {
    std::vector<std::shared_ptr<Thread>> thread_vec;

    for(int i = 0; i < 5; ++i) {
        std::shared_ptr<Thread> temp = std::make_shared<Thread>(&testFunc, "thread_"+std::to_string(i));
        thread_vec.push_back(temp);
    }

    for(int i = 0; i < 5; ++i) {
        thread_vec[i]->join();
    }

    return 0;
}