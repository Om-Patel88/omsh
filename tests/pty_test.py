#!/usr/bin/env python3
"""Drive an interactive omsh on a pseudo-terminal and check job control,
Ctrl-C handling and history recall.

Run from the repository root (make ptytest). Uses only the standard library.
"""

import os
import pty
import re
import select
import signal
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OMSH = os.environ.get("OMSH", os.path.join(ROOT, "omsh"))
TIMEOUT = 10.0

# The line editor draws an empty prompt as the prompt followed by "erase to
# end of line"; with text typed there is text in between.
IDLE_PROMPT = rb"omsh\$ \x1b\[K"


class Failure(Exception):
    pass


class Shell:
    def __init__(self):
        env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": "/nonexistent",  # no ~/.omshrc
            "TERM": "dumb",
        }
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                os.execve(OMSH, ["omsh"], env)
            finally:
                os._exit(127)
        self.buf = b""
        self.pos = 0  # matches are searched for after this offset
        self.alive = True

    def _read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return False
        try:
            data = os.read(self.fd, 4096)
        except OSError:  # EIO once the shell has exited (Linux)
            data = b""
        if not data:
            self.alive = False
            return False
        self.buf += data
        return True

    def expect(self, pattern, timeout=TIMEOUT):
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            m = regex.search(self.buf, self.pos)
            if m:
                self.pos = m.end()
                return m
            left = deadline - time.monotonic()
            if left <= 0 or (not self._read(min(left, 0.2)) and not self.alive):
                tail = self.buf[self.pos:][-400:]
                raise Failure("timed out waiting for %r; got %r" % (pattern, tail))

    def saw(self, pattern, timeout):
        """Like expect, but return None instead of failing."""
        try:
            return self.expect(pattern, timeout)
        except Failure:
            return None

    def send(self, text):
        os.write(self.fd, text.encode() if isinstance(text, str) else text)

    def run(self, line):
        """Type a command line and wait until the editor has accepted it."""
        self.send(line + "\r")
        self.expect(re.escape(line.encode()) + rb"\x1b\[K[^\n]*\r\n")

    def prompt(self, timeout=TIMEOUT):
        self.expect(IDLE_PROMPT, timeout)

    def wait_exit(self, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.pid = 0
                return status
            self._read(0.1)
        raise Failure("shell did not exit")

    def close(self):
        if self.pid:
            # Kill anything we left behind, then the shell itself.
            if self.alive:
                try:
                    self.send("kill -KILL %1 %2 %3 2>/dev/null\r")
                    self.saw(IDLE_PROMPT, 2)
                except OSError:
                    pass
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except OSError:
                pass
            self.pid = 0
        os.close(self.fd)


def send_until(sh, key, pattern, tries=5):
    """Send a control key until pattern shows up. The key is lost if it
    arrives before the shell has handed the terminal to the job."""
    for _ in range(tries):
        time.sleep(0.3)
        sh.send(key)
        if sh.saw(pattern, 2):
            return
    raise Failure("no %r after sending %r" % (pattern, key))


def test_stop_bg_fg_interrupt(sh):
    sh.prompt()
    sh.run("sleep 30")
    send_until(sh, b"\x1a", rb"\[1\]\+\s+Stopped\s+sleep 30")
    sh.prompt()

    sh.run("jobs")
    sh.expect(rb"\[1\]\+\s+Stopped\s+sleep 30\r\n")
    sh.prompt()

    sh.run("bg")
    sh.expect(rb"\[1\]\+ sleep 30 &")
    sh.prompt()
    sh.run("jobs")
    sh.expect(rb"\[1\]\+\s+Running\s+sleep 30\r\n")
    sh.prompt()

    sh.run("fg")
    sh.expect(rb"sleep 30\r\n")
    send_until(sh, b"\x03", IDLE_PROMPT)

    sh.run("jobs")
    start = sh.pos
    sh.run("echo alive")
    m = sh.expect(rb"alive\r\n")
    if b"sleep 30" in sh.buf[start:m.start()]:
        raise Failure("job still listed after Ctrl-C")
    sh.prompt()


def test_ctrl_c_at_prompt(sh):
    sh.prompt()
    sh.send(b"\x03")
    sh.expect(rb"\^C")
    sh.prompt()
    sh.run("echo still-here")
    sh.expect(rb"still-here\r\n")
    sh.prompt()


def test_history(sh):
    sh.prompt()
    sh.run("echo hist-one")
    sh.expect(rb"hist-one\r\n")
    sh.prompt()
    sh.send(b"\x1b[A")
    sh.expect(rb"omsh\$ echo hist-one\x1b\[K")
    sh.send(b"\r")
    sh.expect(rb"\r\nhist-one\r\n")
    sh.prompt()


def test_exit_with_stopped_job(sh):
    sh.prompt()
    sh.run("sh -c 'echo child=$$; exec sleep 30'")
    pid = int(sh.expect(rb"child=(\d+)").group(1))
    send_until(sh, b"\x1a", rb"Stopped")
    sh.prompt()
    sh.run("exit")
    sh.expect(rb"There are stopped jobs\.")
    sh.prompt()
    sh.run("exit")
    status = sh.wait_exit()
    if not os.WIFEXITED(status):
        raise Failure("shell did not exit normally (status %r)" % status)
    # The shell hangs up stopped jobs on its way out.
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    os.kill(pid, signal.SIGKILL)
    raise Failure("stopped job %d survived the shell" % pid)


TESTS = [
    test_stop_bg_fg_interrupt,
    test_ctrl_c_at_prompt,
    test_history,
    test_exit_with_stopped_job,
]


def main():
    if not os.access(OMSH, os.X_OK):
        print("pty_test: %s not found; run make first" % OMSH, file=sys.stderr)
        return 1
    failed = 0
    for test in TESTS:
        sh = Shell()
        try:
            test(sh)
            print("PASS %s" % test.__name__)
        except Failure as e:
            print("FAIL %s: %s" % (test.__name__, e))
            failed += 1
        finally:
            sh.close()
    print("\n%d passed, %d failed" % (len(TESTS) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
