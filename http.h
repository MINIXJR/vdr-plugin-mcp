/*
 * http.h: MCP "Streamable HTTP" transport with TLS and API keys
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_HTTP_H
#define __MCP_HTTP_H

#include "config.h"
#include <vdr/thread.h>
#include <atomic>
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

typedef struct ssl_ctx_st SSL_CTX;
class cMcpSession;

class cMcpHttpServer : public cThread {
private:
  struct cConnection {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
    };
  struct cHttpSession {
    std::shared_ptr<cMcpSession> session;
    std::shared_ptr<std::mutex> mutex;
    std::string keyName;
    time_t lastUse;
    };
  const cMcpConfig &config;
  int listenFd;
  SSL_CTX *ctx;
  std::atomic<bool> stopping;
  std::mutex mutex;
  std::list<cConnection> connections;
  std::mutex sessionMutex;
  std::map<std::string, cHttpSession> sessions;
  void Serve(int Fd, std::string Peer, std::shared_ptr<std::atomic<bool>> Done);
  std::string Request(const std::string &Method, const std::string &Path,
                      std::map<std::string, std::string> &Headers,
                      const std::string &Body, const std::string &Peer);
  void Reap(void);
protected:
  virtual void Action(void) override;
public:
  cMcpHttpServer(const cMcpConfig &Config);
  virtual ~cMcpHttpServer();
  bool Open(void);
  void Shutdown(void);
  };

#endif
