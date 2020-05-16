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
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }
  
  return true;
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
  /*
  if(joblist.hasForegroundJob()) {
    if(tcsetpgrp(STDIN_FILENO, job.getGroupID()) == -1 && errno != ENOTTY)  throw STSHException("authority error.");
  }

  if(tcsetpgrp(STDIN_FILENO, getpgid(getpid())) == -1 && errno != ENOTTY) throw STSHException("authority error.");

  sigprocmask(SIG_BLOCK, &mask, &existing);
  */
  //  while(joblist.hasForegroundJob())  sigsuspend(&existing);   //Suspend the mask while there is foreground job
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
