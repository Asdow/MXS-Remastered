# MX Simulator's .saf archives: a text index of "size path" lines, a "-" line, then each file's
# bytes in the index's order.
import re


def read_index(path):
    """[(size, name)], and where the first file's bytes start."""
    with open(path, 'rb') as f:
        data = f.read(16 * 1024 * 1024)
    entries, pos = [], 0
    while True:
        nl = data.find(b'\n', pos)
        if nl < 0:
            raise ValueError('%s: no end to the index' % path)
        line = data[pos:nl]
        m = re.match(rb'^(\d+) (.+)$', line)
        if not m:
            if line != b'-':
                raise ValueError('%s: not a .saf index line: %r' % (path, line[:60]))
            return entries, nl + 1
        entries.append((int(m.group(1)), m.group(2).decode('latin1')))
        pos = nl + 1


def extract(path, want):
    """{name: bytes} for the files whose name want(name) accepts."""
    entries, offset = read_index(path)
    out = {}
    with open(path, 'rb') as f:
        for size, name in entries:
            if want(name):
                f.seek(offset)
                out[name] = f.read(size)
            offset += size
    return out


def write(path, files):
    """files: [(name, bytes)], written in that order."""
    with open(path, 'wb') as f:
        for name, data in files:
            f.write(b'%d %s\n' % (len(data), name.encode('latin1')))
        f.write(b'-\n')
        for name, data in files:
            f.write(data)
