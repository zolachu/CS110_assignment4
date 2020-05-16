/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-parser/stsh-parse-exception.h"
#include "stsh-signal.h"
#include "stsh-job-list.h"
#include "stsh-job.h"
#include "stsh-process.h"
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>  // for fork
#include <signal.h>  // for kill
#include <sys/wait.h>
#include <assert.h>
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it
static void fgBuiltin(const pipeline& pipeline, size_t index);
static void bgBuiltin(const pipeline& pipeline, size_t index);
static void SHCBuiltin(const pipeline& pipeline, size_t index);


/**
 * Function: handleBuiltin
 * -----------------------
 * Examines the leading command of the provided pipeline to see if
 * it's a shell builtin, and if so, handles and executes it.  handleBuiltin
 * returns true if the command is a builtin, and false otherwise.
 */
static const string kSupportedBuiltins[] = {"quit", "exit", "fg", "bg", "slay", "halt", "cont", "jobs"};
static const size_t kNumSupportedBuiltins = sizeof(kSupportedBuiltins)/sizeof(kSupportedBuiltins[0]);
static bool handleBuiltin(const pipeline& pipeline) {
  const string& command = pipeline.commands[0].command;
  auto iter = find(kSupportedBuiltins, kSupportedBuiltins + kNumSupportedBuiltins, command);
  if (iter == kSupportedBuiltins + kNumSupportedBuiltins) return false;
  size_t index = iter - kSupportedBuiltins;

  switch (index) {
  case 0:
  case 1: exit(0);
  case 2: fgBuiltin(pipeline, index); break;
  case 3: bgBuiltin(pipeline, index); break;
  case 4: case 5: case 6: SHCBuiltin(pipeline, index); break;
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }
  
  return true;
}

/************************************************************************************************************/
/* Builtins */
/************************************************************************************************************/



/**
 * Function: fgBuiltin
 * -------------------------
 *
 */
static void fgBuiltin(const pipeline& pipeline, size_t index) {
  char* first = pipeline.commands[0].tokens[0];
  if (first == NULL)  throw STSHException("Usage: fg <jobid>.");
  pid_t num = atoi(first);
  char* ptr;
  long ret = strtol(first, &ptr, 10);
  if ((strlen(first) > 0 && strlen(ptr) > 0) || ret < 0) throw STSHException("Usage: fg <jobid>.");
  if (!joblist.containsJob(num)) throw STSHException("fg " + to_string(num) + ":  No such job.");
  STSHJob& job = joblist.getJob(num);
  vector<STSHProcess>& processes = job.getProcesses();
  sigset_t mask, existing;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGTSTP);
  sigaddset(&mask, SIGCONT);
  sigprocmask(SIG_BLOCK, &mask, &existing);
  for (auto process: processes) {
    if (kill(process.getID(), SIGCONT) == 0) job.setState(kForeground);
  }
  joblist.synchronize(job);
  while(joblist.hasForegroundJob()) sigsuspend(&existing);
  sigprocmask(SIG_UNBLOCK, &mask, NULL);
}


/**
 * Function: bgBuiltin
 * ----------------------
 *
 */
static void bgBuiltin(const pipeline& pipeline, size_t index) {
  char* first = pipeline.commands[0].tokens[0];
  if (first == NULL) throw STSHException("Usage: bg <jobid>.");
  pid_t num = atoi(first);
  char* ptr;
  long ret = strtol(first, &ptr, 10);
  if ((strlen(first) > 0 && strlen(ptr) > 0) || ret < 0) throw STSHException("Usage: bg <jobid>.");
  if (!joblist.containsJob(num)) throw STSHException("bg " + to_string(num) + ":  No such job.");
  STSHJob& job = joblist.getJob(num);
  vector<STSHProcess>& processes = job.getProcesses();
  for (auto process: processes) kill(process.getID(), SIGCONT);
  joblist.synchronize(job);
}

/**
 * Function: SHCBuiltin
 * ----------------------
 * Support for Slay, Halt, Continue builtins
 */

static void SHCBuiltin(const pipeline& pipeline, size_t index){
  char* first = pipeline.commands[0].tokens[0];
  char* second = pipeline.commands[0].tokens[1];
  int killer;
  switch(index) {
  case 4: killer = SIGKILL;
  case 5: killer = SIGSTOP;
  case 6: killer = SIGCONT;
  }
  string builtin;
  switch(index) {
  case 4: builtin = "slay";
  case 5: builtin = "halt";
  case 6: builtin = "cont";
  }
  if (first == NULL)  throw STSHException("Usage: " + builtin + " <jobid> <index> | <pid>.");
  pid_t num = atoi(first);
  char* ptr;
  long ret = strtol(first, &ptr, 10);
  if (second == NULL) {
    if ((strlen(first) > 0 && strlen(ptr) > 0) || ret < 0) throw STSHException("Usage: bg <jobid>.");
    if (!joblist.containsProcess(num)) throw STSHException("No process with pid " + to_string(num) + ".");
    STSHJob& job = joblist.getJobWithProcess(num);
    vector<STSHProcess>& processes = job.getProcesses();
    for (auto process: processes) kill(process.getID(), killer);
  } else if (second != NULL) {
    if (!joblist.containsJob(num)) throw STSHException("No job with id of " + to_string(num) + ".");
    STSHJob& job = joblist.getJob(num);
    pid_t pid = atoi(second);
    if (!job.containsProcess(pid)) throw STSHException("No process pid " + to_string(pid) + ".");
    STSHProcess process = job.getProcess(pid);
    kill(process.getID(), killer);
  }
}


/************************************************************************************************************/
/* Signal Handlers */
/************************************************************************************************************/


/**
 * Function: reapChild
 * -----------------------
 * reap any children who has terminated/stopped.
 * and update the process state.
 */

void sigchldHandler(int sig) {
  while(1) {
    pid_t pid;
    int status;
    STSHProcessState state;
    pid = waitpid(-1, &status, WNOHANG|WUNTRACED|WCONTINUED);
    if (pid <= 0) break;
    if(WIFEXITED(status))  state = kTerminated;
    if (WIFCONTINUED(status))  state = kRunning;
    if (WIFSIGNALED(status))  state = kTerminated;
    if (WIFSTOPPED(status))  state = kStopped;

    STSHJob& job = joblist.getJobWithProcess(pid);
    assert(job.containsProcess(pid));
    job.getProcess(pid).setState(state);
    joblist.synchronize(job);
  }
}


/**
 * Function: siginHandler
 * ---------------------------------------
 * Custom Handler to forward SIGINT to the
 * foreground job, if exist.
 */

void sigintHandler(int sig) {
  if (joblist.hasForegroundJob()) {
    STSHJob& job = joblist.getForegroundJob();
    std::vector<STSHProcess>& processes = job.getProcesses();
    for (auto process: processes) kill(process.getID(), SIGINT);
  }
}

/**
 * Function: sigtstpHandler
 * -----------------------------------------
 *  Custom Handler to forward SIGTSTP to the
 * foreground job, if exist.
 */

void sigtstpHandler(int sig) {
  if (joblist.hasForegroundJob()) {
    STSHJob& job = joblist.getForegroundJob();
    std::vector<STSHProcess>& processes = job.getProcesses();
    for (auto process: processes) kill(process.getID(), SIGTSTP);
  }
}

/**
 * Function: installSignalHandlers
 * -------------------------------
 * Installs user-defined signals handlers for four signals
 * (once you've implemented signal handlers for SIGCHLD, 
 * SIGINT, and SIGTSTP, you'll add more installSignalHandler calls) and 
 * ignores two others.
 *
 * installSignalHandler is a wrapper around a more robust version of the
 * signal function we've been using all quarter.  Check out stsh-signal.cc
 * to see how it works.
 */
static void installSignalHandlers() {
  installSignalHandler(SIGQUIT, [](int sig) { exit(0); });
  installSignalHandler(SIGTTIN, SIG_IGN);
  installSignalHandler(SIGTTOU, SIG_IGN);
  installSignalHandler(SIGCHLD, sigchldHandler);
  installSignalHandler(SIGINT, sigintHandler);
  installSignalHandler(SIGTSTP, sigtstpHandler);
}

/************************************************************************************************************/
/* Dup2, Print Background process  */
/************************************************************************************************************/


/**
 * Function: Dup2
 * ----------------
 */
void Dup2(int infd1, int infd2) {
  dup2(infd1, infd2) ;
  close(infd1) ;
}

/**
 * Function: Close
 * ---------------
 */
void Close(int* fd) {
  close(fd[0]);
  close(fd[1]);
}

/**
 * Function: printBG
 * --------------------------
 */
void printBG(STSHJob& job) {
  vector<STSHProcess>& processes = job.getProcesses();
  cout << "[" << job.getNum() << "]";
  for (auto process: processes) cout << " "<< process.getID();
  cout << endl;
}

/**
 * Function: createJob
 * -------------------
 * Creates a new job on behalf of the provided pipeline.
 */
static void createJob(const pipeline& p) {
  
  ///* STSHJob& job = */ joblist.addJob(kForeground);
  
  STSHJobState state = (p.background) ? kBackground : kForeground;
  STSHJob& job = joblist.addJob(state);

  sigset_t existing, mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTSTP);
  sigaddset(&mask, SIGCONT);

  int count = p.commands.size();
  int fds[count][2];

  int infd = (!p.input.empty()) ? open(p.input.c_str(), O_RDONLY) : 0;
  int outfd = (!p.output.empty()) ? open(p.output.c_str(), O_WRONLY|O_TRUNC) : 0;
  if (outfd == -1) outfd = open(p.output.c_str(), O_WRONLY|O_CREAT, 0644);

    for (size_t i = 0; i < count - 1; i++) pipe(fds[i]);
   
  for(size_t i = 0; i < count; i++) {
    command cmd =  p.commands[i];
    pid_t pid = fork();
    if(pid == 0) {                              //Child process
      setpgid(pid, job.getGroupID());
      if (count == 1) {                                   
	if(!p.input.empty())   Dup2(infd, STDIN_FILENO);
	if(!p.output.empty())  Dup2(outfd, STDOUT_FILENO);
      } else {
	if(i == 0) {                                       // Set infd as STDIN_FILENO file descriptor
	  if(!p.input.empty())   Dup2(infd, STDIN_FILENO);
	  close(fds[0][0]);
	  Dup2(fds[0][1], STDOUT_FILENO);
	  Close(fds[count]);
	} else if(i == count - 1) {                        // Set outfd as STDOUT_FILENO file descriptor
	  if(!p.output.empty())   Dup2(outfd, STDOUT_FILENO);
	  close(fds[i - 1][1]);
	  Dup2(fds[i - 1][0], STDIN_FILENO);
	  Close(fds[0]);
	} else {                                           // Connect all other file descriptors via pipes
	  close(fds[i - 1][1]);
	  Dup2(fds[i - 1][0], STDIN_FILENO);
	  close(fds[i][0]);
	  Dup2(fds[i][1], STDOUT_FILENO);
     	  Close(fds[0]);
	  Close(fds[count - 1]);
      	}

	for(int j = 1; j < count - 2; j++) Close(fds[j]);
      }

      size_t len = sizeof(cmd.tokens)/sizeof(cmd.tokens[0]); // Execute lines
      char* args[len + 3];
      args[0] = const_cast<char *>(cmd.command);
      for (size_t i = 0; i <= len; i++) {
	args[i + 1] = const_cast<char *>(cmd.tokens[i]);
      }
      args[len + 2] = NULL;
      string str(args[0]);

      if (execvp(args[0], args) < 0) throw STSHException(str + ": Command not found.");
    } else {                                                 // Parent Process
      job.addProcess(STSHProcess(pid, cmd));                 // Add the process in child, to Parent
      setpgid(pid, job.getGroupID());                        // change the process's Group id
    }
  }
  
  for(size_t i = 0; i < count - 1; i++) {
    Close(fds[i]);
  }

  if(p.background) printBG(job);                             // Print out background job id.s
  
  if(joblist.hasForegroundJob()) {
    if(tcsetpgrp(STDIN_FILENO, job.getGroupID()) == -1 && errno != ENOTTY)  throw STSHException("authority error.");
  }

  if(tcsetpgrp(STDIN_FILENO, getpgid(getpid())) == -1 && errno != ENOTTY) throw STSHException("authority error.");
  
  sigprocmask(SIG_BLOCK, &mask, &existing);
 
  while(joblist.hasForegroundJob())  sigsuspend(&existing);   //Suspend the mask while there is foreground job
  sigprocmask(SIG_UNBLOCK, &mask, NULL);
  
}

/**
 * Function: main
 * --------------
 * Defines the entry point for a process running stsh.
 * The main function is little more than a read-eval-print
 * loop (i.e. a repl).  
 */
int main(int argc, char *argv[]) {
  pid_t stshpid = getpid();
  installSignalHandlers();
  rlinit(argc, argv);
  while (true) {
    string line;
    if (!readline(line)) break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      bool builtin = handleBuiltin(p);
      if (!builtin) createJob(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }

  return 0;
}
