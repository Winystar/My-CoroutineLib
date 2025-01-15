#include <vector>
#include "fiber.h"

using namespace john;

class Scheduler {
public:
    void scheduler(std::shared_ptr<Fiber> task) {
        task_vec.push_back(task);
    }

    void run() {
        std::cout << "number" << task_vec.size() << std::endl;

        std::shared_ptr<Fiber> task;

        auto it = task_vec.begin();
        while(it != task_vec.end()) {
            task = *it;
            task->resume(); //由主协程切换到子协程
            it++;
        }

        task_vec.clear();
    }

private:
    std::vector<std::shared_ptr<Fiber>> task_vec;
};

void test_fiber(int i) {
    std::cout << "hello world" << i << std::endl;
}

int main() {
    Fiber::getThis(); //初始化当前线程的主协程

    Scheduler sc; //创建调度器

    for(auto i = 0; i < 20; ++i) {
        std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(std::bind(test_fiber, i), 0, false);
        sc.scheduler(fiber); //将创建好的子协程加入任务队列
    }

    sc.run(); //开始调度运行 先来先服务

    return 0;
}