import os

HOST  = os.getenv("BRIDGE_HOST",  "0.0.0.0")
PORT  = int(os.getenv("BRIDGE_PORT",  "8765"))

# Inference thresholds (tune after calibration)
OCCUPANCY_THRESHOLD_AMP_STD    = 2.0   # amp_std above baseline
OCCUPANCY_THRESHOLD_ENERGY_DELTA = 5.0 # absolute energy shift

# Feature window settings
WINDOW_SECONDS   = 1.0   # seconds of features to aggregate
MAX_BUFFER_POINTS = 300  # per-device rolling history

# Stale data timeout (seconds)
STALE_TIMEOUT = 5.0

# Zone definitions: list of zone names (labels used in inference output)
ZONES = ["zone_1", "zone_2", "zone_3"]
