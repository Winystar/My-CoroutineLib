#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// apply XX to all functions
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

//匿名空间john中实现hook函数的初始化，提供是否启动hook的函数
namespace john {

// if this thread is using hooked function 
//线程局部变量类型的参数，每个线程都会判断是否需要hook，默认关闭
static thread_local bool t_hook_enable = false;

//返回当前线程是否启用hook
bool is_hook_enable()
{
    return t_hook_enable;
}

//设置是否启动hook
void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}


void hook_init()
{
	static bool is_inited = false; //静态局部变量保证只初始化一次，防止重复初始化
	if(is_inited)
	{
		return;
	}

	// test
	is_inited = true;

// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
	HOOK_FUN(XX)
#undef XX
}

// static variable initialisation will run before the main function
struct HookIniter
{
	HookIniter() //hook函数
	{
		hook_init(); //初始化hook，让原始系统调用绑定到宏展开的函数指针中
	}
};

//定义一个静态的HookIniter实例，使得hook_init会在程序开始时被调用，从而初始化hook函数
//因为静态变量的初始化化发生在mian函数前
static HookIniter s_hook_initer;

} //end namespace john


//该结构的成员变量表示定时器是否被取消，用于跟踪定时器状态信息
struct timer_info 
{
    int cancelled = 0;
};

// 通用的 I/O 操作函数模板
//将 I/O 操作包装起来，增加了超时和事件处理逻辑，使得能够在非阻塞模式下有效地处理 I/O 操作
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args) 
{
    // 检查是否启用hook，如果没有，直接调用原始的 I/O 函数
    if(!john::t_hook_enable) 
    {
        return fun(fd, std::forward<Args>(args)...); // 使用完美转发将参数传递给原函数
    }

    // 获取文件描述符的上下文（FdCtx），获取文件描述符相关的状态信息
    std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(fd);
    if(!ctx) 
    {
        // 如果无法获取上下文，直接调用原始 I/O 函数
        return fun(fd, std::forward<Args>(args)...);
    }

    // 如果文件描述符已经关闭，返回错误
    if(ctx->isClosed()) 
    {
        errno = EBADF; // 设置错误码为 EBADF（Bad file descriptor）
        return -1;
    }

    // 如果不是套接字或用户设置了非阻塞模式，直接调用原始 I/O 函数
    if(!ctx->isSocket() || ctx->getUserNonblock()) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取定时器文件描述符的超时值
    uint64_t timeout = ctx->getTimeout(timeout_so);

    // 创建一个 shared_ptr 用于保存定时器状态信息
    std::shared_ptr<timer_info> tinfo(new timer_info);

//1.处理系统调用被中断（EINTR）的情况，必要时重试。
retry:
    // 执行对应的 I/O 操作，实际执行传入的fun（原始系统调用）
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // 如果 I/O 操作被系统调用中断（如收到 SIGINT 等），则重试
    while(n == -1 && errno == EINTR) 
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    //2.处理资源暂时不可用的情况(EAGAIN)，使用定时器和事件机制，等待 I/O 操作完成或者超时。
    // 如果返回值为 -1 且 errno 为 EAGAIN，需要等待资源可用再次尝试
    if(n == -1 && errno == EAGAIN) 
    {
        john::IOManager* iom = john::IOManager::getThis();
        
        std::shared_ptr<john::Timer> timer;
        
        std::weak_ptr<timer_info> winfo(tinfo); //弱指针防止循环引用

        //-1可以看作“无限”或“未设置”的特殊值。不等于-1表示设置了超时值，需要进一步处理。
        //在连接操作超时的情况下设置一个定时器，并设置回调函数：如果超时发生，就取消写事件，从而避免无限等待连接而阻塞。
        if(timeout != (uint64_t)-1) 
        {
            //如果设置了超时值，定时器会在超时后触发回调函数，再执行取消操作
            //[winfo, fd, iom, event]() {...}这是一个lambda表达式，{...}其中是回调函数内容
            timer = iom->addConidtionTimer(timeout, [winfo, fd, iom, event]() 
            {
                //通过lock获取对应的shared_ptr，如果共享指针被销毁则返回nullptr，所以需要检查返回值是否有效
                auto t = winfo.lock();
                //如果定时器已被销毁或者已取消，直接返回
                if(!t || t->cancelled) 
                {
                    return; 
                }
                //如果定时器未取消，标记为超时错误状态，并通知 IO 管理器取消写事件
                t->cancelled = ETIMEDOUT;
                //连接操作在此时仍未完成，IO 管理器将不会再处理该文件描述符的写事件，从而防止继续等待连接完成。
                iom->cancelEvent(fd, (john::IOManager::Event)(event));
            }, winfo); //指向定时器的关联信息。weak_ptr 是用来防止循环引用的智能指针，它不会阻止 timer_info 对象被销毁。
        }

        //未设置超时值，则是IO操作未完成需要等待
        // 3.（关键）将当前事件添加到 I/O 管理器中，进行事件注册
        int rt = iom->addEvent(fd, (john::IOManager::Event)(event));
        
        if(rt) 
        {
            // 如果事件添加失败，打印错误信息并取消定时器
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer) 
            {
                timer->cancel(); // 取消定时器
            }
            return -1; // 返回错误
        } 
        else // 添加事件成功
        {
            // 挂起当前协程，等待事件完成
            john::Fiber::getThis()->yield();

            // 等待完成后取消定时器
            if(timer) 
            {
                timer->cancel();
            }
            
            // 如果超时被触发，设置 errno 并返回错误
            if(tinfo->cancelled == ETIMEDOUT) 
            {
                errno = tinfo->cancelled;
                return -1;
            }
            
            // 重新尝试执行 I/O 操作
            goto retry;
        }
    }

    // 返回 I/O 操作的结果
    return n;
}

extern "C" {

// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name ## _fun name ## _f = nullptr;
	HOOK_FUN(XX)
#undef XX

//sleep hook封装
// only use at task fiber
unsigned int sleep(unsigned int seconds)
{
	if(!john::t_hook_enable)
	{
		return sleep_f(seconds);
	}

	std::shared_ptr<john::Fiber> fiber = john::Fiber::getThis();
	john::IOManager* iom = john::IOManager::getThis();
	// add a timer to reschedule this fiber
	iom->addTimer(seconds*1000, [fiber, iom](){iom->schedulerLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();
	return 0;
}

int usleep(useconds_t usec)
{
	if(!john::t_hook_enable)
	{
		return usleep_f(usec);
	}

	std::shared_ptr<john::Fiber> fiber = john::Fiber::getThis();
	john::IOManager* iom = john::IOManager::getThis();
	// add a timer to reschedule this fiber
	iom->addTimer(usec/1000, [fiber, iom](){iom->schedulerLock(fiber);});
	// wait for the next resume
	fiber->yield();
	return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
	if(!john::t_hook_enable)
	{
		return nanosleep_f(req, rem);
	}	

	int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000;

	std::shared_ptr<john::Fiber> fiber = john::Fiber::getThis();
	john::IOManager* iom = john::IOManager::getThis();
	// add a timer to reschedule this fiber
	iom->addTimer(timeout_ms, [fiber, iom](){iom->schedulerLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();	
	return 0;
}

//socket hook封装
int socket(int domain, int type, int protocol)
{
	if(!john::t_hook_enable)
	{
		return socket_f(domain, type, protocol);
	}	

	int fd = socket_f(domain, type, protocol);
	if(fd==-1)
	{
		std::cerr << "socket() failed:" << strerror(errno) << std::endl;
		return fd;
	}
	john::FdMgr::GetInstance()->get(fd, true);
	return fd;
}

// 超时情况下的非阻塞套接字连接
int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) 
{
    // 没有启动hook，直接调用原始系统调用进行连接
    if (!john::t_hook_enable) 
    {
        return connect_f(fd, addr, addrlen);
    }

    // 1.获取与文件描述符 (fd) 相关的上下文（FdCtx），用于管理该文件描述符的状态
    std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(fd);
    
    // 如果上下文无效或该文件描述符已关闭，返回错误并设置 errno 为 EBADF
    if (!ctx || ctx->isClosed()) 
    {
        errno = EBADF;
        return -1;
    }

    // 如果该文件描述符不是一个套接字，直接调用默认的 connect_f 函数进行连接
    if (!ctx->isSocket()) 
    {
        return connect_f(fd, addr, addrlen);
    }

    // 如果该文件描述符的用户设置为非阻塞模式，直接调用默认的 connect_f 函数进行连接
    if (ctx->getUserNonblock()) 
    {
        return connect_f(fd, addr, addrlen);
    }

    // 2.尝试执行连接操作
    int n = connect_f(fd, addr, addrlen);
    
    // 如果连接成功，直接返回 0
    if (n == 0) 
    {
        return 0;
    } 
    // 如果连接失败且错误码不是 EINPROGRESS（表示连接正在进行中），则返回错误
    else if (n != -1 || errno != EINPROGRESS) 
    {
        return n;
    }

    // 如果到达这里，表示连接操作处于阻塞状态，等待连接完成
    // 通过事件驱动机制来非阻塞地等待连接完成，避免阻塞当前线程。

    // 获取当前的 IO 管理器
    john::IOManager* iom = john::IOManager::getThis();
    
    // 创建一个定时器和一个与之关联的结构体，用于处理超时逻辑
    std::shared_ptr<john::Timer> timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    // 如果设置了超时时间（timeout_ms），则启动定时器
    if (timeout_ms != (uint64_t)-1) 
    {
        timer = iom->addConidtionTimer(timeout_ms, [winfo, fd, iom]() 
        {
            // 获取定时器关联的结构体
            auto t = winfo.lock();
            if (!t || t->cancelled) 
            {
                return;
            }
            // 如果定时器超时，定时器设置为取消状态，并通知 IO 管理器取消写事件
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd, john::IOManager::WRITE);
        }, winfo);
    }

    // 将套接字 fd 注册到 IO 管理器的写事件中，等待连接完成
    int rt = iom->addEvent(fd, john::IOManager::WRITE);
    if (rt == 0) 
    {
        // 如果事件成功注册，则将当前协程挂起（yield），等待事件通知
        john::Fiber::getThis()->yield();

        // 恢复时，取消定时器（如果已设置）
        if (timer) 
        {
            timer->cancel();
        }

        // 如果定时器已被取消，说明是超时导致的，设置 errno 为超时错误
        if (tinfo->cancelled) 
        {
            errno = tinfo->cancelled;
            return -1;
        }
    } 
    else 
    {
        // 如果添加写事件失败，取消定时器（如果已设置），并输出错误信息
        if (timer) 
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // 连接操作完成后，检查套接字是否已成功连接
    int error = 0;
    socklen_t len = sizeof(int);
    
    // 获取套接字的错误状态，如果发生错误，则返回 -1
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) 
    {
        return -1;
    }

    // 如果没有错误，返回 0，表示连接成功
    if (!error) 
    {
        return 0;
    } 
    // 如果发生了其他错误，将 errno 设置为该错误，并返回 -1
    else 
    {
        errno = error;
        return -1;
    }
}


//connect/accept等等IO系统调用 hook封装
static uint64_t s_connect_timeout = -1;
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd = do_io(sockfd, accept_f, "accept", john::IOManager::READ, SO_RCVTIMEO, addr, addrlen);	
	if(fd>=0)
	{
		john::FdMgr::GetInstance()->get(fd, true);
	}
	return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
	return do_io(fd, read_f, "read", john::IOManager::READ, SO_RCVTIMEO, buf, count);	
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, readv_f, "readv", john::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);	
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	return do_io(sockfd, recv_f, "recv", john::IOManager::READ, SO_RCVTIMEO, buf, len, flags);	
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
	return do_io(sockfd, recvfrom_f, "recvfrom", john::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);	
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	return do_io(sockfd, recvmsg_f, "recvmsg", john::IOManager::READ, SO_RCVTIMEO, msg, flags);	
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return do_io(fd, write_f, "write", john::IOManager::WRITE, SO_SNDTIMEO, buf, count);	
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, writev_f, "writev", john::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);	
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	return do_io(sockfd, send_f, "send", john::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);	
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return do_io(sockfd, sendto_f, "sendto", john::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);	
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	return do_io(sockfd, sendmsg_f, "sendmsg", john::IOManager::WRITE, SO_SNDTIMEO, msg, flags);	
}

int close(int fd)
{
	if(!john::t_hook_enable)
	{
		return close_f(fd);
	}	

	std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(fd);

	if(ctx)
	{
		auto iom = john::IOManager::getThis();
		if(iom)
		{	
			iom->cancelAll(fd);
		}
		// del fdctx
		john::FdMgr::GetInstance()->del(fd);
	}
	return close_f(fd);
}

// 对 fd 进行控制的操作函数 fcntl 函数的 hook 封装
int fcntl(int fd, int cmd, ... /* arg */ )
{
    va_list va; // 用于访问可变参数列表

    va_start(va, cmd); // 初始化 va_list，获取可变参数列表

    switch(cmd) 
    {
        // 设置文件描述符的标志 (如 O_NONBLOCK 等)
        case F_SETFL:
            {
                int arg = va_arg(va, int); // 获取接下来的 int 类型参数（文件标志）
                va_end(va); // 结束可变参数的处理

                // 获取文件描述符对应的上下文
                std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(fd);
                
                // 如果上下文无效、文件已关闭或者不是套接字，直接调用默认的 fcntl
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return fcntl_f(fd, cmd, arg);
                }

                // 设置用户是否设定了非阻塞标志 O_NONBLOCK
                ctx->setUserNonblock(arg & O_NONBLOCK);

                // 根据系统的设置决定是否保持非阻塞
                if(ctx->getSysNonblock()) 
                {
                    arg |= O_NONBLOCK; // 系统设置为非阻塞，按位或，将 O_NONBLOCK 标志添加到 arg 中
                } 
                else 
                {
                    arg &= ~O_NONBLOCK; // 否则，取反按位与，移除 O_NONBLOCK
                }

                // 调用默认的 fcntl 函数，进行实际的操作
                return fcntl_f(fd, cmd, arg);
            }
            break;

        // 获取文件描述符的标志 (如 O_NONBLOCK 等)
        case F_GETFL:
            {
                va_end(va); // 结束可变参数的处理
                int arg = fcntl_f(fd, cmd); // 调用默认的 fcntl 函数获取当前标志

                // 获取文件描述符对应的上下文
                std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(fd);
                
                // 如果上下文无效、文件已关闭或者不是套接字，直接返回当前标志
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return arg;
                }

                // 返回给用户的标志值，若用户设置了非阻塞标志，显示为带有 O_NONBLOCK 的标志
                if(ctx->getUserNonblock()) 
                {
                    return arg | O_NONBLOCK; // 返回带 O_NONBLOCK 的标志
                } 
                else 
                {
                    return arg & ~O_NONBLOCK; // 返回移除 O_NONBLOCK 的标志
                }
            }
            break;

        // 对文件描述符执行 dup、setfd 等操作，这些操作不影响非阻塞状态
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
//使用宏进行预处理，如果当前系统中没有定义就不会进入ifdef和endif之内了，目的是让代码在不同环境中灵活性更高，因为当前宏可能在当前系统中没有定义
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int); // 获取接下来的 int 类型参数
                va_end(va); // 结束可变参数的处理
                return fcntl_f(fd, cmd, arg); // 调用默认的 fcntl 函数处理
            }
            break;

        // 获取文件描述符的状态或其他信息（如 fd 是否关闭、进程标识符等）
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va); // 结束可变参数的处理
                return fcntl_f(fd, cmd); // 调用默认的 fcntl 函数处理
            }
            break;

        // 锁定文件描述符的操作
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*); // 获取结构体参数
                va_end(va); // 结束可变参数的处理
                return fcntl_f(fd, cmd, arg); // 调用默认的 fcntl 函数处理
            }
            break;

        // 文件拥有者的操作
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*); // 获取结构体参数
                va_end(va); // 结束可变参数的处理
                return fcntl_f(fd, cmd, arg); // 调用默认的 fcntl 函数处理
            }
            break;

        // 其他不支持的命令，直接交给默认的 fcntl 处理
        default:
            va_end(va); // 结束可变参数的处理
            return fcntl_f(fd, cmd); // 调用默认的 fcntl 函数处理
    }
}

// 对 fd 进行 IO 控制操作的函数封装
int ioctl(int fd, unsigned long request, ...)
{
    va_list va; 
    va_start(va, request); // 初始化 va_list，获取可变参数列表
    void* arg = va_arg(va, void*); // 获取接下来的 void* 类型参数
    va_end(va); // 结束可变参数的处理

    // 如果是 FIONBIO 请求 (设置非阻塞)
    if(FIONBIO == request) 
    {
        bool user_nonblock = !!*(int*)arg; // 获取用户是否设置非阻塞标志
        std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(fd);
        
        // 如果上下文无效、文件已关闭或者不是套接字，直接调用默认的 ioctl
        if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
        {
            return ioctl_f(fd, request, arg);
        }

        // 设置用户的非阻塞标志
        ctx->setUserNonblock(user_nonblock);
    }

    // 调用默认的 ioctl 函数处理
    return ioctl_f(fd, request, arg);
}

// 获取套接字选项的封装
int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    // 直接调用默认的 getsockopt 函数
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

// 设置套接字选项的封装
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    // 如果没有启用hook，直接调用默认的 setsockopt
    if(!john::t_hook_enable) 
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    // 如果是设置超时选项（SO_RCVTIMEO 或 SO_SNDTIMEO）
    if(level == SOL_SOCKET) 
    {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) 
        {
            // 获取文件描述符的上下文
            std::shared_ptr<john::FdCtx> ctx = john::FdMgr::GetInstance()->get(sockfd);
            if(ctx) 
            {
                // 将 timeval 转换为毫秒并设置超时
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }

    // 调用默认的 setsockopt 函数处理
    return setsockopt_f(sockfd, level, optname, optval, optlen);	
}


}