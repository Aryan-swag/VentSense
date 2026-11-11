"""Scenario library. All fault profiles are SYNTHETIC test inputs built from params.py (ASSUMED values);
they are not measured failure data. Cell numbers in titles are 1-based; indices in code are 0-based."""
from twin import Scenario


def ramp(t0, t1, level, t2=None, t3=None):
    """0 before t0, linear up to `level` at t1, hold, linear down from t2 to 0 at t3."""
    def f(t):
        if t < t0:
            return 0.0
        if t < t1:
            return level * (t - t0) / (t1 - t0)
        if t2 is None or t < t2:
            return level
        if t < t3:
            return level * (1 - (t - t2) / (t3 - t2))
        return 0.0
    return f


def no_trip(r):
    return [('no Level 2 trip', not r['trips'], 'trips: %s' % r['trips']),
            ('no Level 3 trip', not r['l3'], '')]


def exp_normal(r):
    return no_trip(r) + [('stays at Level 0', r['max_level'] == 0, 'max level %d' % r['max_level'])]


def exp_incense(r):
    return no_trip(r) + [('ambient-gas alert raised', 1 in r['alerts_seen'], 'alerts %s' % r['alerts_seen'])]


def exp_runaway3(r):
    lat = r['latency'].get(2)
    i_after = r['rec']['current'][-1]
    return [('cell 3 isolated (Level 2)', 2 in r['trips'], 'trips %s' % r['trips']),
            ('only cell 3 isolated', set(r['trips']) == {2}, ''),
            ('no Level 3', not r['l3'], ''),
            ('pack keeps powering the load on 3 cells', i_after > 0.8, 'final current %.2f A' % i_after),
            ('isolation < 1 s after the 2nd signal', lat is not None and lat < 1.0,
             'latency %.2f s' % lat if lat is not None else 'n/a')]


def exp_two_cell(r):
    return [('cells 2 and 3 isolated', {1, 2} <= set(r['trips']), 'trips %s' % r['trips']),
            ('Level 3: pack relay opened', bool(r['l3']) and r['rec']['pack'][-1] == 0, 'L3 at %s' % r['l3']),
            ('pack current 0 after Level 3', r['rec']['current'][-1] == 0.0, '')]


def exp_unplug(r):
    return no_trip(r) + [('sensor-fault alert raised', 128 in r['alerts_seen'], ''),
                         ('max Level 1', r['max_level'] == 1, 'max level %d' % r['max_level'])]


def exp_heater_only(r):
    peak_dummy = max(row[0] for row in r['rec']['temp_true'])
    peak_block = max(r['rec']['block'])
    ksd = r['rec']['ksd']
    opens = sum(1 for a, b in zip(ksd, ksd[1:]) if a == 1 and b == 0)
    return no_trip(r) + [
        ('firmware heater cutoff at 55 C engaged', 16 in r['alerts_seen'], ''),
        ('dummy cell stays below 60 C (25R limit)', peak_dummy < 60.0, 'peak dummy %.1f C' % peak_dummy),
        ('KSD301 backup holds the block near 70 C', peak_block < 72.0,
         'peak block %.1f C, KSD301 opened %d times (block runs hotter than the dummy: ASSUMED coupling)' % (peak_block, opens))]


def exp_rig_vote(r):
    return [('dummy segment 1 trips Level 2 (H2 + temperature)', 0 in r['trips'], 'trips %s' % r['trips']),
            ('real cells untouched', set(r['trips']) <= {0}, ''),
            ('heater off after the trip', r['rec']['heater'][-1] == 0, '')]


def exp_late(r):
    return no_trip(r) + [('reached Level 1 only', r['max_level'] == 1, 'max level %d' % r['max_level'])]


LOAD_CYCLE = lambda t: not (400 <= t < 500)

SCENARIOS = [
    Scenario('normal', 'Normal discharge, load on/off cycle', 900, load=LOAD_CYCLE, expect=exp_normal),
    Scenario('incense_room', 'Incense in the room: all gas sensors rise together', 900,
             room_co=ramp(300, 360, 0.8, 600, 700), room_h2=ramp(300, 360, 0.12, 600, 700), expect=exp_incense),
    Scenario('cell3_runaway', 'Cell 3 failing: vent gas first, then heat and voltage sag', 900,
             faults=[(2, 300, 300, 1.0)], expect=exp_runaway3),
    Scenario('two_cell', 'Cells 2 and 3 failing: Level 3 opens the pack relay', 900,
             faults=[(1, 300, 300, 1.0), (2, 330, 300, 1.0)], expect=exp_two_cell),
    Scenario('sensor_unplug', 'DS18B20 on cell 1 unplugged at 400 s', 700, unplug=[(0, 400)], expect=exp_unplug),
    Scenario('heater_only', 'Heater test on a dummy cell (segment 1): 55 C cutoff, 50 % duty cap', 900,
             dummy=0, heater=(250, 900), expect=exp_heater_only),
    Scenario('rig_vote', 'Rig demo: H2 into the dummy pocket, then heater: 2-of-3 vote', 700,
             dummy=0, local_gas=[(0, 300, 400, 0.1, 0.9)], heater=(330, 700), expect=exp_rig_vote),
    Scenario('late_second', 'H2 pulse, then heat after the window expired: no vote', 1000,
             dummy=0, local_gas=[(0, 300, 330, 0.05, 0.8)], heater=(700, 1000), expect=exp_late),
]
