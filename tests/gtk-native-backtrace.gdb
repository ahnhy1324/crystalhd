# SPDX-License-Identifier: LGPL-2.1-or-later
# One execution of the original GTK suite, with native stacks if it crashes.
# Use --batch --return-child-result so ordinary unittest failures stay failures.
set pagination off
set confirm off
set print thread-events off
set debuginfod enabled off
# Preserve normal ASLR rather than GDB's usual disable-randomization default.
set disable-randomization off
handle SIGSEGV stop print pass
handle SIGABRT stop print pass
handle SIGBUS stop print pass
handle SIGILL stop print pass

python
import gdb
import os
import signal
import time

stops = []
gdb.events.stop.connect(stops.append)

def fail(message):
    gdb.write("GTK native capture failed: " + message + "\n", gdb.STDERR)
    # Do not leave a detached live test behind or let GDB quit kill it implicitly.
    pid = gdb.selected_inferior().pid
    if pid:
        os.kill(pid, signal.SIGTERM)
        gdb.execute("detach")
        deadline = time.monotonic() + 3
        while True:
            try:
                retired, _ = os.waitpid(pid, os.WNOHANG)
                if retired:
                    break
            except ChildProcessError:
                # GDB's SIGCHLD handler may already have reaped this child.
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    break
            if time.monotonic() >= deadline:
                gdb.write("GTK native capture: unretired inferior PID " + str(pid) + "\n", gdb.STDERR)
                gdb.execute("quit 1")
            time.sleep(.02)
        gdb.write("GTK native capture: inferior retired after SIGTERM\n")
    gdb.execute("quit 1")

try:
    gdb.execute("run")
except gdb.error as error:
    fail(str(error))

if gdb.selected_inferior().pid:
    event = stops[-1] if stops else None
    fatal = {"SIGSEGV", "SIGABRT", "SIGBUS", "SIGILL"}
    if not isinstance(event, gdb.SignalEvent) or event.stop_signal not in fatal:
        fail("inferior stopped without a captured fatal signal")

    gdb.write("\n=== GTK native crash: " + event.stop_signal + " ===\n")
    try:
        gdb.execute("thread apply all bt full")
    except gdb.error as error:
        gdb.write("Native backtrace unavailable: " + str(error) + "\n", gdb.STDERR)
    try:
        gdb.execute("thread apply all py-bt")
    except gdb.error as error:
        gdb.write("Python backtrace unavailable: " + str(error) + "\n", gdb.STDERR)

    # Deliver the original signal. Python's faulthandler can re-raise it after
    # printing its stack; let that terminate too, without another debugger stop.
    # Do not retry the test or suppress the crash.
    for fatal_signal in fatal:
        gdb.execute("handle " + fatal_signal + " nostop print pass")
    try:
        gdb.execute("continue")
    except gdb.error as error:
        fail(str(error))
    if gdb.selected_inferior().pid:
        fail("inferior remained stopped after the original signal was delivered")
    # A signal handler that recovers and exits zero must not turn a captured
    # fatal signal into a green CI result. Ordinary test exits bypass this path.
    gdb.execute("quit 1")
end
