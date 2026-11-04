/*
 * session.h: one MCP client connection (JSON-RPC 2.0, one message per line)
 *
 * See the README file for copyright information and how to reach the author.
 */

#ifndef __MCP_SESSION_H
#define __MCP_SESSION_H

#include "config.h"
#include <string>
#include <vector>

struct cMcpTool {
  std::string name;        // MCP tool name, e.g. vdr_LSTT or epgsearch_LSTS
  std::string plugin;      // empty for the commands of VDR itself
  std::string command;     // SVDRP command, e.g. LSTT
  std::string description; // SVDRP help page
  };

class cMcpSession {
private:
  const cMcpConfig &config;
  std::vector<cMcpTool> tools;
  bool toolsBuilt;
  bool BuildTools(std::string &Error);
  std::string Call(const cMcpTool &Tool, const std::string &Args, bool &IsError);
  std::string Instructions(void);
public:
  cMcpSession(const cMcpConfig &Config);
  // Handles one JSON-RPC message, returns the reply (empty for notifications).
  std::string Handle(const std::string &Line);
  };

#endif
