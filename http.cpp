/*
 * http.cpp: MCP "Streamable HTTP" transport with TLS and API keys
 *
 * Only what MCP needs: POST /mcp with one JSON-RPC message, answered with
 * application/json (no server-sent events), DELETE /mcp ends a session.
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "http.h"
#include "keys.h"
#include "session.h"
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <vdr/tools.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <strings.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>

#define MAX_HEADER      16384
#define MAX_BODY        (4 * 1024 * 1024)
#define IDLE_TIMEOUT_S  60
#define IO_TIMEOUT_S    30
#define SESSION_IDLE_S  3600
#define MAX_SESSIONS    64

// --- Plain or TLS stream ---------------------------------------------------

class cStream {
private:
  int fd;
  SSL *ssl;
public:
  cStream(int Fd, SSL *Ssl) : fd(Fd), ssl(Ssl) {}
  // > 0 bytes, 0 closed, -1 error, -2 timeout
  int Read(char *Buf, int Size, int TimeoutMs)
  {
    if (!(ssl && SSL_pending(ssl) > 0)) {
       pollfd p = { fd, POLLIN, 0 };
       int r = poll(&p, 1, TimeoutMs);
       if (r == 0 || (r < 0 && errno == EINTR))
          return -2;
       if (r < 0)
          return -1;
       }
    if (ssl) {
       int n = SSL_read(ssl, Buf, Size);
       if (n > 0)
          return n;
       int e = SSL_get_error(ssl, n);
       if (e == SSL_ERROR_ZERO_RETURN)
          return 0;
       if (e == SSL_ERROR_WANT_READ)
          return -2;
       return -1;
       }
    ssize_t n = read(fd, Buf, Size);
    if (n < 0)
       return (errno == EINTR || errno == EAGAIN) ? -2 : -1;
    return n;
  }
  bool Write(const std::string &Data)
  {
    size_t done = 0;
    while (done < Data.size()) {
          if (ssl) {
             int n = SSL_write(ssl, Data.data() + done, Data.size() - done);
             if (n <= 0)
                return false;
             done += n;
             }
          else {
             ssize_t n = send(fd, Data.data() + done, Data.size() - done, MSG_NOSIGNAL);
             if (n < 0) {
                if (errno == EINTR)
                   continue;
                return false;
                }
             done += n;
             }
          }
    return true;
  }
  };

static std::string Lower(std::string s)
{
  for (auto &c : s)
      c = tolower((unsigned char)c);
  return s;
}

static std::string Trim(const std::string &s)
{
  size_t b = s.find_first_not_of(" \t");
  size_t e = s.find_last_not_of(" \t\r");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

static std::string Response(int Code, const char *Reason, const std::string &ContentType,
                            const std::string &Body, const std::string &Extra = "")
{
  std::string r = "HTTP/1.1 " + std::to_string(Code) + " " + Reason + "\r\n";
  r += "Server: vdr-mcp\r\n";
  r += "Cache-Control: no-store\r\n";
  if (!ContentType.empty())
     r += "Content-Type: " + ContentType + "\r\n";
  r += "Content-Length: " + std::to_string(Body.size()) + "\r\n";
  r += Extra;
  r += "\r\n";
  r += Body;
  return r;
}

static std::string Text(int Code, const char *Reason, const std::string &Message, const std::string &Extra = "")
{
  return Response(Code, Reason, "text/plain; charset=utf-8", Message + "\n", Extra);
}

// --- cMcpHttpServer --------------------------------------------------------

cMcpHttpServer::cMcpHttpServer(const cMcpConfig &Config)
:cThread("mcp http server")
,config(Config)
,listenFd(-1)
,ctx(NULL)
,stopping(false)
{
}

cMcpHttpServer::~cMcpHttpServer()
{
  Shutdown();
}

bool cMcpHttpServer::Open(void)
{
  std::string cert = config.certFile.empty() ? config.configDir + "/cert.pem" : config.certFile;
  std::string key = config.keyFile.empty() ? config.configDir + "/key.pem" : config.keyFile;
  if (config.httpTls) {
     std::string error;
     if (config.certFile.empty() && !EnsureCertificate(cert, key, error)) {
        esyslog("mcp: TLS certificate: %s", error.c_str());
        return false;
        }
     ctx = SSL_CTX_new(TLS_server_method());
     if (!ctx) {
        esyslog("mcp: cannot create the TLS context");
        return false;
        }
     SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
     if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1 ||
         SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
         SSL_CTX_check_private_key(ctx) != 1) {
        esyslog("mcp: cannot load the TLS certificate %s or key %s", cert.c_str(), key.c_str());
        SSL_CTX_free(ctx);
        ctx = NULL;
        return false;
        }
     }
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
  a.sin_port = htons(config.httpPort);
  if (inet_pton(AF_INET, config.httpBind.c_str(), &a.sin_addr) != 1 ||
      bind(listenFd, (sockaddr *)&a, sizeof(a)) < 0 || listen(listenFd, 8) < 0) {
     esyslog("mcp: cannot listen on %s:%d: %s", config.httpBind.c_str(), config.httpPort, strerror(errno));
     close(listenFd);
     listenFd = -1;
     return false;
     }
  if (ctx)
     isyslog("mcp: HTTPS on %s:%d/mcp, certificate %s (SHA-256 %s)", config.httpBind.c_str(), config.httpPort,
             cert.c_str(), CertFingerprint(cert).c_str());
  else
     esyslog("mcp: WARNING: plain HTTP on %s:%d/mcp, keys travel unencrypted", config.httpBind.c_str(), config.httpPort);
  if (McpKeys.Count() == 0)
     isyslog("mcp: no API key yet, create one with: svdrpsend PLUG mcp KEYADD <name> ro|rw");
  Start();
  return true;
}

void cMcpHttpServer::Reap(void)
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

void cMcpHttpServer::Action(void)
{
  while (Running() && !stopping) {
        Reap();
        pollfd p = { listenFd, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0)
           continue;
        sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = accept4(listenFd, (sockaddr *)&peer, &len, SOCK_CLOEXEC);
        if (fd < 0)
           continue;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mutex);
        if (connections.size() >= 32) {
           close(fd);
           continue;
           }
        connections.push_back(cConnection{ std::thread(&cMcpHttpServer::Serve, this, fd, std::string(ip), done), done });
        }
}

void cMcpHttpServer::Serve(int Fd, std::string Peer, std::shared_ptr<std::atomic<bool>> Done)
{
  timeval tv = { IO_TIMEOUT_S, 0 };
  setsockopt(Fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(Fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  SSL *ssl = NULL;
  if (ctx) {
     ssl = SSL_new(ctx);
     SSL_set_fd(ssl, Fd);
     if (SSL_accept(ssl) <= 0) {
        isyslog("mcp: TLS handshake with %s failed", Peer.c_str());
        SSL_free(ssl);
        close(Fd);
        *Done = true;
        return;
        }
     }
  cStream stream(Fd, ssl);
  std::string buffer;
  char tmp[8192];
  bool keep = true;
  while (keep && !stopping) {
        // header
        size_t end;
        int idle = 0;
        while (keep && (end = buffer.find("\r\n\r\n")) == std::string::npos) {
              if (buffer.size() > MAX_HEADER) {
                 stream.Write(Text(431, "Request Header Fields Too Large", "header too large", "Connection: close\r\n"));
                 keep = false;
                 break;
                 }
              int n = stream.Read(tmp, sizeof(tmp), 1000);
              if (n == -2) {
                 if (stopping || ++idle >= IDLE_TIMEOUT_S)
                    keep = false;
                 continue;
                 }
              if (n <= 0) {
                 keep = false;
                 break;
                 }
              idle = 0;
              buffer.append(tmp, n);
              }
        if (!keep)
           break;
        std::string head = buffer.substr(0, end);
        buffer.erase(0, end + 4);
        std::istringstream hs(head);
        std::string line, method, path, version;
        std::getline(hs, line);
        std::istringstream rl(line);
        rl >> method >> path >> version;
        std::map<std::string, std::string> headers;
        while (std::getline(hs, line)) {
              size_t c = line.find(':');
              if (c != std::string::npos)
                 headers[Lower(Trim(line.substr(0, c)))] = Trim(line.substr(c + 1));
              }
        if (headers.count("transfer-encoding")) {
           stream.Write(Text(501, "Not Implemented", "chunked requests are not supported", "Connection: close\r\n"));
           break;
           }
        size_t length = headers.count("content-length") ? strtoul(headers["content-length"].c_str(), NULL, 10) : 0;
        if (length > MAX_BODY) {
           stream.Write(Text(413, "Payload Too Large", "request too large", "Connection: close\r\n"));
           break;
           }
        while (buffer.size() < length) {
              int n = stream.Read(tmp, sizeof(tmp), IO_TIMEOUT_S * 1000);
              if (n <= 0) {
                 keep = false;
                 break;
                 }
              buffer.append(tmp, n);
              }
        if (!keep)
           break;
        std::string body = buffer.substr(0, length);
        buffer.erase(0, length);
        bool closeAfter = version == "HTTP/1.0" || Lower(headers["connection"]) == "close";
        std::string reply = Request(method, path, headers, body, Peer);
        if (!stream.Write(reply) || closeAfter)
           break;
        }
  if (ssl) {
     SSL_shutdown(ssl);
     SSL_free(ssl);
     }
  close(Fd);
  *Done = true;
}

std::string cMcpHttpServer::Request(const std::string &Method, const std::string &Path,
                                    std::map<std::string, std::string> &Headers,
                                    const std::string &Body, const std::string &Peer)
{
  std::string path = Path.substr(0, Path.find('?'));
  if (path != "/mcp" && path != "/mcp/")
     return Text(404, "Not Found", "the MCP endpoint is /mcp");
  // DNS rebinding protection: browsers send Origin, MCP clients do not
  if (Headers.count("origin")) {
     isyslog("mcp: request from %s with Origin '%s' refused", Peer.c_str(), Headers["origin"].c_str());
     return Text(403, "Forbidden", "requests with an Origin header are not accepted");
     }
  std::string auth = Headers["authorization"];
  std::string keyName;
  bool readOnly = true;
  if (auth.size() <= 7 || strncasecmp(auth.c_str(), "Bearer ", 7) != 0 ||
      !McpKeys.Check(Trim(auth.substr(7)), keyName, readOnly)) {
     isyslog("mcp: request from %s without a valid API key", Peer.c_str());
     sleep(1); // slows down guessing
     return Text(401, "Unauthorized", "missing or wrong API key (header Authorization: Bearer <key>)",
                 "WWW-Authenticate: Bearer realm=\"vdr-mcp\"\r\n");
     }
  std::string sid = Headers["mcp-session-id"];
  time_t now = time(NULL);
  if (Method == "DELETE") {
     std::lock_guard<std::mutex> lock(sessionMutex);
     auto it = sessions.find(sid);
     if (it != sessions.end() && it->second.keyName == keyName)
        sessions.erase(it);
     return Response(204, "No Content", "", "");
     }
  if (Method != "POST")
     return Text(405, "Method Not Allowed", "use POST (no server-sent events)", "Allow: POST, DELETE\r\n");
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(Body);
    }
  catch (const std::exception &) {
    return Response(400, "Bad Request", "application/json",
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
    }
  if (!req.is_object())
     return Response(400, "Bad Request", "application/json",
                     "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid request\"}}");
  std::string method = req.contains("method") && req["method"].is_string() ? req["method"].get<std::string>() : "";
  std::shared_ptr<cMcpSession> session;
  std::shared_ptr<std::mutex> sessionLock;
  std::string extra;
  if (method == "initialize") {
     sid = RandomHex(16);
     session = std::make_shared<cMcpSession>(config, readOnly);
     sessionLock = std::make_shared<std::mutex>();
     std::lock_guard<std::mutex> lock(sessionMutex);
     for (auto it = sessions.begin(); it != sessions.end(); ) {
         if (now - it->second.lastUse > SESSION_IDLE_S)
            it = sessions.erase(it);
         else
            ++it;
         }
     while (sessions.size() >= MAX_SESSIONS) {
           auto oldest = sessions.begin();
           for (auto it = sessions.begin(); it != sessions.end(); ++it) {
               if (it->second.lastUse < oldest->second.lastUse)
                  oldest = it;
               }
           sessions.erase(oldest);
           }
     sessions[sid] = cHttpSession{ session, sessionLock, keyName, now };
     extra = "Mcp-Session-Id: " + sid + "\r\n";
     isyslog("mcp: HTTP session for key '%s' from %s%s", keyName.c_str(), Peer.c_str(), readOnly ? " (read only)" : "");
     }
  else {
     if (sid.empty())
        return Text(400, "Bad Request", "header Mcp-Session-Id missing, send initialize first");
     std::lock_guard<std::mutex> lock(sessionMutex);
     auto it = sessions.find(sid);
     if (it == sessions.end() || it->second.keyName != keyName)
        return Text(404, "Not Found", "unknown or expired session, send initialize again");
     it->second.lastUse = now;
     session = it->second.session;
     sessionLock = it->second.mutex;
     }
  std::string reply;
  {
    std::lock_guard<std::mutex> lock(*sessionLock);
    reply = session->Handle(Body);
  }
  if (reply.empty())
     return Response(202, "Accepted", "", "", extra);
  return Response(200, "OK", "application/json", reply, extra);
}

void cMcpHttpServer::Shutdown(void)
{
  stopping = true;
  Cancel(3);
  if (listenFd >= 0) {
     close(listenFd);
     listenFd = -1;
     }
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto &c : connections) {
        if (c.thread.joinable())
           c.thread.join();
        }
    connections.clear();
  }
  {
    std::lock_guard<std::mutex> lock(sessionMutex);
    sessions.clear();
  }
  if (ctx) {
     SSL_CTX_free(ctx);
     ctx = NULL;
     }
}
