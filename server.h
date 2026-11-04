/*
 * server.h: TCP listener of the mcp plugin, one thread per client
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_SERVER_H
#define __MCP_SERVER_H

#include "config.h"
#include <vdr/thread.h>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

class cMcpServer : public cThread {
private:
  struct cConnection {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
    };
  const cMcpConfig &config;
  int listenFd;
  std::atomic<bool> stopping;
  std::mutex mutex;
  std::list<cConnection> connections;
  void Serve(int Fd, std::shared_ptr<std::atomic<bool>> Done);
  void Reap(void);
protected:
  virtual void Action(void);
public:
  cMcpServer(const cMcpConfig &Config);
  virtual ~cMcpServer();
  bool Open(void);
  void Shutdown(void);
  };

#endif
