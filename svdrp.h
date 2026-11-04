/*
 * svdrp.h: minimal SVDRP client for the local VDR
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_SVDRP_H
#define __MCP_SVDRP_H

#include <string>
#include <vector>

class cSvdrpClient {
private:
  int port;
  int fd;
  std::string buffer;
  bool ReadLine(std::string &Line);
  bool Send(const std::string &Data);
public:
  cSvdrpClient(int Port);
  ~cSvdrpClient();
  bool Connect(std::string &Error);
  void Close(void);
  // Sends one command. Code is the reply code, Lines the reply texts without
  // the code. Returns false if the connection failed.
  bool Command(const std::string &Cmd, int &Code, std::vector<std::string> &Lines);
  };

#endif
