#include "ioscheduler.h"
#include "hook.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <stack>
#include <cstring>
#include <chrono>
#include <thread>


/*基于事件驱动和非阻塞 I/O 实现一个HTTP服务端。
使用 john::IOManager 来调度和管理 I/O 事件，
确保每个客户端连接在非阻塞模式下处理，从而提高并发性能。*/

static int sock_listen_fd = -1;

void test_accept();
void error(const char *msg)
{
    perror(msg);
    printf("erreur...\n");
    exit(1);
}

//监听IO事件
void watch_io_read()
{
    john::IOManager::getThis()->addEvent(sock_listen_fd, john::IOManager::READ, test_accept);
}

void test_accept()
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    int fd = accept(sock_listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0)
    {
        // 如果接收连接失败，退出
        return;
    }
    
    //std::cout << "accepted connection, fd = " << fd << std::endl;
    fcntl(fd, F_SETFL, O_NONBLOCK);  // 设置为非阻塞模式

    john::IOManager::getThis()->addEvent(fd, john::IOManager::READ, [fd]() 
    {
        char buffer[1024]; // 缓冲区
        memset(buffer, 0, sizeof(buffer));

        // 用来标记是否保持连接
        bool keep_alive = false;

        while (true)
        {
            // 接收客户端请求数据
            int ret = recv(fd, buffer, sizeof(buffer), 0);
            if (ret > 0)
            {
                std::string request(buffer);

                // 判断是否有 "Connection: keep-alive" 字段
                if (request.find("Connection: keep-alive") != std::string::npos)
                {
                    keep_alive = true;
                }

                // 构建HTTP响应
                const char *response = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Length: 13\r\n"
                                       "Connection: ";

                // 根据 keep_alive 决定连接是否关闭
                if (keep_alive)
                {
                    response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 13\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n"
                               "Hello, World!";
                }
                else
                {
                    response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 13\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Hello, World!";
                }

                // 发送HTTP响应
                ret = send(fd, response, strlen(response), 0);
                if (ret < 0)
                {
                    close(fd);
                    break;
                }

                // 如果是短连接，处理完请求后关闭连接
                if (!keep_alive)
                {
                    close(fd);
                    break;
                }
                else
                {
                    // 如果是长连接，继续等待下一个请求
                    memset(buffer, 0, sizeof(buffer));  // 清空缓冲区
                }
            }
            else
            {
                if (ret == 0 || errno != EAGAIN)
                {
                    // 如果没有数据或者发生错误，关闭连接
                    close(fd);
                    break;
                }
                else if (errno == EAGAIN)
                {
                    // 如果没有数据，稍作休息避免忙等
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }
    });

    // 继续监听新的连接请求
    john::IOManager::getThis()->addEvent(sock_listen_fd, john::IOManager::READ, test_accept);
}


void test_iomanager()
{
    int portno = 8080;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 设置套接字
    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0)
    {
        error("Error creating socket..\n");
    }

    int yes = 1;
    // 解决 "address already in use" 错误
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset((char *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portno);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // 绑定套接字并监听连接
    if (bind(sock_listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error("Error binding socket..\n");

    if (listen(sock_listen_fd, 1024) < 0)
    {
        error("Error listening..\n");
    }

    printf("epoll echo server listening for connections on port: %d\n", portno);

    //设置监听套接字为非阻塞模式
    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);

    //创建IOManager实例，添加读事件，处理客户端请求
    john::IOManager iom(9);
    iom.addEvent(sock_listen_fd, john::IOManager::READ, test_accept);
}

int main(int argc, char *argv[])
{
    test_iomanager();
    return 0;
}