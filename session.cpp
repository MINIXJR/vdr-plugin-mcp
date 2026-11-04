/*
 * session.cpp: one MCP client connection (JSON-RPC 2.0, one message per line)
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "session.h"
#include "journal.h"
#include "recording.h"
#include "svdrp.h"
#include <nlohmann/json.hpp>
#include <vdr/tools.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>

using json = nlohmann::json;

// --- Helpers ---------------------------------------------------------------

static std::string Trim(const std::string &s)
{
  size_t b = 0, e = s.size();
  while (b < e && isspace((unsigned char)s[b]))
        b++;
  while (e > b && isspace((unsigned char)s[e - 1]))
        e--;
  return s.substr(b, e - b);
}

static std::string Upper(std::string s)
{
  for (auto &c : s)
      c = toupper((unsigned char)c);
  return s;
}

static bool Match(const std::string &Pattern, const std::string &Text)
{
  std::string p = Upper(Pattern), t = Upper(Text);
  if (!p.empty() && p.back() == '*')
     return t.compare(0, p.size() - 1, p, 0, p.size() - 1) == 0;
  return p == t;
}

bool cMcpConfig::Denied(const std::string &Plugin, const std::string &Command) const
{
  for (auto &p : deny) {
      if (Match(p, Command))
         return true;
      if (!Plugin.empty() && Match(p, Plugin + "." + Command))
         return true;
      }
  return false;
}

static bool ValidCommand(const std::string &w)
{
  if (w.empty() || w.size() > 32)
     return false;
  for (char c : w) {
      if (!isalnum((unsigned char)c) && c != '_' && c != '-')
         return false;
      }
  return true;
}

static std::string ToolName(const std::string &Prefix, const std::string &Command)
{
  std::string n = Prefix + "_" + Command;
  for (auto &c : n) {
      if (!isalnum((unsigned char)c) && c != '_' && c != '-')
         c = '_';
      }
  return n.substr(0, 64);
}

static std::string Dump(const json &j)
{
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

static std::string Result(const json &Id, const json &Res)
{
  json j = json::object();
  j["jsonrpc"] = "2.0";
  j["id"] = Id;
  j["result"] = Res;
  return Dump(j);
}

static std::string Error(const json &Id, int Code, const std::string &Message)
{
  json e = json::object();
  e["code"] = Code;
  e["message"] = Message;
  json j = json::object();
  j["jsonrpc"] = "2.0";
  j["id"] = Id;
  j["error"] = e;
  return Dump(j);
}

// --- cMcpSession -----------------------------------------------------------

cMcpSession::cMcpSession(const cMcpConfig &Config)
:config(Config)
,toolsBuilt(false)
{
}

std::string cMcpSession::Instructions(void)
{
  char host[256] = "";
  gethostname(host, sizeof(host) - 1);
  return std::string("Each tool is one SVDRP command of the VDR on host '") + host +
         "'. Tools named vdr_* are commands of VDR itself, the others belong to the "
         "plugin in front of the underscore. Pass the arguments in 'args' exactly as "
         "SVDRP expects them (see the tool description). Channels, timers and recordings "
         "are addressed by the numbers the LST* commands return. Commands on the deny "
         "list of this server are not offered." +
         (config.logUnit.empty() ? std::string("") :
          " The tool vdr_log shows the system log of VDR (" + config.logUnit + ").") +
         " vdr_marks, vdr_setmarks and vdr_genindex work on recordings beyond SVDRP: "
         "editing marks (e.g. check the marks of markad before vdr_EDIT cuts) and the "
         "regeneration of the index, which also counts the errors of a recording.";
}

// --- Tools beyond SVDRP: editing marks, index ------------------------------

static const char *MARKS_TOOL    = "vdr_marks";
static const char *SETMARKS_TOOL = "vdr_setmarks";
static const char *GENINDEX_TOOL = "vdr_genindex";

static json IdProperty(void)
{
  json id = json::object();
  id["type"] = "integer";
  id["description"] = "id of the recording, as listed by vdr_LSTR";
  return id;
}

static json RecordingTool(const char *Name, const std::string &Description, bool WithMarks)
{
  json props = json::object();
  props["id"] = IdProperty();
  json required = json::array();
  required.push_back("id");
  if (WithMarks) {
     json item = json::object();
     item["type"] = "string";
     json marks = json::object();
     marks["type"] = "array";
     marks["items"] = item;
     marks["minItems"] = 1;
     marks["description"] = "all marks of the recording as h:mm:ss or h:mm:ss.ff, "
                            "alternating begin and end of the parts to keep";
     props["marks"] = marks;
     required.push_back("marks");
     }
  json schema = json::object();
  schema["type"] = "object";
  schema["properties"] = props;
  schema["required"] = required;
  json tool = json::object();
  tool["name"] = Name;
  tool["description"] = Description;
  tool["inputSchema"] = schema;
  return tool;
}

static std::vector<json> RecordingTools(const cMcpConfig &Config)
{
  std::vector<json> v;
  if (!Config.Denied("", MARKS_TOOL))
     v.push_back(RecordingTool(MARKS_TOOL,
       "Lists the editing marks of a recording (time, begin/end, number of sequences) "
       "and its length before and after cutting. SVDRP itself cannot show marks.", false));
  if (!Config.Denied("", SETMARKS_TOOL))
     v.push_back(RecordingTool(SETMARKS_TOOL,
       "Replaces ALL editing marks of a recording; VDR aligns them to the nearest "
       "I-frame. Refused while the recording is recorded, replayed or cut. "
       "Cut with vdr_EDIT afterwards.", true));
  if (!Config.Denied("", GENINDEX_TOOL))
     v.push_back(RecordingTool(GENINDEX_TOOL,
       "Regenerates the index file of a recording in the background, like vdr --genindex, "
       "and thereby counts its errors (line O of vdr_LSTR <id>; -1 means not checked). "
       "One recording at a time; refused while it is in use.", false));
  return v;
}

static bool GetId(const json &Args, int &Id)
{
  if (!Args.contains("id"))
     return false;
  if (Args["id"].is_number_integer()) {
     Id = Args["id"].get<int>();
     return true;
     }
  if (Args["id"].is_string()) {
     std::string s = Args["id"].get<std::string>();
     if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
     Id = atoi(s.c_str());
     return true;
     }
  return false;
}

static const char *LOG_TOOL = "vdr_log";
#define LOG_LINES_DEFAULT 100
#define LOG_LINES_MAX     1000

static json LogTool(const std::string &Unit)
{
  json lines = json::object();
  lines["type"] = "integer";
  lines["minimum"] = 1;
  lines["maximum"] = LOG_LINES_MAX;
  lines["description"] = "number of log lines, the newest ones (default 100)";
  json filter = json::object();
  filter["type"] = "string";
  filter["description"] = "only lines containing this text, case insensitive (optional)";
  json props = json::object();
  props["lines"] = lines;
  props["filter"] = filter;
  json schema = json::object();
  schema["type"] = "object";
  schema["properties"] = props;
  json tool = json::object();
  tool["name"] = LOG_TOOL;
  tool["description"] = "System log (systemd journal) of VDR, unit " + Unit +
                        ": the newest lines, oldest first, optionally filtered. Shows "
                        "recording errors, plugin messages, start and stop of VDR.";
  tool["inputSchema"] = schema;
  return tool;
}

bool cMcpSession::BuildTools(std::string &Error)
{
  tools.clear();
  cSvdrpClient svdrp(config.svdrpPort);
  if (!svdrp.Connect(Error))
     return false;
  int code;
  std::vector<std::string> lines;
  if (!svdrp.Command("HELP", code, lines) || code != 214) {
     Error = "SVDRP command HELP failed";
     return false;
     }
  // HELP lists the topics of VDR after "Topics:", then one block per plugin
  // starting with "Plugin <name> v<version> - <description>".
  struct cSection {
    std::string plugin;
    std::string text;
    std::vector<std::string> commands;
    };
  std::vector<cSection> sections;
  int current = -1;
  for (auto &l : lines) {
      if (l.empty())
         continue;
      if (isspace((unsigned char)l[0])) {
         if (current < 0)
            continue;
         size_t i = 0;
         while (i < l.size()) {
               while (i < l.size() && isspace((unsigned char)l[i]))
                     i++;
               size_t j = i;
               while (j < l.size() && !isspace((unsigned char)l[j]))
                     j++;
               if (j > i) {
                  std::string w = l.substr(i, j - i);
                  if (ValidCommand(w))
                     sections[current].commands.push_back(w);
                  }
               i = j;
               }
         continue;
         }
      std::string t = Trim(l);
      if (t == "Topics:") {
         sections.push_back({ "", "", {} });
         current = sections.size() - 1;
         }
      else if (t.compare(0, 7, "Plugin ") == 0) {
         size_t e = t.find(' ', 7);
         std::string name = t.substr(7, e == std::string::npos ? std::string::npos : e - 7);
         size_t d = t.find(" - ");
         sections.push_back({ name, d == std::string::npos ? "" : t.substr(d + 3), {} });
         current = sections.size() - 1;
         }
      else
         current = -1;
      }
  for (auto &s : sections) {
      if (s.plugin == "mcp")
         continue;
      for (auto &cmd : s.commands) {
          if (s.plugin.empty() && Upper(cmd) == "QUIT")
             continue; // closes the SVDRP connection, nothing to offer
          if (config.Denied(s.plugin, cmd))
             continue;
          std::string query = s.plugin.empty() ? "HELP " + cmd : "PLUG " + s.plugin + " HELP " + cmd;
          std::string help;
          if (svdrp.Command(query, code, lines) && code == 214) {
             for (auto &h : lines) {
                 if (h == "End of HELP info")
                    continue;
                 help += h + "\n";
                 }
             }
          cMcpTool tool;
          tool.plugin = s.plugin;
          tool.command = cmd;
          tool.name = ToolName(s.plugin.empty() ? "vdr" : s.plugin, cmd);
          tool.description = (s.plugin.empty() ? std::string("SVDRP command of VDR.") :
                              "SVDRP command of the plugin " + s.plugin +
                              (s.text.empty() ? std::string("") : " (" + s.text + ")") + ".") +
                             "\n" + help;
          tools.push_back(tool);
          }
      }
  toolsBuilt = true;
  isyslog("mcp: %d tools from SVDRP", (int)tools.size());
  return true;
}

std::string cMcpSession::Call(const cMcpTool &Tool, const std::string &Args, bool &IsError)
{
  IsError = true;
  std::string args = Args;
  for (auto &c : args) {
      if (c == '\r' || c == '\n')
         c = ' '; // one command only
      }
  args = Trim(args);
  std::string cmd = Tool.plugin.empty() ? Tool.command : "PLUG " + Tool.plugin + " " + Tool.command;
  if (!args.empty())
     cmd += " " + args;
  std::string error;
  cSvdrpClient svdrp(config.svdrpPort);
  if (!svdrp.Connect(error))
     return error;
  int code;
  std::vector<std::string> lines;
  if (!svdrp.Command(cmd, code, lines))
     return "SVDRP connection lost";
  std::string text;
  for (auto &l : lines) {
      if (!text.empty())
         text += "\n";
      text += l;
      }
  if (text.size() > config.maxReply) {
     size_t cut = config.maxReply;
     while (cut > 0 && (text[cut] & 0xC0) == 0x80) // do not split a UTF-8 character
           cut--;
     text = text.substr(0, cut) + "\n[reply cut after " + std::to_string(cut) + " bytes]";
     }
  IsError = code >= 400 && code < 600; // 9xx are the replies of plugins
  if (IsError)
     return std::to_string(code) + " " + text;
  return text.empty() ? std::to_string(code) : text;
}

std::string cMcpSession::Handle(const std::string &Line)
{
  json req;
  try {
    req = json::parse(Line);
    }
  catch (const std::exception &) {
    return Error(nullptr, -32700, "Parse error");
    }
  if (!req.is_object())
     return Error(nullptr, -32600, "Invalid request");
  if (!req.contains("id"))
     return ""; // notification, e.g. notifications/initialized
  json id = req["id"];
  try {
    std::string method = req.contains("method") && req["method"].is_string() ? req["method"].get<std::string>() : "";
    json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();

    if (method == "initialize") {
       std::string pv = params.contains("protocolVersion") && params["protocolVersion"].is_string() ?
                        params["protocolVersion"].get<std::string>() : "";
       static const char *Known[] = { "2024-11-05", "2025-03-26", "2025-06-18", NULL };
       bool known = false;
       for (int i = 0; Known[i]; i++) {
           if (pv == Known[i])
              known = true;
           }
       json tcap = json::object();
       tcap["listChanged"] = false;
       json caps = json::object();
       caps["tools"] = tcap;
       json info = json::object();
       info["name"] = "vdr-mcp";
       info["version"] = config.version;
       json r = json::object();
       r["protocolVersion"] = known ? pv : "2025-06-18";
       r["capabilities"] = caps;
       r["serverInfo"] = info;
       r["instructions"] = Instructions();
       return Result(id, r);
       }

    if (method == "ping")
       return Result(id, json::object());

    if (method == "tools/list") {
       std::string error;
       if (!toolsBuilt && !BuildTools(error))
          return Error(id, -32603, error);
       json list = json::array();
       for (auto &t : tools) {
           json arg = json::object();
           arg["type"] = "string";
           arg["description"] = "arguments of the SVDRP command exactly as SVDRP expects them, may be empty";
           json props = json::object();
           props["args"] = arg;
           json schema = json::object();
           schema["type"] = "object";
           schema["properties"] = props;
           json tool = json::object();
           tool["name"] = t.name;
           tool["description"] = t.description;
           tool["inputSchema"] = schema;
           list.push_back(tool);
           }
       if (!config.logUnit.empty())
          list.push_back(LogTool(config.logUnit));
       for (auto &t : RecordingTools(config))
           list.push_back(t);
       json r = json::object();
       r["tools"] = list;
       return Result(id, r);
       }

    if (method == "tools/call") {
       std::string name = params.contains("name") && params["name"].is_string() ? params["name"].get<std::string>() : "";
       if (name == LOG_TOOL && !config.logUnit.empty()) {
          int n = LOG_LINES_DEFAULT;
          std::string filter;
          if (params.contains("arguments") && params["arguments"].is_object()) {
             const json &a = params["arguments"];
             if (a.contains("lines") && a["lines"].is_number_integer())
                n = a["lines"].get<int>();
             else if (a.contains("lines") && a["lines"].is_string())
                n = atoi(a["lines"].get<std::string>().c_str());
             if (a.contains("filter") && a["filter"].is_string())
                filter = a["filter"].get<std::string>();
             }
          n = std::max(1, std::min(n, LOG_LINES_MAX));
          bool isError = true;
          std::string text = ReadJournal(config.logUnit, n, filter, isError);
          if (text.size() > config.maxReply) { // keep the newest lines
             size_t cut = text.size() - config.maxReply;
             size_t nl = text.find('\n', cut);
             text = "[older lines cut]\n" + (nl == std::string::npos ? text.substr(cut) : text.substr(nl + 1));
             }
          isyslog("mcp: %s %d%s%s -> %s", LOG_TOOL, n, filter.empty() ? "" : " ", filter.c_str(), isError ? "error" : "ok");
          json item = json::object();
          item["type"] = "text";
          item["text"] = text;
          json content = json::array();
          content.push_back(item);
          json r = json::object();
          r["content"] = content;
          r["isError"] = isError;
          return Result(id, r);
          }
       if ((name == MARKS_TOOL || name == SETMARKS_TOOL || name == GENINDEX_TOOL) && !config.Denied("", name)) {
          json a = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
          bool isError = true;
          std::string text;
          int recId = 0;
          if (!GetId(a, recId))
             text = "argument 'id' (number of the recording from vdr_LSTR) is missing or invalid";
          else if (name == MARKS_TOOL)
             text = ReadMarks(recId, isError);
          else if (name == GENINDEX_TOOL)
             text = StartGenIndex(recId, isError);
          else {
             std::vector<std::string> marks;
             if (a.contains("marks") && a["marks"].is_array()) {
                for (auto &m : a["marks"]) {
                    if (m.is_string())
                       marks.push_back(m.get<std::string>());
                    }
                }
             text = WriteMarks(recId, marks, isError);
             }
          isyslog("mcp: %s %d -> %s", name.c_str(), recId, isError ? "error" : "ok");
          json item = json::object();
          item["type"] = "text";
          item["text"] = text;
          json content = json::array();
          content.push_back(item);
          json r = json::object();
          r["content"] = content;
          r["isError"] = isError;
          return Result(id, r);
          }
       std::string error;
       if (!toolsBuilt && !BuildTools(error))
          return Error(id, -32603, error);
       std::string args;
       if (params.contains("arguments") && params["arguments"].is_object()) {
          const json &a = params["arguments"];
          if (a.contains("args") && a["args"].is_string())
             args = a["args"].get<std::string>();
          }
       const cMcpTool *tool = NULL;
       for (auto &t : tools) {
           if (t.name == name)
              tool = &t;
           }
       bool isError = true;
       std::string text = tool ? Call(*tool, args, isError) : "unknown tool: " + name;
       if (tool)
          isyslog("mcp: %s%s%s -> %s", tool->name.c_str(), args.empty() ? "" : " ", args.c_str(), isError ? "error" : "ok");
       json item = json::object();
       item["type"] = "text";
       item["text"] = text;
       json content = json::array();
       content.push_back(item);
       json r = json::object();
       r["content"] = content;
       r["isError"] = isError;
       return Result(id, r);
       }

    return Error(id, -32601, "Method not found: " + method);
    }
  catch (const std::exception &e) {
    return Error(id, -32603, e.what());
    }
}
