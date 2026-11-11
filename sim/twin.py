"""Closed-loop digital twin: plant (cells, pack path, thermal, vent gas, heater rig) + sensor models
+ the real firmware safety core. One Twin = one scenario run."""
import math
import random

import params as P
from vs_core import SafetyCore, Health, IrTest, SIG_NAMES


def ocv(soc):
    soc = min(max(soc, 0.0), 1.0)
    x = soc * 10.0
    i = min(int(x), 9)
    return P.OCV_TABLE[i] + (P.OCV_TABLE[i + 1] - P.OCV_TABLE[i]) * (x - i)


class Scenario:
    def __init__(self, name, title, duration, load=lambda t: True, faults=(), room_co=lambda t: 0.0,
                 room_h2=lambda t: 0.0, local_gas=(), unplug=(), heater=None, dummy=None, resets=(), tweak=None,
                 expect=None, ir_tests=(), r0_change=()):
        self.name, self.title, self.duration = name, title, duration
        self.load, self.faults = load, list(faults)  # faults: (cell, onset_s, ramp_s, scale)
        self.room_co, self.room_h2 = room_co, room_h2
        self.local_gas = list(local_gas)  # (cell, t_on, t_off, co_level, h2_level): gas fed into one pocket
        self.unplug = list(unplug)  # (cell, t)
        self.heater = heater  # (t_on, t_off) request window
        self.dummy = dummy  # segment index holding a dummy cell (heater test)
        self.resets = list(resets)
        self.tweak = tweak
        self.expect = expect  # f(result) -> [(check, passed, detail)]
        self.ir_tests = list(ir_tests)    # times at which the firmware runs the internal-resistance test
        self.r0_change = list(r0_change)  # (cell, t, factor): synthetic accelerated ageing step


class Twin:
    def __init__(self, sc, seed=1, tweak=None):
        self.sc = sc
        self.rng = rng = random.Random(seed)

        def cfg_tweak(c):
            if sc.dummy is not None:
                c.segment_enabled[sc.dummy] = 0
                c.heater_cell = sc.dummy
            if sc.tweak:
                sc.tweak(c)
            if tweak:
                tweak(c)

        self.core = SafetyCore(cfg_tweak)
        n = P.N
        self.real = [k != sc.dummy for k in range(n)]
        self.soc = [P.SOC0] * n
        self.vrc = [0.0] * n
        self.isc = [0.0] * n
        self.r0 = [P.R0 * (1 + rng.uniform(-P.R0_SPREAD, P.R0_SPREAD)) for _ in range(n)]
        self.T = [P.T_AMB] * n
        self.Tp = [P.T_AMB] * n
        self.Tmeas = [P.T_AMB] * n
        self.Tb = P.T_AMB
        self.ksd_closed = True
        self.ds_off = [rng.uniform(-P.DS18B20_OFFSET, P.DS18B20_OFFSET) for _ in range(n)]
        self.next_ds = 0.0
        self.c_vent = [0.0] * n
        self.c_room_co = [0.0] * n
        self.c_room_h2 = [0.0] * n
        self.x_co = [0.0] * n
        self.x_h2 = [0.0] * n
        self.base_co = [rng.uniform(*P.MQ_AO_BASE) for _ in range(n)]
        self.base_h2 = [rng.uniform(*P.MQ_AO_BASE) for _ in range(n)]
        self.drift_ph = [rng.uniform(0, 6.28) for _ in range(2 * n)]
        self.gain = [1 + rng.uniform(-P.TAP_GAIN_ERR, P.TAP_GAIN_ERR) for _ in range(n)]
        self.seg_act = [0] * n
        self.pack_act = 0
        self.heater_act = 0
        self.load_act = 0
        self.debounce = self.core.cfg.debounce_s
        self.r0_nominal = list(self.r0)
        self.health, self.irt = Health(), IrTest()
        self.ir_base = [0.0] * n
        self.ir_now = [0.0] * n
        self.ir_results = []  # (t, [ohm per cell], [model ohm per cell])
        self.user_load = None  # dash server: None = scenario load pattern, else forced on/off
        self.ack_until = -1.0
        self.t = 0.0

    def severity(self, k, t):
        s = 0.0
        for cell, onset, ramp, scale in self.sc.faults:
            if cell == k and t >= onset:
                s = max(s, min(1.0, (t - onset) / ramp) * scale)
        return s

    def run(self):
        for _ in self.iter_steps():
            pass
        return self.summarise(self.rec, self.events)

    def iter_steps(self, n_steps=None):
        """advance the plant + firmware one 100 ms step per iteration; n_steps=-1 runs forever (dash server)"""
        sc, rng, core, n, dt = self.sc, self.rng, self.core, P.N, P.DT
        self.rec = rec = {k: [] for k in ('t', 'gas', 'temp', 'temp_true', 'cell_v', 'current', 'level', 'seg', 'pack',
                               'heater', 'block', 'duty', 'ksd', 'score', 'soc')}
        self.events = events = []
        steps = int(round(sc.duration / dt)) if n_steps is None else n_steps
        i = -1
        while steps < 0 or i + 1 < steps:
            i += 1
            self.t = (i + 1) * dt
            t = (i + 1) * dt
            for cell, tc, factor in sc.r0_change:
                if abs(t - tc) < dt / 2:
                    self.r0[cell] = self.r0_nominal[cell] * factor
            # ---------------- electrical
            in_str = [self.seg_act[k] and self.real[k] for k in range(n)]
            oc = [ocv(self.soc[k]) if self.real[k] else 0.0 for k in range(n)]
            I = 0.0
            if self.pack_act and self.load_act and any(in_str):
                num = sum(oc[k] - self.vrc[k] - self.isc[k] * self.r0[k] for k in range(n) if in_str[k])
                den = P.R_LOAD + P.R_PATH + sum(self.r0[k] for k in range(n) if in_str[k])
                I = num / den
            vterm, heat = [0.0] * n, [0.0] * n
            for k in range(n):
                if not self.real[k]:
                    continue
                s = self.severity(k, t)
                ik = I if in_str[k] else 0.0
                itot = ik + self.isc[k]
                vterm[k] = oc[k] - self.vrc[k] - itot * self.r0[k]
                r_isc = P.R_ISC_MIN / s if s > 1e-3 else float('inf')
                self.isc[k] = vterm[k] / r_isc
                self.soc[k] -= itot * dt / (P.CAPACITY_AH * 3600.0)
                self.vrc[k] += dt / P.TAU_RC * (itot * P.R1 - self.vrc[k])
                exo = P.EXO_POWER * max(0.0, (s - P.EXO_ONSET) / (1 - P.EXO_ONSET))
                heat[k] = itot * itot * self.r0[k] + vterm[k] * self.isc[k] + exo
            taps, node = [0.0] * n, 0.0
            for k in range(n):
                taps[k] = node + vterm[k]
                if self.seg_act[k]:
                    node = taps[k]
            bus = (node - I * P.R_PATH) if self.pack_act else 0.0
            # ---------------- thermal
            p_heater = P.P_HEATER if (self.heater_act and self.ksd_closed) else 0.0
            newT = list(self.T)
            for k in range(n):
                q = heat[k] - (self.T[k] - P.T_AMB) / P.R_TH_AMB
                for j in (k - 1, k + 1):
                    if 0 <= j < n:
                        q -= (self.T[k] - self.T[j]) / P.R_TH_NB
                if self.real[k]:
                    cap = P.M_CELL * P.CP_CELL
                    if P.WAX_MELT[0] <= self.T[k] < P.WAX_MELT[1]:
                        cap += P.WAX_MASS * P.WAX_LATENT / (P.WAX_MELT[1] - P.WAX_MELT[0])
                else:
                    cap = P.C_DUMMY
                    q += (self.Tb - self.T[k]) / P.R_BLOCK_DUMMY
                newT[k] = self.T[k] + dt * q / cap
            if sc.dummy is not None:
                d = sc.dummy
                qb = p_heater - (self.Tb - self.T[d]) / P.R_BLOCK_DUMMY - (self.Tb - P.T_AMB) / P.R_BLOCK_AMB
                self.Tb += dt * qb / P.C_BLOCK
                if self.Tb >= P.KSD301_OPEN:
                    self.ksd_closed = False
                elif self.Tb <= P.KSD301_RESET:
                    self.ksd_closed = True
            self.T = newT
            for k in range(n):
                self.Tp[k] += dt / P.TAU_PROBE * (self.T[k] - self.Tp[k])
            if t >= self.next_ds:  # DS18B20: all 4 conversions together, 750 ms
                self.next_ds = t + P.DS18B20_CONV_S
                self.Tmeas = [round((self.Tp[k] + self.ds_off[k]) / P.DS18B20_LSB) * P.DS18B20_LSB for k in range(n)]
            # ---------------- gas
            rco, rh2 = sc.room_co(t), sc.room_h2(t)
            gas_adc = []
            for k in range(n):
                s = self.severity(k, t)
                self.c_vent[k] += dt * (P.VENT_RATE * s - self.c_vent[k] / P.TAU_POCKET)
                lco = sum(g[3] for g in sc.local_gas if g[0] == k and g[1] <= t < g[2])
                lh2 = sum(g[4] for g in sc.local_gas if g[0] == k and g[1] <= t < g[2])
                self.c_room_co[k] += dt * (rco + lco - self.c_room_co[k]) / P.TAU_POCKET
                self.c_room_h2[k] += dt * (rh2 + lh2 - self.c_room_h2[k]) / P.TAU_POCKET
                co_seen = P.VENT_CO_FRAC * self.c_vent[k] + self.c_room_co[k]
                h2_seen = P.VENT_H2_FRAC * self.c_vent[k] + self.c_room_h2[k]
                self.x_co[k] += dt / P.TAU_MQ * (co_seen - self.x_co[k])
                self.x_h2[k] += dt / P.TAU_MQ * (h2_seen - self.x_h2[k])
                vals = []
                for base, x, ph in ((self.base_co[k], self.x_co[k], self.drift_ph[k]),
                                    (self.base_h2[k], self.x_h2[k], self.drift_ph[n + k])):
                    ao = min(P.MQ_AO_MAX, base * (1 + P.MQ_GAIN * x) * (1 + 0.01 * math.sin(t / 900.0 + ph)))
                    v = ao * P.GAS_DIVIDER + rng.gauss(0, P.MQ_NOISE_V)
                    vals.append(round(v / P.ADS_LSB) * P.ADS_LSB)
                gas_adc.append(vals)
            # ---------------- firmware inputs
            inp = core.inp
            inp.t_s = t
            for k in range(n):
                c = inp.cell[k]
                c.co_v, c.h2_v = gas_adc[k]
                unpl = any(cell == k and t >= tu for cell, tu in sc.unplug)
                c.temp_c = -127.0 if unpl else self.Tmeas[k]
                c.temp_ok = 0 if unpl else 1
                c.tap_v = round(taps[k] * self.gain[k] / P.TAP_RATIO / P.ADS_LSB) * P.ADS_LSB * P.TAP_RATIO
                c.co_ok = c.h2_ok = c.tap_ok = 1
            inp.pack_current_a = I + rng.gauss(0, P.INA_I_NOISE)
            inp.pack_bus_v = round(bus / P.INA_V_LSB) * P.INA_V_LSB
            inp.ambient_c = P.T_AMB + rng.gauss(0, 0.05)
            inp.ambient_rh = 50.0
            inp.current_ok = inp.bus_ok = inp.ambient_ok = 1
            inp.armed = 1
            inp.load_request = (1 if sc.load(t) else 0) if self.user_load is None else int(self.user_load)
            inp.heater_request = 1 if (sc.heater and sc.heater[0] <= t < sc.heater[1]) else 0
            inp.reset_pressed = 1 if any(tr <= t < tr + 0.3 for tr in sc.resets) else 0
            inp.ack_pressed = 1 if t < self.ack_until else 0
            # ---------------- internal-resistance test (firmware sequence), overrides the load request
            prev = core.out
            ii = self.irt.inp
            ii.t_s = t
            for k in range(n):
                ii.cell_v[k] = prev.cell_v[k] if prev.cell_v[k] == prev.cell_v[k] else 0.0
                ii.in_string[k] = prev.in_string[k]
            ii.pack_current_a, ii.current_ok = inp.pack_current_a, 1
            ii.level, ii.load_on_now = prev.level, prev.load_on
            ii.start = 1 if any(ts <= t < ts + dt for ts in sc.ir_tests) else 0
            self.irt.step()
            io = self.irt.out
            if io.load_override >= 0:
                inp.load_request = io.load_override
            if io.result_new:
                r = [io.r_ohm[k] for k in range(n)]
                self.ir_results.append((t, r, [self.r0[k] + P.R1 * (1 - math.exp(-1.0 / P.TAU_RC)) for k in range(n)]))
                if not any(self.ir_base):  # first measurement = commissioning baseline, also used for sag compensation
                    self.ir_base = [x if x == x else 0.0 for x in r]
                    for k in range(n):
                        core.cfg.ir_ohm[k] = self.ir_base[k]
                    core.set_config()
                self.ir_now = [x if x == x else 0.0 for x in r]
            for e in core.step():
                events.append(e)
            out = core.out
            # ---------------- health indicator (advisory)
            hi = self.health.inp
            hi.t_s = t
            for k in range(n):
                hi.cell_v[k] = out.cell_v[k]
                hi.in_string[k] = out.in_string[k]
                hi.temp_ok[k] = inp.cell[k].temp_ok
                hi.temp_c[k] = inp.cell[k].temp_c
                hi.gas_base_co[k] = out.gas_base_co[k]
                hi.gas_base_h2[k] = out.gas_base_h2[k]
                hi.ir_now[k] = self.ir_now[k]
                hi.ir_base[k] = self.ir_base[k]
            hi.pack_current_a, hi.pack_bus_v, hi.current_ok = inp.pack_current_a, inp.pack_bus_v, 1
            hi.gas_warm = 1 if t >= core.cfg.gas_warmup_s else 0
            self.health.step()
            # ---------------- actuators follow at the next step (relay operate time < one step)
            self.seg_act = [out.seg_relay_on[k] for k in range(n)]
            self.pack_act = out.pack_relay_on
            self.heater_act = out.heater_on
            self.load_act = out.load_on
            # ---------------- record
            rec['t'].append(t)
            rec['gas'].append([max(out.gas_rise_co[k], out.gas_rise_h2[k]) for k in range(n)])
            rec['temp'].append([inp.cell[k].temp_c if inp.cell[k].temp_ok else float('nan') for k in range(n)])
            rec['temp_true'].append(list(self.T))
            rec['cell_v'].append([out.cell_v[k] for k in range(n)])
            rec['current'].append(I)
            rec['level'].append(out.level)
            rec['seg'].append([out.seg_relay_on[k] for k in range(n)])
            rec['pack'].append(out.pack_relay_on)
            rec['heater'].append(out.heater_on)
            rec['block'].append(self.Tb)
            rec['duty'].append(out.heater_duty)
            rec['ksd'].append(1 if self.ksd_closed else 0)
            ho = self.health.out
            rec['score'].append([ho.score[k] for k in range(n)])
            rec['soc'].append([ho.soc[k] for k in range(n)])
            yield t

    def summarise(self, rec, events):
        trips = {e[2]: e[0] for e in events if e[1] == 'L2_TRIP'}
        trip_mask = {e[2]: e[3] for e in events if e[1] == 'L2_TRIP'}
        l3 = [e[0] for e in events if e[1] == 'L3_TRIP']
        latency = {}
        for k, tt in trips.items():
            ons = sorted(e[0] for e in events if e[1] == 'SIGNAL_ON' and e[2] == k and (trip_mask[k] >> e[3]) & 1 and e[0] <= tt)
            if len(ons) >= 2:
                raw2 = ons[-1] - self.debounce
                latency[k] = (tt + P.DT) - raw2  # relay physically switched at the next step
        order = {k: [SIG_NAMES[s] for s in range(3) if (trip_mask[k] >> s) & 1] for k in trips}
        ho = self.health.out
        return dict(rec=rec, events=events, trips=trips, trip_signals=order, l3=l3, latency=latency,
                    ir_results=self.ir_results, pack_wh=ho.pack_wh, pack_ah=ho.pack_ah,
                    alerts_seen=sorted({e[3] for e in events if e[1] == 'ALERT_ON'}),
                    max_level=max(rec['level']) if rec['level'] else 0)
