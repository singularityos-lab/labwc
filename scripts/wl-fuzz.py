#!/usr/bin/env python3
import array
import errno
import glob
import os
import random
import socket
import struct
import sys
import time
import xml.etree.ElementTree as ET


def load_protocols(dirs):
    ifaces = {}
    for d in dirs:
        for path in glob.glob(os.path.join(d, "**", "*.xml"), recursive=True):
            try:
                root = ET.parse(path).getroot()
            except ET.ParseError:
                continue
            if root.tag != "protocol":
                continue
            for it in root.findall("interface"):
                reqs, evs = [], []
                for kind, out in (("request", reqs), ("event", evs)):
                    for r in it.findall(kind):
                        args = [(a.get("type"), a.get("interface"), a.get("allow-null") == "true") for a in r.findall("arg")]
                        out.append((r.get("name"), args, r.get("type") == "destructor"))
                ifaces[it.get("name")] = {"version": int(it.get("version", "1")), "requests": reqs, "events": evs}
    return ifaces


INTS = [0, 1, -1, 2, 7, 255, 256, 65535, 65536, 0x7FFFFFFF, -0x80000000, 0x40000000, -2, 3, 4, 5]
UINTS = [0, 1, 2, 3, 4, 7, 8, 15, 16, 255, 256, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 1 << 20]


class Conn:
    def __init__(self, path, ifaces, rng, log):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.setblocking(False)
        self.ifaces = ifaces
        self.rng = rng
        self.log = log
        self.objects = {1: ("wl_display", 1)}
        self.next_id = 2
        self.globals = []
        self.inbuf = b""
        self.dead = False
        self.error = None
        self.configures = {}

    def send(self, oid, opcode, payload, fds=()):
        size = 8 + len(payload)
        msg = struct.pack("<II", oid, (size << 16) | opcode) + payload
        anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))] if fds else []
        try:
            self.s.setblocking(True)
            self.s.settimeout(2)
            self.s.sendmsg([msg], anc)
        except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError):
            self.dead = True
        finally:
            for fd in fds:
                os.close(fd)

    def pump(self, wait=0.0):
        end = time.monotonic() + wait
        while True:
            try:
                self.s.settimeout(max(0.0, end - time.monotonic()) or 0.0001)
                data, anc, _, _ = self.s.recvmsg(65536, socket.CMSG_SPACE(28 * 4))
            except (socket.timeout, BlockingIOError):
                break
            except (ConnectionResetError, OSError):
                self.dead = True
                break
            for level, kind, cdata in anc:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    fds = array.array("i")
                    fds.frombytes(cdata[: len(cdata) - len(cdata) % fds.itemsize])
                    for fd in fds:
                        os.close(fd)
            if not data:
                self.dead = True
                break
            self.inbuf += data
            self.parse()
            if time.monotonic() >= end:
                break

    def parse(self):
        while len(self.inbuf) >= 8:
            oid, word = struct.unpack_from("<II", self.inbuf)
            size = word >> 16
            if size < 8 or len(self.inbuf) < size:
                return
            body = self.inbuf[8:size]
            opcode = word & 0xFFFF
            self.inbuf = self.inbuf[size:]
            if oid == 1 and opcode == 0:
                self.dead = True
                if len(body) >= 12:
                    obj, code, ln = struct.unpack_from("<III", body)
                    self.error = (self.objects.get(obj, ("?", 0))[0], code, body[12:12 + ln - 1].decode(errors="replace")[:70])
            elif oid == 1 and opcode == 1 and len(body) >= 4:
                self.objects.pop(struct.unpack_from("<I", body)[0], None)
            elif oid in self.objects and self.objects[oid][0] == "xdg_surface" and opcode == 0 and len(body) >= 4:
                self.configures[oid] = struct.unpack_from("<I", body)[0]
            elif oid in self.objects and self.objects[oid][0] == "zwlr_layer_surface_v1" and opcode == 0 and len(body) >= 4:
                self.configures[oid] = struct.unpack_from("<I", body)[0]
            elif oid in self.objects and self.objects[oid][0] == "wl_registry" and opcode == 0:
                name, = struct.unpack_from("<I", body)
                ln, = struct.unpack_from("<I", body, 4)
                iface = body[8:8 + ln - 1].decode(errors="replace")
                ver, = struct.unpack_from("<I", body, 8 + ((ln + 3) & ~3))
                self.globals.append((name, iface, ver))

    def new_id(self, iface, version):
        oid = self.next_id
        self.next_id += 1
        self.objects[oid] = (iface, version)
        return oid

    def string(self, s):
        b = s.encode() + b"\0"
        return struct.pack("<I", len(b)) + b + b"\0" * ((4 - len(b) % 4) % 4)

    def rand_string(self):
        r = self.rng
        choice = r.randrange(6)
        if choice == 0:
            return ""
        if choice == 1:
            return "x" * r.choice([1, 63, 64, 255, 256, 1024, 4000])
        if choice == 2:
            return "".join(chr(r.randrange(1, 0x2FF)) for _ in range(r.randrange(1, 40)))
        if choice == 3:
            return r.choice(["/", "..", "../../etc/passwd", "%s%n", "‮", "a\nb", "default", "left_ptr", "org.example"])
        return "".join(r.choice("abcdefghijklmnopqrstuvwxyz-_.0123456789") for _ in range(r.randrange(1, 24)))

    def object_arg(self, want, allow_null):
        r = self.rng
        matching = [o for o, (i, _) in self.objects.items() if want is None or i == want]
        roll = r.random()
        if allow_null and roll < 0.1:
            return 0
        if matching and roll < 0.8:
            return r.choice(matching)
        if roll < 0.9:
            return r.choice(list(self.objects))
        return r.choice([0, 0xFF000000, self.next_id + 50, 0xFFFFFFFF])

    def fd_arg(self):
        r = self.rng
        k = r.randrange(4)
        if k == 0:
            fd = os.memfd_create("wlfuzz")
            size = r.choice([0, 1, 4096, 1 << 20])
            os.ftruncate(fd, size)
            return fd
        if k == 1:
            return os.open("/dev/null", os.O_RDWR)
        if k == 2:
            a, b = os.pipe()
            if r.random() < 0.5:
                os.close(b)
                return a
            os.close(a)
            return b
        return os.open("/", os.O_RDONLY | os.O_DIRECTORY)

    def request(self, oid):
        iface, version = self.objects[oid]
        spec = self.ifaces.get(iface)
        if not spec or not spec["requests"]:
            return None
        r = self.rng
        if r.random() < 0.02:
            opcode = r.randrange(len(spec["requests"]) + 3)
            payload = bytes(r.randrange(256) for _ in range(r.randrange(0, 40) & ~3))
            self.send(oid, opcode, payload)
            return f"{iface}#{oid}.raw({opcode}, {len(payload)} bytes)"
        opcode = r.randrange(len(spec["requests"]))
        name, args, destructor = spec["requests"][opcode]
        if iface == "wl_registry" and name == "bind":
            pick = [g for g in self.globals if g[1] in self.ifaces]
            if not pick:
                return None
            gname, giface, gver = r.choice(pick)
            ver = min(gver, self.ifaces[giface]["version"])
            if r.random() < 0.05:
                ver = r.choice([0, gver + 1, 0xFFFFFFFF])
            if r.random() < 0.03:
                gname = r.choice([0, 0xFFFFFFFF, gname + 1000])
            nid = self.new_id(giface, max(ver, 1))
            self.send(oid, opcode, struct.pack("<I", gname) + self.string(giface) + struct.pack("<II", ver, nid))
            return f"wl_registry#{oid}.bind({gname}, {giface} v{ver} -> #{nid})"
        payload = b""
        fds = []
        shown = []
        for typ, aiface, allow_null in args:
            sane = r.random() < 0.85
            if typ == "int":
                v = r.randrange(0, 8) if sane else (r.choice(INTS) if r.random() < 0.7 else r.randrange(-5000, 5000))
                payload += struct.pack("<i", v)
                shown.append(str(v))
            elif typ == "uint":
                v = r.randrange(0, 4) if sane else (r.choice(UINTS) if r.random() < 0.7 else r.randrange(0, 5000))
                payload += struct.pack("<I", v)
                shown.append(str(v))
            elif typ == "fixed":
                v = r.choice([0, 1, -1, 0x7FFFFFFF, -0x80000000, 256, r.randrange(-1 << 20, 1 << 20)])
                payload += struct.pack("<i", v)
                shown.append(f"f{v}")
            elif typ == "string":
                if allow_null and r.random() < 0.1:
                    payload += struct.pack("<I", 0)
                    shown.append("null")
                else:
                    s = self.rand_string()
                    payload += self.string(s)
                    shown.append(repr(s[:20]))
            elif typ == "object":
                matching = [o for o, (i, _) in self.objects.items() if i == aiface] if aiface else []
                v = r.choice(matching) if sane and matching else self.object_arg(aiface, allow_null)
                payload += struct.pack("<I", v)
                shown.append(f"@{v}")
            elif typ == "new_id":
                if aiface:
                    nid = self.new_id(aiface, version)
                    payload += struct.pack("<I", nid)
                    shown.append(f"new {aiface}#{nid}")
                else:
                    pick = [g for g in self.globals if g[1] in self.ifaces] or self.globals
                    if not pick:
                        return None
                    gname, giface, gver = r.choice(pick)
                    ver = r.choice([1, gver, gver, gver + 1]) if r.random() < 0.1 else min(gver, self.ifaces.get(giface, {"version": gver})["version"])
                    nid = self.new_id(giface, ver)
                    payload += self.string(giface) + struct.pack("<II", ver, nid)
                    shown.append(f"new {giface}#{nid} v{ver}")
            elif typ == "array":
                n = r.choice([0, 4, 8, 12, 64, 4096])
                payload += struct.pack("<I", n) + bytes(r.randrange(256) for _ in range(n))
                shown.append(f"array[{n}]")
            elif typ == "fd":
                fds.append(self.fd_arg())
                shown.append("fd")
        self.send(oid, opcode, payload, fds)
        if destructor:
            self.objects.pop(oid, None)
        return f"{iface}#{oid}.{name}({', '.join(shown)})"


def bind(c, iface, maxver=None):
    for gname, giface, gver in c.globals:
        if giface == iface:
            ver = min(gver, c.ifaces.get(iface, {"version": gver})["version"], maxver or gver)
            nid = c.new_id(iface, ver)
            c.send(2, 0, struct.pack("<I", gname) + c.string(iface) + struct.pack("<II", ver, nid))
            return nid
    return None


def prelude(c, r):
    comp = bind(c, "wl_compositor")
    wm = bind(c, "xdg_wm_base")
    shm = bind(c, "wl_shm")
    bind(c, "wl_seat")
    bind(c, "wl_subcompositor")
    layer = bind(c, "zwlr_layer_shell_v1")
    bind(c, "wl_data_device_manager")
    bind(c, "wp_viewporter")
    bind(c, "wp_fractional_scale_manager_v1")
    bind(c, "zxdg_decoration_manager_v1")
    bind(c, "xdg_activation_v1")
    for g in r.sample([g for g in c.globals if g[1] in c.ifaces], k=min(4, len(c.globals))):
        bind(c, g[1])
    if not (comp and wm and shm):
        return
    surf = c.new_id("wl_surface", c.objects[comp][1])
    c.send(comp, 0, struct.pack("<I", surf))
    xs = c.new_id("xdg_surface", c.objects[wm][1])
    c.send(wm, 2, struct.pack("<II", xs, surf))
    top = c.new_id("xdg_toplevel", c.objects[wm][1])
    c.send(xs, 1, struct.pack("<I", top))
    c.send(top, 2, c.string("wlfuzz"))
    c.send(surf, 6, b"")
    end = time.monotonic() + 0.3
    while xs not in c.configures and not c.dead and time.monotonic() < end:
        c.pump(0.02)
    if xs in c.configures:
        c.send(xs, 4, struct.pack("<I", c.configures[xs]))
    w, h = r.choice([(1, 1), (64, 48), (640, 480), (4000, 3000)])
    size = w * h * 4
    fd = os.memfd_create("wlfuzz-pool")
    os.ftruncate(fd, size)
    pool = c.new_id("wl_shm_pool", 1)
    c.send(shm, 0, struct.pack("<Ii", pool, size), [fd])
    buf = c.new_id("wl_buffer", 1)
    c.send(pool, 0, struct.pack("<IiiiiI", buf, 0, w, h, w * 4, 0))
    c.send(surf, 1, struct.pack("<Iii", buf, 0, 0))
    c.send(surf, 6, b"")
    if layer and r.random() < 0.5:
        ls_surf = c.new_id("wl_surface", c.objects[comp][1])
        c.send(comp, 0, struct.pack("<I", ls_surf))
        lsurf = c.new_id("zwlr_layer_surface_v1", c.objects[layer][1])
        c.send(layer, 0, struct.pack("<IIII", lsurf, ls_surf, 0, r.randrange(4)) + c.string("wlfuzz"))
        c.send(lsurf, 0, struct.pack("<II", 100, 40))
        c.send(lsurf, 1, struct.pack("<I", r.randrange(16)))
        c.send(ls_surf, 6, b"")
        end = time.monotonic() + 0.3
        while lsurf not in c.configures and not c.dead and time.monotonic() < end:
            c.pump(0.02)
        if lsurf in c.configures:
            c.send(lsurf, 6, struct.pack("<I", c.configures[lsurf]))
    c.pump(0.05)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split(") ")[1][0] != "Z"
    except OSError:
        return False


def main():
    sock, pid, seconds, seed = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
    dirs = sys.argv[5:]
    ifaces = load_protocols(dirs)
    rng = random.Random(seed)
    start = time.monotonic()
    conns = requests = 0
    errors = {}
    while time.monotonic() - start < seconds:
        conn_seed = rng.randrange(1 << 32)
        crng = random.Random(conn_seed)
        trail = []
        try:
            c = Conn(sock, ifaces, crng, trail)
        except OSError as e:
            if not alive(pid):
                break
            time.sleep(0.05)
            continue
        conns += 1
        c.send(1, 1, struct.pack("<I", c.new_id("wl_registry", 1)))
        c.send(1, 0, struct.pack("<I", c.new_id("wl_callback", 1)))
        c.pump(0.05)
        if crng.random() < 0.8:
            prelude(c, crng)
        budget = crng.randrange(20, 400)
        for _ in range(budget):
            if c.dead:
                break
            oid = crng.choice(list(c.objects))
            desc = c.request(oid)
            if desc:
                trail.append(desc)
                requests += 1
            if crng.random() < 0.2:
                c.send(1, 0, struct.pack("<I", c.new_id("wl_callback", 1)))
            c.pump(0)
        c.pump(0.05)
        if c.error:
            errors[c.error] = errors.get(c.error, 0) + 1
        c.s.close()
        if not alive(pid):
            print(f"COMPOSITOR DIED after {conns} connections, {requests} requests; connection seed {conn_seed}")
            print("last requests:")
            for line in trail[-40:]:
                print("  " + line)
            sys.exit(1)
    for (iface, code, msg), n in sorted(errors.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  {n:5d} {iface} {code}: {msg}")
    print(f"survived {conns} connections, {requests} requests in {time.monotonic() - start:.0f}s (seed {seed}, {len(ifaces)} interfaces)")


if __name__ == "__main__":
    main()
