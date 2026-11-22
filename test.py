# Functional tests for the switch. Usage: python3 test.py [./switch]
# Run `make` first. Prints PASS/FAIL per check.
import socket, struct, subprocess, time, sys, os, signal
SW = sys.argv[1] if len(sys.argv) > 1 else './switch'
PORT = 9200
os.chdir(os.path.dirname(os.path.abspath(__file__)))
def mac(n): return bytes([2,0,0,0,0,n])
BC = b'\xff'*6
def frame(src, dst, payload=b'x', vlan=1, prio=1):
    return struct.pack('!H', len(payload)) + src + dst + struct.pack('!H', vlan) + bytes([prio]) + payload
def recv_n(s, n, timeout=2):
    s.settimeout(timeout); buf = b''
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d: return None
        buf += d
    return buf
def recv_frame(s, timeout=2):
    try:
        h = recv_n(s, 17, timeout)
        if h is None: return None
        l = struct.unpack('!H', h[:2])[0]
        p = recv_n(s, l, timeout) if l else b''
        return (h[2:8], h[8:14], struct.unpack('!H', h[14:16])[0], h[16], p)
    except socket.timeout:
        return 'timeout'
def conn(): 
    s = socket.create_connection(('127.0.0.1', PORT)); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); return s
def ctl(*a):
    r = subprocess.run(['./swctl', *a], capture_output=True, text=True, timeout=10); return r.stdout
res = []
def check(name, ok): res.append(ok); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

def start(n, conf=None):
    args = [SW, str(PORT), str(n)] + ([conf] if conf else [])
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5 if 'valgrind' in SW or 'tsan' in SW else 0.6); return p
def stop(p):
    p.send_signal(signal.SIGINT)
    try: out = p.communicate(timeout=20)[0]
    except subprocess.TimeoutExpired: p.kill(); out = 'TIMEOUT'
    if 'WARNING' in out or '==' in out or 'TIMEOUT' in out: print('SWITCH OUTPUT (sanitizer/valgrind/hang):', out[:3000], flush=True)
    return out
slow = ('valgrind' in SW or 'tsan' in SW)
T = 3 if slow else 1

# --- 1 forwarding / learning / partial reads
p = start(4)
a, b, c = conn(), conn(), conn(); time.sleep(0.3)
a.sendall(frame(mac(1), mac(2), b'hello'))                      # unknown dst -> flood
check('flood unknown', recv_frame(b)[4] == b'hello' and recv_frame(c)[4] == b'hello')
b.sendall(frame(mac(2), mac(1), b'back'))                       # learned: only A
check('unicast to A', recv_frame(a)[4] == b'back')
check('C does not see unicast', recv_frame(c, 0.5) == 'timeout')
a.sendall(frame(mac(1), mac(2), b'uni'))
check('learned unicast only to B', recv_frame(b)[4] == b'uni' and recv_frame(c, 0.5) == 'timeout')
raw = frame(mac(1), BC, b'slowbytes')
for i in range(len(raw)): a.sendall(raw[i:i+1]); time.sleep(0.002)
check('partial writes reassembled', recv_frame(b)[4] == b'slowbytes' and recv_frame(c)[4] == b'slowbytes')
a.sendall(raw[:10]); a.close(); time.sleep(0.3)                  # disconnect mid-frame
b.sendall(frame(mac(2), BC, b'still-alive'))
check('survives mid-frame disconnect', recv_frame(c)[4] == b'still-alive')
check('port freed after disconnect', 'port 1: free' in ctl('show', 'ports'))
b.close(); c.close(); out = stop(p); check('clean exit 1', 'clean shutdown' in out)

# --- 2 frozen host must not block others
p = start(3)
a, b, d = conn(), conn(), conn(); time.sleep(0.3)
d.sendall(frame(mac(4), BC, b'hi')); time.sleep(0.2)
d.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
big = b'z'*1400; t0 = time.time(); N = 400 if slow else 1500
for i in range(N): a.sendall(frame(mac(1), BC, big))
got = 0
b.settimeout(5)
try:
    while got < N * 0.5 and time.time() - t0 < 20:
        f = recv_frame(b, 5)
        if f in (None, 'timeout'): break
        got += 1
except Exception: pass
check('frozen host: B still gets frames (%d)' % got, got > 100)
st = ctl('stats'); drops = [l.split() for l in st.splitlines()[1:]]
check('frozen host: drops counted', any(int(r[6]) > 0 for r in drops))
a.close(); b.close(); d.close(); out = stop(p); check('clean exit 2', 'clean shutdown' in out)

# --- 3 switch full
p = start(2)
a, b = conn(), conn(); time.sleep(0.3)
c = conn(); t0 = time.time(); f = recv_frame(c, 10)
check('switch full reply in ~5s', f not in (None, 'timeout') and b'switch full' in f[4] and 3 < time.time() - t0 < 8)
check('extra host disconnected', recv_frame(c, 2) is None)
c.close()
# waiting host gets a port when one frees
c = conn(); time.sleep(1); a.close(); time.sleep(0.5)
c.sendall(frame(mac(3), BC, b'in!')); check('waiting host admitted', recv_frame(b)[4] == b'in!')
b.close(); c.close(); out = stop(p); check('clean exit 3', 'clean shutdown' in out)

# --- 4 many extra hosts at once rejected in parallel
p = start(2)
a, b = conn(), conn(); time.sleep(0.3)
extras = [conn() for _ in range(10)]; t0 = time.time()
fs = [recv_frame(e, 12) for e in extras]
check('10 extras all rejected within ~5s', all(f not in (None,'timeout') and b'switch full' in f[4] for f in fs) and time.time() - t0 < 8)
a.close(); b.close(); [e.close() for e in extras]; out = stop(p); check('clean exit 4', 'clean shutdown' in out)

# --- 5 port security
open('.t_ps.conf','w').write('portmacs 4\n')
p = start(3, '.t_ps.conf')
a, b = conn(), conn(); time.sleep(0.3)
closed = False
try:
    for i in range(20):
        a.sendall(frame(mac(10+i), BC, b'flood')); time.sleep(0.02)
    time.sleep(0.3); a.sendall(frame(mac(40), BC, b'x')); time.sleep(0.2)
    a.settimeout(1); closed = (a.recv(10) == b'')
except (ConnectionError, socket.timeout, OSError) as e: closed = isinstance(e, ConnectionError)
check('MAC flooding port shut down', closed and 'port 1: down' in ctl('show', 'ports'))
ctl('up', '1'); check('up re-enables', 'port 1: free' in ctl('show', 'ports'))
b.close(); out = stop(p); check('clean exit 5', 'clean shutdown' in out)

# --- 6 rate limit
p = start(2)
a, b = conn(), conn(); time.sleep(0.3)
ctl('rate', '20'); time.sleep(0.5)
t0 = time.time()
for i in range(40): a.sendall(frame(mac(1), BC, b'r%d' % i))
n = 0
while n < 40:
    f = recv_frame(b, 6)
    if f in (None, 'timeout'): break
    n += 1
dt = time.time() - t0
check('rate limit 20/s: 40 frames took %.1fs' % dt, n == 40 and 1.4 < dt < 3.5)
a.close(); b.close(); out = stop(p); check('clean exit 6', 'clean shutdown' in out)

# --- 7 priority / WRR order, no starvation
p = start(2)
a, b = conn(), conn(); time.sleep(0.3)
ctl('rate', '10'); time.sleep(0.4)
for pr in (2, 1, 0):
    for i in range(10): a.sendall(frame(mac(1), BC, b'p%d' % pr, prio=pr))
order = []
for i in range(30):
    f = recv_frame(b, 6)
    if f in (None, 'timeout'): break
    order.append(f[3])
check('high served first, low not starved: ' + ''.join(map(str, order)), len(order) == 30 and order[:5] == [0]*5 and 2 in order[:12])
a.close(); b.close(); out = stop(p); check('clean exit 7', 'clean shutdown' in out)

# --- 8 trunk + mirror
p = start(4)
ctl('vlan', '1', '10'); ctl('vlan', '2', '20'); ctl('trunk', '3'); ctl('mirror', '1', '4')
a, b, t, s = conn(), conn(), conn(), conn(); time.sleep(0.3)
a.sendall(frame(mac(1), BC, b'v10', vlan=99))
f = recv_frame(t); check('trunk gets frame with vlan field 10', f[2] == 10)
check('other vlan host does not see it', recv_frame(b, 0.5) == 'timeout')
check('mirror copy at sniffer', recv_frame(s)[4] == b'v10')
t.sendall(frame(mac(9), BC, b'fromtrunk', vlan=20))
check('trunk frame reaches only vlan 20 host', recv_frame(b)[4] == b'fromtrunk' and recv_frame(a, 0.5) == 'timeout')
ctl('shutdown', '2'); time.sleep(0.4); check('admin shutdown kicks host', recv_frame(b, 1) is None)
check('shown down', 'port 2: down' in ctl('show', 'ports'))
for x in (a, t, s): x.close()
out = stop(p); check('clean exit 8', 'clean shutdown' in out)

# --- 9 MAC move, age, signals
open('.t_ag.conf','w').write('age 2\n')
p = start(3, '.t_ag.conf')
a, b = conn(), conn(); time.sleep(0.3)
a.sendall(frame(mac(1), BC, b'1')); recv_frame(b); time.sleep(0.2)
b.sendall(frame(mac(1), BC, b'moved')); time.sleep(0.3)
p.send_signal(signal.SIGUSR1); time.sleep(0.5)
d = open('mac_dump.txt').read(); check('MAC moved to port 2 in dump', '02:00:00:00:00:01  port 2' in d)
time.sleep(3.5); ctl('show', 'mac'); check('aged out', ctl('show', 'mac').strip() == '')
a.close(); b.close(); out = stop(p)
log = open('switch.log').read(); check('MAC moved logged', 'MAC moved' in log); check('clean exit 9', 'clean shutdown' in out)

# --- 10 SIGINT with hosts still connected and traffic
p = start(4)
hs = [conn() for _ in range(3)]; time.sleep(0.3)
for h in hs: h.sendall(frame(mac(1), BC, b'a'*100))
out = stop(p); check('SIGINT with live hosts: clean', 'clean shutdown' in out)
for f in ('.t_ps.conf', '.t_ag.conf'):
    if os.path.exists(f): os.remove(f)
print('%d/%d passed' % (sum(res), len(res)))
