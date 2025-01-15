#ifndef _HOOK_H_
#define _HOOK_H_

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>          
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <fcntl.h>

//外挂式hook
//通过优先加载自定义的动态库来实现hook
//将hook的系统调用同名实现编译为.so动态库，设置LD_PRELOAD环境变量使其优先被加载
namespace john {

bool is_hook_enable(); //判断hook是否启动
void set_hook_enable(bool flag); //设置hook是否启动

}

extern "C" {
	// track the original version
    //C函数声明与重定向
    /*sleep延时接口sleep/usleep/nanosleep
    hook封装：在执行系统调用前，给调度器注册定时事件，在定时任务触发后再resume执行该协程，
    该协程注册完事件后即可yield让出控制权。*/
	typedef unsigned int (*sleep_fun) (unsigned int seconds);
	extern sleep_fun sleep_f;

	typedef int (*usleep_fun) (useconds_t usec);

	extern usleep_fun usleep_f;

	typedef int (*nanosleep_fun) (const struct timespec* req, struct timespec* rem);
	extern nanosleep_fun nanosleep_f;

    /*socket IO接口read/write/recv等等
    首先判断是不是socket fd，判断用户是否显示地设置过非阻塞模式，如果是则使用原始系统调用，否则需要hook。
    hook封装：在执行系统调用前，给调度器注册IO事件，等待事件发生后再resume执行该协程，
    该协程注册完事件后即可yield让出控制权。*/
    typedef int (*socket_fun) (int domain, int type, int protocol);
	extern socket_fun socket_f;

	typedef int (*connect_fun) (int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	extern connect_fun connect_f;

	typedef int (*accept_fun) (int sockfd, struct sockaddr *addr, socklen_t *addrlen);
	extern accept_fun accept_f;

	typedef ssize_t (*read_fun) (int fd, void *buf, size_t count);
	extern read_fun read_f;

	typedef ssize_t (*readv_fun)(int fd, const struct iovec *iov, int iovcnt);
	extern readv_fun readv_f;

	typedef ssize_t (*recv_fun) (int sockfd, void *buf, size_t len, int flags);
	extern recv_fun recv_f;

	typedef ssize_t (*recvfrom_fun) (int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
	extern recvfrom_fun recvfrom_f;

	typedef ssize_t (*recvmsg_fun) (int sockfd, struct msghdr *msg, int flags);
	extern recvmsg_fun recvmsg_f;

	typedef ssize_t (*write_fun) (int fd, const void *buf, size_t count);
	extern write_fun write_f;

	typedef ssize_t (*writev_fun) (int fd, const struct iovec *iov, int iovcnt);
	extern writev_fun writev_f;

	typedef ssize_t (*send_fun) (int sockfd, const void *buf, size_t len, int flags);
	extern send_fun send_f;

	typedef ssize_t (*sendto_fun) (int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
	extern sendto_fun sendto_f;

	typedef ssize_t (*sendmsg_fun) (int sockfd, const struct msghdr *msg, int flags);
	extern sendmsg_fun sendmsg_f;

    //socket/fcntl/ioctl/close等接口
    //处理边缘情况，如：分配fd上下文、处理超时、用户显示设置非阻塞等问题
    typedef int (*close_fun) (int fd);
	extern close_fun close_f;

	typedef int (*fcntl_fun) (int fd, int cmd, ... /* arg */ );
	extern fcntl_fun fcntl_f;

	typedef int (*ioctl_fun) (int fd, unsigned long request, ...);
	extern ioctl_fun ioctl_f;

	typedef int (*getsockopt_fun) (int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    extern getsockopt_fun getsockopt_f;

    typedef int (*setsockopt_fun) (int sockfd, int level, int optname, const void *optval, socklen_t optlen);
    extern setsockopt_fun setsockopt_f;

    // function prototype -> 对应.h中已经存在 可以省略
	// sleep function 
	unsigned int sleep(unsigned int seconds);
	int usleep(useconds_t usce);
	int nanosleep(const struct timespec* req, struct timespec* rem);

	// socket funciton
	int socket(int domain, int type, int protocol);
	int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

	// read 
	ssize_t read(int fd, void *buf, size_t count);
	ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

    ssize_t recv(int sockfd, void *buf, size_t len, int flags);
    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

    // write
    ssize_t write(int fd, const void *buf, size_t count);
    ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

    ssize_t send(int sockfd, const void *buf, size_t len, int flags);
    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);

    // fd
    int close(int fd);

    // socket control
    int fcntl(int fd, int cmd, ... /* arg */ );
    int ioctl(int fd, unsigned long request, ...);

    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
}

#endif