/*
 * recording.cpp: tools on recordings that SVDRP does not offer
 *                (editing marks, index regeneration)
 *
 * See the README file for copyright information and how to reach the author.
 */

#include "recording.h"
#include <vdr/recording.h>
#include <vdr/thread.h>
#include <vdr/tools.h>
#include <cctype>
#include <ctime>

struct cRecordingData {
  bool ok = false;
  std::string error;
  std::string fileName;
  std::string name;
  double fps = DEFAULTFRAMESPERSECOND;
  bool pes = false;
  int use = ruNone;
  };

static cRecordingData Find(int Id)
{
  cRecordingData d;
  LOCK_RECORDINGS_READ;
  const cRecording *r = Recordings->GetById(Id);
  if (!r) {
     d.error = "recording " + std::to_string(Id) + " not found (see vdr_LSTR)";
     return d;
     }
  d.fileName = r->FileName();
  d.name = r->Name();
  d.fps = r->FramesPerSecond();
  d.pes = r->IsPesRecording();
  d.use = r->IsInUse();
  d.ok = true;
  return d;
}

static std::string Usage(int Use)
{
  std::string s;
  if (Use & ruTimer)   s += " recording";
  if (Use & ruReplay)  s += " replay";
  if (Use & ruCut)     s += " cutting";
  if (Use & ruMove)    s += " moving";
  if (Use & ruCopy)    s += " copying";
  if (Use & ruPending) s += " pending";
  return s.empty() ? " busy" : s;
}

static std::string Hmsf(int Frames, double Fps)
{
  return *IndexToHMSF(Frames, true, Fps);
}

// --- Marks -----------------------------------------------------------------

std::string ReadMarks(int Id, bool &IsError)
{
  IsError = true;
  cRecordingData d = Find(Id);
  if (!d.ok)
     return d.error;
  int length = cIndexFile::GetLength(d.fileName.c_str(), d.pes); // frames, < 0 without index
  cMarks marks;
  marks.Load(d.fileName.c_str(), d.fps, d.pes);
  IsError = false;
  std::string text = d.name + "\n";
  text += "length: " + (length > 0 ? Hmsf(length, d.fps) : std::string("unknown (no index, see vdr_genindex)")) + "\n";
  if (marks.Count() == 0)
     return text + "no editing marks";
  int n = 0, kept = 0, begin = -1;
  for (const cMark *m = marks.First(); m; m = marks.Next(m)) {
      bool isBegin = (n % 2) == 0;
      n++;
      text += std::to_string(n) + ". " + Hmsf(m->Position(), d.fps) + (isBegin ? " begin" : " end");
      if (m->Comment() && *m->Comment())
         text += std::string(" ") + m->Comment();
      text += "\n";
      if (isBegin)
         begin = m->Position();
      else if (begin >= 0) {
         kept += m->Position() - begin;
         begin = -1;
         }
      }
  if (begin >= 0 && length > 0) // last sequence runs to the end
     kept += length - begin;
  text += std::to_string(n) + " marks, " + std::to_string(marks.GetNumSequences()) + " sequence(s)";
  if (length > 0)
     text += ", length after cutting " + Hmsf(kept, d.fps);
  return text;
}

static bool ValidTime(const std::string &s)
{
  if (s.empty())
     return false;
  for (char c : s) {
      if (!isdigit((unsigned char)c) && c != ':' && c != '.')
         return false;
      }
  return true;
}

std::string WriteMarks(int Id, const std::vector<std::string> &Marks, bool &IsError)
{
  IsError = true;
  if (Marks.empty())
     return "no marks given";
  cRecordingData d = Find(Id);
  if (!d.ok)
     return d.error;
  if (d.use != ruNone)
     return "recording " + std::to_string(Id) + " is in use:" + Usage(d.use);
  int length = cIndexFile::GetLength(d.fileName.c_str(), d.pes);
  if (length <= 0)
     return "recording " + std::to_string(Id) + " has no index, run vdr_genindex first";
  std::vector<int> positions;
  for (auto &s : Marks) {
      if (!ValidTime(s))
         return "invalid time '" + s + "', expected h:mm:ss or h:mm:ss.ff";
      int p = HMSFToIndex(s.c_str(), d.fps);
      if (p < 0 || p > length)
         return "time " + s + " is outside the recording (length " + Hmsf(length, d.fps) + ")";
      positions.push_back(p);
      }
  cMarks marks;
  marks.Load(d.fileName.c_str(), d.fps, d.pes); // sets the file names, local object only
  while (cMark *m = marks.First()) // cConfig::Clear() is private
        marks.Del(m);
  for (int p : positions)
      marks.Add(p);
  marks.Align();
  if (!marks.Save())
     return "cannot save the marks of recording " + std::to_string(Id);
  isyslog("mcp: %d editing marks set for %s", (int)positions.size(), d.name.c_str());
  return ReadMarks(Id, IsError);
}

// --- Index regeneration ----------------------------------------------------

class cGenIndexJob : public cThread {
private:
  std::string fileName;
  std::string name;
protected:
  virtual void Action(void) override
  {
    bool ok = GenerateIndex(fileName.c_str());
    isyslog("mcp: genindex %s for %s", ok ? "done" : "FAILED", name.c_str());
  }
public:
  time_t started;
  std::string Name(void) const { return name; }
  cGenIndexJob(const std::string &FileName, const std::string &Name)
  :cThread("mcp genindex")
  ,fileName(FileName)
  ,name(Name)
  ,started(time(NULL))
  {
    Start();
  }
  virtual ~cGenIndexJob() { Cancel(5); }
  };

static cMutex jobMutex;
static cGenIndexJob *job = NULL;

std::string StartGenIndex(int Id, bool &IsError)
{
  IsError = true;
  cRecordingData d = Find(Id);
  if (!d.ok)
     return d.error;
  if (d.pes)
     return "recording " + std::to_string(Id) + " is a PES recording, only TS recordings are supported";
  if (d.use != ruNone)
     return "recording " + std::to_string(Id) + " is in use:" + Usage(d.use);
  cMutexLock lock(&jobMutex);
  if (job && job->Active()) {
     char t[16];
     struct tm tm;
     localtime_r(&job->started, &tm);
     strftime(t, sizeof(t), "%H:%M", &tm);
     return "busy: the index of '" + job->Name() + "' is being regenerated since " + t + ", try again later";
     }
  delete job;
  job = new cGenIndexJob(d.fileName, d.name);
  isyslog("mcp: genindex started for %s", d.name.c_str());
  IsError = false;
  return "index regeneration started for '" + d.name + "'. It reads the whole recording "
         "(a few minutes per hour). When it is done, vdr_LSTR " + std::to_string(Id) +
         " shows the errors in the line O, and vdr_log shows 'mcp: genindex done'.";
}

bool GenIndexRunning(void)
{
  cMutexLock lock(&jobMutex);
  return job && job->Active();
}

void StopGenIndex(void)
{
  cMutexLock lock(&jobMutex);
  delete job;
  job = NULL;
}
