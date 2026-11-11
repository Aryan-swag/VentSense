"""ctypes binding to the real firmware safety core (firmware/lib/safety_core).

The struct layouts mirror safety_core.h field by field; load() checks every size against the
library so a mismatch fails loudly instead of corrupting memory.
"""
import ctypes as C
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(HERE, '..', 'firmware', 'lib', 'safety_core', 'src')
SRCS = [os.path.join(SRC_DIR, f) for f in ('safety_core.cpp', 'health.cpp', 'sequences.cpp', 'config_table.cpp')]
LIB = os.path.join(HERE, 'build', 'libvscore.so')
N, NS = 4, 3
f32, i32, u8, u32 = C.c_float, C.c_int32, C.c_uint8, C.c_uint32

SIG_GAS, SIG_TEMP, SIG_VOLT = 0, 1, 2
SIG_NAMES = {0: 'gas', 1: 'temperature', 2: 'voltage'}
EV = {1: 'SIGNAL_ON', 2: 'SIGNAL_OFF', 3: 'L2_TRIP', 4: 'L3_TRIP', 5: 'RESET', 6: 'FAULT_ON', 7: 'FAULT_OFF',
      8: 'ALERT_ON', 9: 'ALERT_OFF', 10: 'ARMED', 11: 'DISARMED', 12: 'ACK'}
ALERT_AMBIENT_GAS, ALERT_RELAY_MISMATCH, ALERT_SENSOR_FAULT, ALERT_HEATER_CUTOFF = 1, 2, 128, 16
FAULT_TEMP = 4


class Config(C.Structure):
    _fields_ = [
        ('window_s', f32), ('debounce_s', f32),
        ('gas_warmup_s', f32), ('gas_rise_frac', f32), ('gas_rate_frac_per_min', f32),
        ('gas_rate_min_rise_frac', f32), ('gas_cm_margin_frac', f32), ('gas_cm_reject', i32),
        ('gas_ambient_min_cells', i32), ('gas_baseline_tau_s', f32), ('gas_min_baseline_v', f32),
        ('gas_fault_low_v', f32), ('gas_fault_high_v', f32), ('gas_slope_tau_s', f32),
        ('temp_abs_c', f32), ('temp_above_ambient_c', f32), ('temp_above_neighbors_c', f32),
        ('temp_rate_c_per_min', f32), ('temp_slope_tau_s', f32), ('temp_valid_min_c', f32), ('temp_valid_max_c', f32),
        ('volt_sag_v', f32), ('ir_ohm', f32 * N), ('cell_v_min_valid', f32), ('segment_enabled', i32 * N), ('cell_uv_v', f32),
        ('relay_settle_s', f32), ('relay_check_tol_v', f32), ('path_r_ohm', f32),
        ('heater_cell', i32), ('heater_cutoff_c', f32), ('heater_rearm_c', f32), ('heater_max_duty', f32),
        ('heater_duty_tau_s', f32)]


class CellIn(C.Structure):
    _fields_ = [('co_v', f32), ('h2_v', f32), ('temp_c', f32), ('tap_v', f32),
                ('co_ok', u8), ('h2_ok', u8), ('temp_ok', u8), ('tap_ok', u8)]


class Inputs(C.Structure):
    _fields_ = [('t_s', f32), ('cell', CellIn * N), ('pack_current_a', f32), ('pack_bus_v', f32),
                ('ambient_c', f32), ('ambient_rh', f32), ('current_ok', u8), ('bus_ok', u8), ('ambient_ok', u8),
                ('armed', u8), ('reset_pressed', u8), ('ack_pressed', u8), ('heater_request', u8), ('load_request', u8)]


class Outputs(C.Structure):
    _fields_ = [('seg_relay_on', u8 * N), ('pack_relay_on', u8), ('heater_on', u8), ('load_on', u8), ('buzzer_mode', u8),
                ('level', u8), ('cell_level', u8 * N), ('cell_latched', u8 * N), ('pack_latched', u8),
                ('sig_active', (u8 * NS) * N), ('cell_faults', u8 * N), ('trip_order', (u8 * NS) * N), ('alerts', u32),
                ('cell_v', f32 * N), ('gas_rise_co', f32 * N), ('gas_rise_h2', f32 * N), ('temp_rate_c_per_min', f32 * N),
                ('volt_dev_v', f32 * N), ('trip_time_s', f32 * N), ('pack_trip_time_s', f32), ('heater_duty', f32),
                ('gas_base_co', f32 * N), ('gas_base_h2', f32 * N), ('in_string', u8 * N), ('pad_', u8 * 4)]


class HealthCfg(C.Structure):
    _fields_ = [('capacity_ah', f32), ('ocv_table', f32 * 11), ('rest_current_a', f32), ('rest_time_s', f32),
                ('ir_ratio_span', f32), ('imbalance_span_v', f32), ('temp_span_c', f32), ('gas_drift_span', f32),
                ('w_ir', f32), ('w_imb', f32), ('w_temp', f32), ('w_gas', f32)]


class HealthIn(C.Structure):
    _fields_ = [('t_s', f32), ('cell_v', f32 * N), ('in_string', u8 * N), ('temp_ok', u8 * N), ('temp_c', f32 * N),
                ('pack_current_a', f32), ('pack_bus_v', f32), ('current_ok', u8), ('gas_warm', u8), ('pad_', u8 * 2),
                ('gas_base_co', f32 * N), ('gas_base_h2', f32 * N), ('ir_now', f32 * N), ('ir_base', f32 * N)]


class HealthOut(C.Structure):
    _fields_ = [('soc', f32 * N), ('ah_out', f32 * N), ('pack_ah', f32), ('pack_wh', f32), ('score', f32 * N),
                ('sub_ir', f32 * N), ('sub_imb', f32 * N), ('sub_temp', f32 * N), ('sub_gas', f32 * N)]


class IrIn(C.Structure):
    _fields_ = [('t_s', f32), ('cell_v', f32 * N), ('in_string', u8 * N), ('pack_current_a', f32), ('current_ok', u8),
                ('level', u8), ('load_on_now', u8), ('start', u8)]


class IrOut(C.Structure):
    _fields_ = [('load_override', C.c_int8), ('state', u8), ('result_new', u8), ('abort_reason', u8),
                ('r_ohm', f32 * N), ('di_a', f32)]


class Event(C.Structure):
    _fields_ = [('t_s', f32), ('type', i32), ('cell', i32), ('detail', i32)]


def build(force=False):
    """Compile the firmware safety core into a shared library (same source the ESP32 uses)."""
    os.makedirs(os.path.dirname(LIB), exist_ok=True)
    newest = max(os.path.getmtime(f) for f in SRCS + [os.path.join(SRC_DIR, h) for h in os.listdir(SRC_DIR)])
    if force or not os.path.exists(LIB) or os.path.getmtime(LIB) < newest:
        subprocess.check_call(['g++', '-std=gnu++17', '-O2', '-shared', '-fPIC', '-Wall'] + SRCS + ['-o', LIB])
    return LIB


_lib = None


def load():
    global _lib
    if _lib is None:
        _lib = C.CDLL(build())
        _lib.vs_create.restype = C.c_void_p
        _lib.vs_create.argtypes = [C.POINTER(Config)]
        _lib.vs_destroy.argtypes = [C.c_void_p]
        _lib.vs_step.argtypes = [C.c_void_p, C.POINTER(Inputs), C.POINTER(Outputs)]
        _lib.vs_pop_event.argtypes = [C.c_void_p, C.POINTER(Event)]
        _lib.vs_pop_event.restype = C.c_int
        _lib.vs_default_config.argtypes = [C.POINTER(Config)]
        _lib.vs_set_config.argtypes = [C.c_void_p, C.POINTER(Config)]
        _lib.vs_config_json.argtypes = [C.POINTER(Config), C.c_char_p, C.c_size_t]
        _lib.vs_config_json.restype = C.c_size_t
        _lib.vs_config_set.argtypes = [C.POINTER(Config), C.c_char_p, C.c_float, C.c_char_p, C.c_size_t]
        _lib.vs_config_set.restype = C.c_int
        for fn_name in ('vs_health_create', 'vs_irtest_create'):
            getattr(_lib, fn_name).restype = C.c_void_p
        _lib.vs_health_create.argtypes = [C.c_void_p]
        _lib.vs_irtest_create.argtypes = [C.c_void_p]
        _lib.vs_health_step.argtypes = [C.c_void_p, C.POINTER(HealthIn), C.POINTER(HealthOut)]
        _lib.vs_irtest_step.argtypes = [C.c_void_p, C.POINTER(IrIn), C.POINTER(IrOut)]
        for name, st in (('config', Config), ('inputs', Inputs), ('outputs', Outputs), ('event', Event),
                         ('health_config', HealthCfg), ('health_in', HealthIn), ('health_out', HealthOut),
                         ('irtest_in', IrIn), ('irtest_out', IrOut)):
            fn = getattr(_lib, 'vs_sizeof_' + name)
            fn.restype = C.c_size_t
            if fn() != C.sizeof(st):
                raise RuntimeError('struct %s size mismatch: C %d vs Python %d' % (name, fn(), C.sizeof(st)))
    return _lib


class SafetyCore:
    def __init__(self, tweak=None):
        lib = load()
        self.cfg = Config()
        lib.vs_default_config(C.byref(self.cfg))
        if tweak:
            tweak(self.cfg)
        self.h = lib.vs_create(C.byref(self.cfg))
        self.inp, self.out, self._ev = Inputs(), Outputs(), Event()

    def set_config(self):
        _lib.vs_set_config(self.h, C.byref(self.cfg))

    def config_json(self):
        buf = C.create_string_buffer(8192)
        _lib.vs_config_json(C.byref(self.cfg), buf, 8192)
        return buf.value.decode()

    def config_set(self, key, value):
        """range-checked by the firmware's own settings table; returns '' or the reason it was rejected"""
        err = C.create_string_buffer(160)
        ok = _lib.vs_config_set(C.byref(self.cfg), key.encode(), float(value), err, 160)
        if ok:
            self.set_config()
        return '' if ok else err.value.decode()

    def step(self):
        _lib.vs_step(self.h, C.byref(self.inp), C.byref(self.out))
        evs = []
        while _lib.vs_pop_event(self.h, C.byref(self._ev)):
            e = self._ev
            evs.append((e.t_s, EV.get(e.type, e.type), e.cell, e.detail))
        return evs

    def __del__(self):
        try:
            _lib.vs_destroy(self.h)
        except Exception:
            pass


class Health:
    """health.cpp: charge/energy, state of charge, health indicator (advisory only)"""
    def __init__(self):
        load()
        self.h = _lib.vs_health_create(None)
        self.inp, self.out = HealthIn(), HealthOut()

    def step(self):
        _lib.vs_health_step(self.h, C.byref(self.inp), C.byref(self.out))


class IrTest:
    """sequences.cpp: internal resistance per cell from a load step (DCIR at 1 s)"""
    def __init__(self):
        load()
        self.h = _lib.vs_irtest_create(None)
        self.inp, self.out = IrIn(), IrOut()

    def step(self):
        _lib.vs_irtest_step(self.h, C.byref(self.inp), C.byref(self.out))
