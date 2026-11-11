"""Laptop stand-in for the ESP32 web server: serves firmware/web/index.html and the API in firmware/API.md
from a live digital twin (the real safety core + health + IR test run inside).

    python3 dash_server.py                 # http://localhost:8080, free-running demo rig
    python3 dash_server.py --speed 5       # 5x real time
    python3 dash_server.py --scenario cell3_runaway

Extra console commands (dashboard Console box) to drive the model:
    fault K        start a failing-cell profile on cell K (vent gas, then heat)
    room           incense in the room for 4 minutes
    h2 K           hydrogen into cell K's gas pocket for 60 s
    reset          press the physical reset button (only in this simulator: on the rig it is a button)
"""
import argparse
import json
import math
import os
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import params as P
from scenarios import SCENARIOS
from twin import Scenario, Twin

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, '..', 'firmware', 'web', 'index.html')
ADMIN = 'ventsense-admin'
NOTE = 100


def num(x, d=3):
    return None if x is None or (isinstance(x, float) and math.isnan(x)) else round(float(x), d)


class Rig:
    def __init__(self, scenario, speed, seed=7):
        self.lock = threading.Lock()
        self.tw = Twin(scenario, seed=seed)
        self.speed = speed
        self.hist, self.events, self.ev_id = [], [], 0
        self.profile = 0
        self.sim_until = -1.0
        self.last_inc = None
        self.gen = self.tw.iter_steps(-1)

    def note(self, text):
        self.ev_id += 1
        self.events.append(dict(id=self.ev_id, t=round(self.tw.t, 1), time=self.clock(), type=NOTE, cell=-1, detail=0, text=text))

    def clock(self):
        return 'sim %07.1f s' % self.tw.t

    def loop(self):
        n = 0
        while True:
            t_wall = time.time()
            with self.lock:
                if self.profile:
                    c = self.tw.t % 80.0
                    self.tw.user_load = (c < 20) or (25 <= c < 35) or (45 <= c < 75)
                before = len(getattr(self.tw, 'events', []))
                next(self.gen)
                for e in self.tw.events[before:]:
                    self.ev_id += 1
                    self.events.append(dict(id=self.ev_id, t=round(e[0], 1), time=self.clock(),
                                            type=[k for k, v in EVN.items() if v == e[1]][0], cell=e[2], detail=e[3]))
                    if e[1] in ('L2_TRIP', 'L3_TRIP'):
                        self.last_inc = dict(time=self.clock(), cell=max(e[2], 0), mask=e[3] if e[1] == 'L2_TRIP' else 0,
                                             level=2 if e[1] == 'L2_TRIP' else 3)
                self.events = self.events[-300:]
                n += 1
                if n % 10 == 0:
                    self.hist.append(self.row())
                    self.hist = self.hist[-600:]
                if n % 600 == 0:
                    for k in self.tw.rec:
                        self.tw.rec[k].clear()
            time.sleep(max(0.0, P.DT / self.speed - (time.time() - t_wall)))

    def row(self):
        o, i = self.tw.core.out, self.tw.core.inp
        rel = sum(o.seg_relay_on[k] << k for k in range(4)) | (o.pack_relay_on << 4)
        g = [max(o.gas_rise_co[k], o.gas_rise_h2[k]) for k in range(4)]
        return ([round(self.tw.t, 1), o.level, rel] + [num(o.cell_v[k], 4) for k in range(4)] +
                [num(i.cell[k].temp_c, 2) if i.cell[k].temp_ok else None for k in range(4)] + [num(x, 4) for x in g] +
                [num(i.pack_current_a, 4), num(i.pack_bus_v, 3)])

    def state(self):
        tw = self.tw
        o, i, h, it = tw.core.out, tw.core.inp, tw.health.out, tw.irt.out
        cells = []
        for k in range(4):
            cells.append(dict(v=num(o.cell_v[k], 4) or 0.0, t=num(i.cell[k].temp_c, 2), co=num(o.gas_rise_co[k], 3),
                              h2=num(o.gas_rise_h2[k], 3), relay=o.seg_relay_on[k], level=o.cell_level[k],
                              latched=o.cell_latched[k], faults=o.cell_faults[k], sig=[o.sig_active[k][s] for s in range(3)],
                              trip_t=num(o.trip_time_s[k], 1), order=[o.trip_order[k][s] for s in range(3)],
                              soc=num(h.soc[k], 3), ah=num(h.ah_out[k], 4), score=num(h.score[k], 0),
                              ir=num(tw.ir_now[k], 5), ir_base=num(tw.ir_base[k], 5), en=tw.core.cfg.segment_enabled[k]))
        mode = 'off'
        if tw.sc.heater and tw.sc.heater[0] <= tw.t < tw.sc.heater[1]:
            mode = 'on'
        return dict(fw='1.1-sim', mock=1, t=round(tw.t, 1), time=self.clock(), level=o.level, armed=1, selftest=1,
                    relaycheck=dict(state=2, phase=4, code=0, cell=-1, meas=0, exp=0), sim=int(tw.t < self.sim_until),
                    alerts=o.alerts, buzzer=o.buzzer_mode,
                    pack=dict(relay=o.pack_relay_on, v=num(i.pack_bus_v, 3), i=num(i.pack_current_a, 4),
                              w=num(i.pack_bus_v * i.pack_current_a, 2), ah=num(h.pack_ah, 4), wh=num(h.pack_wh, 3),
                              latched=o.pack_latched),
                    amb=dict(t=num(i.ambient_c, 2), rh=num(i.ambient_rh, 1), ok=i.ambient_ok), tc=num(tw.Tb, 2),
                    heater=dict(on=o.heater_on, req=i.heater_request, duty=num(o.heater_duty, 3), mode=mode),
                    load=dict(on=o.load_on, req=i.load_request, profile=self.profile),
                    irtest=dict(state=it.state, reason=it.abort_reason, di=num(it.di_a, 3)), cells=cells)

    def command(self, c):
        tw, t = self.tw, self.tw.t
        w = c.split()
        if not w:
            return 'empty command'
        if c == 'ack':
            tw.ack_until = t + 0.5
        elif c in ('load on', 'load off'):
            self.profile = 0
            tw.user_load = c == 'load on'
        elif c in ('profile 0', 'profile 1'):
            self.profile = int(w[1])
            if not self.profile:
                tw.user_load = None
        elif c == 'irtest':
            tw.sc.ir_tests.append(t + P.DT)
        elif c == 'ir baseline':
            if not any(tw.ir_now):
                return 'no IR result yet: run irtest first'
            tw.ir_base = list(tw.ir_now)
            for k in range(4):
                tw.core.cfg.ir_ohm[k] = tw.ir_base[k]
            tw.core.set_config()
            self.note('IR baseline saved')
        elif w[0] == 'fault' and len(w) == 2:
            tw.sc.faults.append((int(w[1]) - 1, t, 300.0, 1.0))
            self.note('model: failing-cell profile started on cell %s' % w[1])
        elif c == 'room':
            r0 = t
            old = tw.sc.room_co
            tw.sc.room_co = lambda tt, r0=r0, old=old: old(tt) + (0.8 if r0 <= tt < r0 + 240 else 0.0)
            self.note('model: incense in the room for 4 min')
        elif w[0] == 'h2' and len(w) == 2:
            tw.sc.local_gas.append((int(w[1]) - 1, t, t + 60, 0.05, 0.9))
            self.note('model: hydrogen into cell %s pocket for 60 s' % w[1])
        elif w[0] == 'sim' and len(w) >= 2:
            if w[1] == 'off':
                self.sim_until = -1
            elif len(w) == 3:
                k = int(w[2]) - 1
                if w[1] == 'gas':
                    tw.sc.local_gas.append((k, t, t + 120, 0.6, 0.6))
                else:
                    tw.sc.faults.append((k, t, 60.0, 0.6))
                self.sim_until = t + 120
                self.note('SIMULATED %s signal on cell %d (demo)' % (w[1], k + 1))
        elif c == 'reset':
            tw.sc.resets.append(t + P.DT)
        elif c in ('arm', 'disarm', 'relaycheck', 'selftest', 'heater off') or w[0] in ('ramp', 'wifi', 'ntfy', 'admin'):
            return 'ok (accepted; not modelled in the simulator): ' + c
        else:
            return 'unknown command: ' + c
        return 'ok: ' + c


EVN = {1: 'SIGNAL_ON', 2: 'SIGNAL_OFF', 3: 'L2_TRIP', 4: 'L3_TRIP', 5: 'RESET', 6: 'FAULT_ON', 7: 'FAULT_OFF',
       8: 'ALERT_ON', 9: 'ALERT_OFF', 10: 'ARMED', 11: 'DISARMED', 12: 'ACK'}


def make_handler(rig):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype='application/json'):
            b = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(b)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(b)

        def admin(self, q):
            return self.headers.get('X-VS-Key') == ADMIN or q.get('key', [''])[0] == ADMIN

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            with rig.lock:
                if u.path in ('/', '/index.html'):
                    with open(WEB, 'rb') as fh:
                        return self.send(200, fh.read(), 'text/html; charset=utf-8')
                if u.path == '/api/state':
                    return self.send(200, json.dumps(rig.state()))
                if u.path == '/api/history':
                    since = float(q.get('since', ['-1'])[0])
                    rows = [r for r in rig.hist if r[0] > since]
                    cols = ['t', 'lvl', 'rel', 'v1', 'v2', 'v3', 'v4', 'T1', 'T2', 'T3', 'T4', 'g1', 'g2', 'g3', 'g4', 'i', 'bus']
                    return self.send(200, json.dumps(dict(cols=cols, rows=rows)))
                if u.path == '/api/events':
                    since = int(q.get('since', ['0'])[0])
                    ev = [e for e in rig.events if e['id'] > since]
                    return self.send(200, json.dumps(dict(next=rig.ev_id, events=ev)))
                if u.path == '/api/incidents':
                    return self.send(200, json.dumps(dict(sd=0, last=rig.last_inc, items=[])))
                if u.path == '/api/config':
                    return self.send(200, '{"settings":%s}' % rig.tw.core.config_json())
                if u.path == '/api/selftest':
                    return self.send(200, json.dumps(dict(ok=1, report='SELF-TEST PASSED (SIMULATOR: digital twin, nothing is measured)')))
                if u.path == '/api/cmd':
                    if not self.admin(q):
                        return self.send(403, 'admin password required', 'text/plain')
                    return self.send(200, rig.command(q.get('c', [''])[0].strip()), 'text/plain')
            self.send(404, 'not found', 'text/plain')

        def do_POST(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            n = int(self.headers.get('Content-Length', '0'))
            body = self.rfile.read(n)
            if not self.admin(q):
                return self.send(403, 'admin password required', 'text/plain')
            if u.path == '/api/config':
                form = urllib.parse.parse_qs(body.decode())
                with rig.lock:
                    for k, v in form.items():
                        err = rig.tw.core.config_set(k, float(v[0]))
                        if err:
                            return self.send(200, json.dumps(dict(ok=0, err=err)))
                return self.send(200, json.dumps(dict(ok=1, err='')))
            if u.path == '/update':
                return self.send(200, 'simulator: firmware update not modelled', 'text/plain')
            self.send(404, 'not found', 'text/plain')
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--speed', type=float, default=1.0, help='simulation speed vs real time')
    ap.add_argument('--scenario', default='', help='replay one of scenarios.py instead of the free-running rig')
    a = ap.parse_args()
    if a.scenario:
        sc = [s for s in SCENARIOS if s.name == a.scenario][0]
    else:
        sc = Scenario('demo', 'Free-running demo rig', 1e9, tweak=lambda c: setattr(c, 'gas_warmup_s', 60.0))
    rig = Rig(sc, a.speed)
    threading.Thread(target=rig.loop, daemon=True).start()
    print('VentSense dashboard (simulator) on http://localhost:%d  admin password: %s' % (a.port, ADMIN))
    ThreadingHTTPServer(('127.0.0.1', a.port), make_handler(rig)).serve_forever()


if __name__ == '__main__':
    main()
