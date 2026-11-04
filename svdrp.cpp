/*
 * svdrp.cpp: minimal SVDRP client for the local VDR
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "svdrp.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#define SVDRP_TIMEOUT_MS 20000

cSvdrpClient::cSvdrpClient(int Port)
:port(Port)
,fd(-1)
{
}

cSvdrpClient::~cSvdrpClient()
{
  Close();
}

bool cSvdrpClient::Send(const std::string &Data)
{
  size_t done = 0;
  while (done < Data.size()) {
        ssize_t n = send(fd, Data.data() + done, Data.size() - done, MSG_NOSIGNAL);
        if (n < 0) {
           if (errno == EINTR)
              continue;
           return false;
           }
        done += n;
        }
  return true;
}

bool cSvdrpClient::ReadLine(std::string &Line)
{
  for (;;) {
      size_t pos = buffer.find('\n');
      if (pos != std::string::npos) {
         Line = buffer.substr(0, pos);
         buffer.erase(0, pos + 1);
         if (!Line.empty() && Line.back() == '\r')
            Line.pop_back();
         return true;
         }
      pollfd p = { fd, POLLIN, 0 };
      int r = poll(&p, 1, SVDRP_TIMEOUT_MS);
      if (r < 0 && errno == EINTR)
         continue;
      if (r <= 0)
         return false;
      char tmp[4096];
      ssize_t n = read(fd, tmp, sizeof(tmp));
      if (n <= 0)
         return false;
      buffer.append(tmp, n);
      }
}

bool cSvdrpClient::Connect(std::string &Error)
{
  Close();
  fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
     Error = std::string("socket: ") + strerror(errno);
     return false;
     }
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (sockaddr *)&a, sizeof(a)) < 0) {
     Error = "cannot connect to SVDRP port " + std::to_string(port) + ": " + strerror(errno);
     close(fd);
     fd = -1;
     return false;
     }
  std::string line;
  if (!ReadLine(line) || line.compare(0, 3, "220") != 0) {
     Error = line.empty() ? "no SVDRP greeting" : "SVDRP refused the connection: " + line;
     close(fd);
     fd = -1;
     return false;
     }
  return true;
}

void cSvdrpClient::Close(void)
{
  if (fd >= 0) {
     Send("QUIT\r\n");
     close(fd);
     fd = -1;
     }
  buffer.clear();
}

bool cSvdrpClient::Command(const std::string &Cmd, int &Code, std::vector<std::string> &Lines)
{
  Lines.clear();
  Code = 0;
  if (fd < 0 || !Send(Cmd + "\r\n"))
     return false;
  for (;;) {
      std::string line;
      if (!ReadLine(line))
         return false;
      if (line.size() < 3)
         continue;
      Code = atoi(line.substr(0, 3).c_str());
      Lines.push_back(line.size() > 4 ? line.substr(4) : "");
      if (line.size() == 3 || line[3] != '-')
         return true;
      }
}
