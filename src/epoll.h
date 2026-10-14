#ifndef EPOLL_H
#define EPOLL_H
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/ip.h>  /* superset of previous */
#include <sys/timerfd.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>


// ============================== Tunables ===============================
// EPOLLERR va EPOLLHUP hamishe tavasote kernel gozaresh mishan va niazi be set kardan nadaran
#define EPOLL_EVENTS_TCP_NONBLOCKING (EPOLLIN | EPOLLRDHUP | EPOLLET)
// listener level-triggered hast ta ba mahdodiyat MAX_ACCEPT_PER_EVENT connection ja namone
#define EPOLL_LISTINER_EVENTS (EPOLLIN)


#endif // EPOLL_H
