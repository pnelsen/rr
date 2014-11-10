/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "Replayer"

#include "replayer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <limits>
#include <map>
#include <string>
#include <vector>

#include "Event.h"
#include "GdbContext.h"
#include "GdbServer.h"
#include "kernel_abi.h"
#include "log.h"
#include "ReplaySession.h"
#include "ScopedFd.h"
#include "task.h"
#include "util.h"

using namespace rr;
using namespace std;

// Special-sauce macros defined by rr when launching the gdb client,
// which implement functionality outside of the gdb remote protocol.
// (Don't stare at them too long or you'll go blind ;).)
//
// See #define's above for origin of magic values below.
static const char gdb_rr_macros[] =
    // TODO define `document' sections for these
    "define checkpoint\n"
    "  init-if-undefined $_next_checkpoint_index = 1\n"
    /* Ensure the command echoes the checkpoint number, not the encoded message
       */
    "  p (*(int*)29298 = 0x01000000 | $_next_checkpoint_index), "
    "$_next_checkpoint_index++\n"
    "end\n"
    "define delete checkpoint\n"
    "  p (*(int*)29298 = 0x02000000 | $arg0), $arg0\n"
    "end\n"
    "define restart\n"
    "  run c$arg0\n"
    "end\n"
    "handle SIGURG stop\n";

// The parent process waits until the server, |waiting_for_child|, creates a
// debug socket. Then the parent exec()s the debugger over itself. While it's
// waiting for the child, this is the child's pid.
// This needs to be global because it's used by a signal handler.
static pid_t waiting_for_child;

// Setting these causes us to trace instructions after
// instruction_trace_at_event_start up to and including
// instruction_trace_at_event_last
static uint64_t instruction_trace_at_event_start = 0;
static uint64_t instruction_trace_at_event_last = 0;

bool trace_instructions_up_to_event(uint64_t event) {
  return event > instruction_trace_at_event_start &&
         event <= instruction_trace_at_event_last;
}

bool is_ignored_replay_signal(int sig) {
  switch (sig) {
    // SIGCHLD can arrive after tasks die during replay.  We don't
    // care about SIGCHLD unless it was recorded, in which case
    // we'll emulate its delivery.
    case SIGCHLD:
    // SIGWINCH arrives when the user resizes the terminal window.
    // Not relevant to replay.
    case SIGWINCH:
      return true;
    default:
      return false;
  }
}

/**
 * Set the blocked-ness of |sig| to |blockedness|.
 */
static void set_sig_blockedness(int sig, int blockedness) {
  sigset_t sset;
  sigemptyset(&sset);
  sigaddset(&sset, sig);
  if (sigprocmask(blockedness, &sset, nullptr)) {
    FATAL() << "Didn't change sigmask.";
  }
}

static void serve_replay_no_debugger(const string& trace_dir) {
  ReplaySession::shr_ptr replay_session = ReplaySession::create(trace_dir);

  while (true) {
    auto result = replay_session->replay_step(ReplaySession::RUN_CONTINUE);

    if (result.status == ReplaySession::REPLAY_EXITED) {
      break;
    }
    assert(result.status == ReplaySession::REPLAY_CONTINUE);
    assert(result.break_status.reason == Session::BREAK_NONE ||
           result.break_status.reason == Session::BREAK_SIGNAL);
  }

  LOG(info) << ("Replayer successfully finished.");
}

static void handle_signal(int sig) {
  switch (sig) {
    case SIGINT:
      // Translate the SIGINT into SIGTERM for the debugger
      // server, because it's blocking SIGINT.  We don't use
      // SIGINT for anything, so all it's meant to do is
      // kill us, and SIGTERM works just as well for that.
      if (waiting_for_child > 0) {
        kill(waiting_for_child, SIGTERM);
      }
      break;
    default:
      FATAL() << "Unhandled signal " << signalname(sig);
  }
}

int replay(int argc, char* argv[], char** envp) {
  string trace_dir = argc > 0 ? argv[0] : "";

  // If we're not going to autolaunch the debugger, don't go
  // through the rigamarole to set that up.  All it does is
  // complicate the process tree and confuse users.
  if (Flags::get().dont_launch_debugger) {
    if (Flags::get().goto_event ==
        numeric_limits<decltype(Flags::get().goto_event)>::max()) {
      serve_replay_no_debugger(trace_dir);
    } else {
      GdbServer().serve_replay_with_debugger(trace_dir, nullptr);
    }
    return 0;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_signal;
  if (sigaction(SIGINT, &sa, nullptr)) {
    FATAL() << "Couldn't set sigaction for SIGINT.";
  }

  int debugger_params_pipe[2];
  if (pipe2(debugger_params_pipe, O_CLOEXEC)) {
    FATAL() << "Couldn't open debugger params pipe.";
  }
  if (0 == (waiting_for_child = fork())) {
    // Ensure only the parent has the read end of the pipe open. Then if
    // the parent dies, our writes to the pipe will error out.
    close(debugger_params_pipe[0]);
    ScopedFd debugger_params_write_pipe(debugger_params_pipe[1]);
    // The parent process (gdb) must be able to receive
    // SIGINT's to interrupt non-stopped tracees.  But the
    // debugger server isn't set up to handle SIGINT.  So
    // block it.
    set_sig_blockedness(SIGINT, SIG_BLOCK);
    GdbServer().serve_replay_with_debugger(trace_dir,
                                           &debugger_params_write_pipe);
    return 0;
  }
  // Ensure only the child has the write end of the pipe open. Then if
  // the child dies, our reads from the pipe will return EOF.
  close(debugger_params_pipe[1]);
  LOG(debug) << getpid() << ": forked debugger server " << waiting_for_child;

  {
    ScopedFd params_pipe_read_fd(debugger_params_pipe[0]);
    GdbContext::launch_gdb(params_pipe_read_fd, gdb_rr_macros);
  }

  // Child must have died before we were able to get debugger parameters
  // and exec gdb. Exit with the exit status of the child.
  while (true) {
    int status;
    int ret = waitpid(waiting_for_child, &status, 0);
    int err = errno;
    LOG(debug) << getpid() << ": waitpid(" << waiting_for_child << ") returned "
               << strerror(err) << "(" << err << "); status:" << HEX(status);
    if (waiting_for_child != ret) {
      if (EINTR == err) {
        continue;
      }
      FATAL() << getpid() << ": waitpid(" << waiting_for_child << ") failed";
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      LOG(info) << ("Debugger server died.  Exiting.");
      exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
  }

  return 0;
}

void start_debug_server(Task* t) {
  // See the comment in |guard_overshoot()| explaining why we do
  // this.  Unlike in that context though, we don't know if |t|
  // overshot an internal breakpoint.  If it did, cover that
  // breakpoint up.
  t->vm()->destroy_all_breakpoints();

  // Don't launch a debugger on fatal errors; the user is most
  // likely already in a debugger, and wouldn't be able to
  // control another session.
  unique_ptr<GdbContext> dbg = GdbContext::await_client_connection(
      t->tid, GdbContext::PROBE_PORT, t->tgid());

  GdbServer().process_debugger_requests(*dbg, t);
}
