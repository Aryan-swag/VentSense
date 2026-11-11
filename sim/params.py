"""Digital-twin parameters.

Every value is tagged:
  SPEC     - from the design spec / the BOM (our own design numbers)
  DATASHEET- from a part datasheet (link to verify noted)
  ASSUMED  - a modelling guess; replace with a datasheet value, a cited paper or a rig measurement
Gas concentrations are in arbitrary units (a.u.): c = 1 roughly doubles an MQ module's output.
The simulation shows the firmware logic works against these modelled faults. It does NOT prove
detection on real cells.
"""

DT = 0.1                 # s, plant and firmware step (firmware safety task runs at 10 Hz)  SPEC (our design)
N = 4

# ---------------------------------------------------------------- cells (Samsung INR18650-25R)
CAPACITY_AH = 2.5        # BOM: 2500 mAh nominal
SOC0 = 0.90              # ASSUMED start state of charge
# generic NMC open-circuit-voltage curve, SoC 0..1 in 0.1 steps. ASSUMED: replace with a measured 25R curve
OCV_TABLE = [3.00, 3.45, 3.55, 3.62, 3.68, 3.74, 3.82, 3.90, 3.98, 4.08, 4.20]
R0 = 0.025               # ohm, ASSUMED (check 25R datasheet impedance)
R0_SPREAD = 0.15         # +-15 % cell-to-cell, ASSUMED
R1 = 0.012               # ohm, ASSUMED polarisation resistance
TAU_RC = 30.0            # s, ASSUMED polarisation time constant

# ---------------------------------------------------------------- pack path / load
R_LOAD = 10.0            # ohm, SPEC (10 ohm 50 W)
R_PATH = 0.20            # ohm, ASSUMED: 0.1 ohm INA219 shunt (SPEC) + fuse, relays, wiring
RELAY_DELAY_STEPS = 1    # relay follows the command at the next 100 ms step (Songle operate time is ~10 ms, DATASHEET to verify)

# ---------------------------------------------------------------- thermal
T_AMB = 27.0             # C, ASSUMED lab ambient
M_CELL = 0.045           # kg, ASSUMED (check 25R datasheet mass)
CP_CELL = 900.0          # J/kg/K, ASSUMED
R_TH_AMB = 25.0          # K/W cell -> ambient through carrier + wax jacket, ASSUMED
R_TH_NB = 60.0           # K/W cell -> neighbour cell, ASSUMED
WAX_MASS = 0.02          # kg per cell jacket, ASSUMED
WAX_LATENT = 200e3       # J/kg paraffin, ASSUMED (literature range to cite)
WAX_MELT = (58.0, 60.0)  # C, SPEC
TAU_PROBE = 5.0          # s, DS18B20 probe-to-cell-wall lag, ASSUMED

# heater test rig (dummy cell in an aluminium block)
P_HEATER = 40.0          # W, SPEC
C_BLOCK = 0.05 * 900.0   # J/K, ASSUMED 50 g aluminium block
R_BLOCK_DUMMY = 2.0      # K/W block -> dummy cell, ASSUMED
R_BLOCK_AMB = 15.0       # K/W, ASSUMED
C_DUMMY = 0.045 * 900.0  # J/K, ASSUMED aluminium dummy cell
KSD301_OPEN = 70.0       # C, SPEC (KSD301 70 C, normally closed)
KSD301_RESET = 55.0      # C, ASSUMED reset differential (check KSD301 datasheet)

# ---------------------------------------------------------------- vent gas and MQ modules (a.u.)
TAU_POCKET = 60.0        # s, gas pocket exchange with room air, ASSUMED
TAU_MQ = 20.0            # s, MQ module response time constant, ASSUMED (check Winsen/Hanwei datasheets)
VENT_CO_FRAC = 0.6       # share of vent gas the MQ-7 responds to, ASSUMED
VENT_H2_FRAC = 0.8       # share of vent gas the MQ-8 responds to, ASSUMED
INCENSE_H2_FRAC = 0.15   # MQ-8 cross-sensitivity to incense smoke, ASSUMED
MQ_AO_BASE = (0.9, 1.3)  # V, module output in clean air (uniform range), ASSUMED
MQ_GAIN = 1.0            # output multiplier per a.u.: AO = base * (1 + gain * c), ASSUMED
MQ_AO_MAX = 4.9          # V, ASSUMED module saturation
GAS_DIVIDER = 22.0 / 37.0  # SPEC 15k/22k
MQ_NOISE_V = 0.003       # V at the ADC, ASSUMED

# ---------------------------------------------------------------- failing-cell model (synthetic, a.u.)
# severity s rises 0 -> 1 over RAMP_S after onset. Vent gas follows s at once ("gas before heat" is
# the project premise; cite literature for real vent timing). Heat appears after s passes EXO_ONSET.
FAULT_RAMP_S = 300.0      # ASSUMED
VENT_RATE = 0.06          # a.u./s at s = 1, ASSUMED
EXO_ONSET = 0.35          # ASSUMED
EXO_POWER = 2.5           # W at s = 1, ASSUMED side-reaction heat
R_ISC_MIN = 4.0           # ohm internal-short resistance at s = 1, ASSUMED

# ---------------------------------------------------------------- measurement chain
ADS_LSB = 4.096 / 32768   # V, DATASHEET ADS1115 +-4.096 V range
TAP_RATIO = 6.0           # SPEC 100k/20k divider
TAP_GAIN_ERR = 0.001      # +-0.1 % residual after two-point calibration, ASSUMED
DS18B20_LSB = 0.0625      # C, DATASHEET (12-bit)
DS18B20_CONV_S = 0.75     # s, DATASHEET max conversion time
DS18B20_OFFSET = 0.3      # +-C, ASSUMED typical (datasheet limit +-0.5 C, -10..85 C)
INA_I_NOISE = 0.001       # A, ASSUMED
INA_V_LSB = 0.004         # V, DATASHEET INA219 bus-voltage LSB
