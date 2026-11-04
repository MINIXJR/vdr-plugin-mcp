/*
 * server.cpp: TCP listener of the mcp plugin, one thread per client
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "server.h"
#include "session.h"
#include <vdr/tools.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#define MAX_REQUEST (4 * 1024 * 1024)

static bool SendAll(int Fd, const std::string &Data)
{
  size_t done = 0;
  while (done < Data.size()) {
        ssize_t n = send(Fd, Data.data() + done, Data.size() - done, MSG_NOSIGNAL);
        if (n < 0) {
           if (errno == EINTR)
              continue;
           return false;
           }
        done += n;
        }
  return true;
}

cMcpServer::cMcpServer(const cMcpConfig &Config)
:cThread("mcp server")
,config(Config)
,listenFd(-1)
,stopping(false)
{
}

cMcpServer::~cMcpServer()
{
  Shutdown();
}

bool cMcpServer::Open(void)
{
  listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listenFd < 0) {
     esyslog("mcp: socket: %s", strerror(errno));
     return false;
     }
  int on = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(config.port);
  if (inet_pton(AF_INET, config.bind.c_str(), &a.sin_addr) != 1) {
     esyslog("mcp: invalid bind address '%s'", config.bind.c_str());
     close(listenFd);
     listenFd = -1;
     return false;
     }
  if (bind(listenFd, (sockaddr *)&a, sizeof(a)) < 0 || listen(listenFd, 4) < 0) {
     esyslog("mcp: cannot listen on %s:%d: %s", config.bind.c_str(), config.port, strerror(errno));
     close(listenFd);
     listenFd = -1;
     return false;
     }
  isyslog("mcp: listening on %s:%d, SVDRP port %d", config.bind.c_str(), config.port, config.svdrpPort);
  if (config.bind != "127.0.0.1")
     esyslog("mcp: WARNING: %s is reachable from the network without authentication", config.bind.c_str());
  Start();
  return true;
}

void cMcpServer::Reap(void)
{
  std::lock_guard<std::mutex> lock(mutex);
  for (auto it = connections.begin(); it != connections.end(); ) {
      if (*it->done) {
         if (it->thread.joinable())
            it->thread.join();
         it = connections.erase(it);
         }
      else
         ++it;
      }
}

void cMcpServer::Action(void)
{
  while (Running() && !stopping) {
        Reap();
        pollfd p = { listenFd, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0)
           continue;
        int fd = accept4(listenFd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
           continue;
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mutex);
        connections.push_back(cConnection{ std::thread(&cMcpServer::Serve, this, fd, done), done });
        }
}

void cMcpServer::Serve(int Fd, std::shared_ptr<std::atomic<bool>> Done)
{
  isyslog("mcp: client connected");
  cMcpSession session(config);
  std::string buffer;
  char tmp[4096];
  bool ok = true;
  while (ok && !stopping) {
        pollfd p = { Fd, POLLIN, 0 };
        int r = poll(&p, 1, 1000);
        if (r < 0 && errno == EINTR)
           continue;
        if (r < 0)
           break;
        if (r == 0)
           continue;
        ssize_t n = read(Fd, tmp, sizeof(tmp));
        if (n <= 0)
           break;
        buffer.append(tmp, n);
        if (buffer.size() > MAX_REQUEST) {
           esyslog("mcp: request too large, closing connection");
           break;
           }
        size_t pos;
        while (ok && (pos = buffer.find('\n')) != std::string::npos) {
              std::string line = buffer.substr(0, pos);
              buffer.erase(0, pos + 1);
              if (!line.empty() && line.back() == '\r')
                 line.pop_back();
              if (line.empty())
                 continue;
              std::string reply = session.Handle(line);
              if (!reply.empty() && !SendAll(Fd, reply + "\n"))
                 ok = false;
              }
        }
  close(Fd);
  isyslog("mcp: client disconnected");
  *Done = true;
}

void cMcpServer::Shutdown(void)
{
  stopping = true;
  Cancel(3);
  if (listenFd >= 0) {
     close(listenFd);
     listenFd = -1;
     }
  std::lock_guard<std::mutex> lock(mutex);
  for (auto &c : connections) {
      if (c.thread.joinable())
         c.thread.join();
      }
  connections.clear();
}
