/*
 * journal.cpp: read the systemd journal of VDR for the tool vdr_log
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "journal.h"
#include <systemd/sd-journal.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <vector>

#define MAX_SCAN 200000 // entries looked at when searching with a filter

static std::string Lower(std::string s)
{
  for (auto &c : s)
      c = tolower((unsigned char)c);
  return s;
}

static std::string Field(sd_journal *j, const char *Name)
{
  const void *data;
  size_t length;
  size_t n = strlen(Name) + 1; // "NAME="
  if (sd_journal_get_data(j, Name, &data, &length) < 0 || length < n)
     return "";
  return std::string((const char *)data + n, length - n);
}

std::string ReadJournal(const std::string &Unit, int Lines, const std::string &Filter, bool &IsError)
{
  IsError = true;
  sd_journal *j = NULL;
  int r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM);
  if (r < 0)
     return std::string("cannot open the journal: ") + strerror(-r);
  std::string match = "_SYSTEMD_UNIT=" + Unit;
  sd_journal_add_match(j, match.c_str(), 0);
  sd_journal_seek_tail(j);
  std::string filter = Lower(Filter);
  std::vector<std::string> lines;
  int scanned = 0;
  while ((int)lines.size() < Lines && scanned < MAX_SCAN && sd_journal_previous(j) > 0) {
        scanned++;
        std::string msg = Field(j, "MESSAGE");
        if (!filter.empty() && Lower(msg).find(filter) == std::string::npos)
           continue;
        uint64_t usec = 0;
        sd_journal_get_realtime_usec(j, &usec);
        time_t t = usec / 1000000;
        struct tm tm;
        localtime_r(&t, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%b %d %H:%M:%S", &tm);
        std::string ident = Field(j, "SYSLOG_IDENTIFIER");
        std::string pid = Field(j, "_PID");
        lines.push_back(std::string(ts) + " " + ident + (pid.empty() ? "" : "[" + pid + "]") + ": " + msg);
        }
  sd_journal_close(j);
  IsError = false;
  if (lines.empty()) {
     return "no journal entries of " + Unit +
            (Filter.empty() ? "" : " containing '" + Filter + "'") +
            ". If there should be some, the user vdr may lack the right to read the "
            "journal: adduser vdr systemd-journal, then restart VDR.";
     }
  std::reverse(lines.begin(), lines.end());
  std::string text;
  for (auto &l : lines)
      text += l + "\n";
  return text;
}
