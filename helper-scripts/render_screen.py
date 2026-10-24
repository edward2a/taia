import re
import sys


def render(path, rows=40, cols=120):
    data = open(path, "rb").read().decode("utf-8", "replace")
    screen = [[" "] * cols for _ in range(rows)]
    r = c = 0
    i = 0
    n = len(data)
    while i < n:
        ch = data[i]
        if ch == "\x1b":
            m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", data[i:])
            if not m:
                i += 1
                continue
            params, code = m.group(1), m.group(2)
            args = [int(p) for p in params.split(";") if p.isdigit()]
            if code in ("H", "f"):
                r = (args[0] - 1) if len(args) >= 1 else 0
                c = (args[1] - 1) if len(args) >= 2 else 0
            elif code == "A":
                r -= args[0] if args else 1
            elif code == "B":
                r += args[0] if args else 1
            elif code == "C":
                c += args[0] if args else 1
            elif code == "D":
                c -= args[0] if args else 1
            elif code == "J" and (args and args[0] == 2):
                screen = [[" "] * cols for _ in range(rows)]
            elif code == "K":
                if 0 <= r < rows:
                    for x in range(c, cols):
                        screen[r][x] = " "
            i += m.end()
            continue
        if ch == "\r":
            c = 0
        elif ch == "\n":
            r += 1
        elif ch == "\b":
            c = max(0, c - 1)
        else:
            if 0 <= r < rows and 0 <= c < cols:
                screen[r][c] = ch
            c += 1
        i += 1
    return "\n".join("".join(row).rstrip() for row in screen).rstrip("\n")


if __name__ == "__main__":
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    print(render(sys.argv[1], rows, cols))
