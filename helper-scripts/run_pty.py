import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time


def drain(fd, out, seconds):
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        out.extend(data)


def main():
    rows = int(sys.argv[1])
    cols = int(sys.argv[2])
    log_path = sys.argv[3]
    keys = sys.argv[4].encode().decode("unicode_escape").encode()
    argv = sys.argv[5:]
    start_wait = float(os.environ.get("PTY_START_WAIT", "1.0"))
    key_wait = float(os.environ.get("PTY_KEY_WAIT", "1.2"))
    tail_wait = float(os.environ.get("PTY_TAIL_WAIT", "1.0"))
    decision = os.environ.get("PTY_DECISION_KEYS", "")
    decision_wait = float(os.environ.get("PTY_DECISION_WAIT", "0"))

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = bytearray()

    drain(fd, out, start_wait)
    os.write(fd, keys)
    drain(fd, out, key_wait)
    if decision:
        os.write(fd, decision.encode().decode("unicode_escape").encode())
        drain(fd, out, decision_wait)
    os.write(fd, b"q")
    drain(fd, out, tail_wait)

    try:
        os.close(fd)
    except OSError:
        pass
    os.waitpid(pid, 0)

    with open(log_path, "wb") as fh:
        fh.write(out)


main()
