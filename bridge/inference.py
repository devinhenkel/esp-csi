"""
Inference module: occupancy detection and zone classification.

Rule-based MVP — tune thresholds in config.py after calibration.
Replace classify() with an ML model by swapping the body of that function.
"""
from __future__ import annotations
import time
from collections import deque
from typing import Optional
import config


class Baseline:
    """Running statistics for the empty-room reference."""

    def __init__(self):
        self.amp_mean:     Optional[float] = None
        self.amp_std:      Optional[float] = None
        self.energy:       Optional[float] = None
        self.calibrated_at: Optional[float] = None
        self._samples: list[dict] = []

    def add_sample(self, features: dict):
        self._samples.append(features)

    def commit(self) -> bool:
        if not self._samples:
            return False
        n = len(self._samples)
        self.amp_mean = sum(s["amp_mean"] for s in self._samples) / n
        self.amp_std  = sum(s["amp_std"]  for s in self._samples) / n
        # proxy energy: mean(amp_mean^2)
        self.energy   = sum(s["amp_mean"] ** 2 for s in self._samples) / n
        self.calibrated_at = time.time()
        self._samples.clear()
        return True

    def is_ready(self) -> bool:
        return self.amp_mean is not None


class InferenceEngine:
    def __init__(self):
        self.baselines:      dict[str, Baseline] = {}  # keyed by rx device_id
        self.feature_window: dict[str, deque]    = {}  # sliding window per rx
        self._last_state:    Optional[dict]      = None

    def _get_baseline(self, device_id: str) -> Baseline:
        if device_id not in self.baselines:
            self.baselines[device_id] = Baseline()
        return self.baselines[device_id]

    def _get_window(self, device_id: str) -> deque:
        if device_id not in self.feature_window:
            self.feature_window[device_id] = deque(maxlen=50)
        return self.feature_window[device_id]

    def add_calibration_sample(self, device_id: str, features: dict):
        self._get_baseline(device_id).add_sample(features)

    def commit_calibration(self, device_id: str) -> bool:
        return self._get_baseline(device_id).commit()

    def commit_all_calibration(self) -> dict:
        results = {}
        for dev_id, bl in self.baselines.items():
            results[dev_id] = bl.commit()
        return results

    def ingest(self, payload: dict) -> Optional[dict]:
        """
        Feed one receiver feature payload. Returns an inference result dict
        when enough data is available, otherwise None.
        """
        dev_id   = payload.get("device_id", "rx-unknown")
        features = payload.get("features")
        ts_ms    = payload.get("ts_ms", int(time.time() * 1000))
        if not features:
            return None

        window = self._get_window(dev_id)
        window.append({"ts_ms": ts_ms, "features": features})

        # Need at least a few frames before classifying
        if len(window) < 5:
            return None

        return self._classify(window, ts_ms)

    def _classify(self, window: deque, ts_ms: int) -> dict:
        # Aggregate window features
        n         = len(window)
        amp_std   = sum(w["features"]["amp_std"]      for w in window) / n
        e_delta   = sum(w["features"]["energy_delta"] for w in window) / n
        amp_mean  = sum(w["features"]["amp_mean"]     for w in window) / n

        # Occupancy: threshold on amp_std elevation and energy shift
        occ_score = 0.0
        if amp_std > config.OCCUPANCY_THRESHOLD_AMP_STD:
            occ_score += 0.5
        if abs(e_delta) > config.OCCUPANCY_THRESHOLD_ENERGY_DELTA:
            occ_score += 0.5

        occupied    = occ_score >= 0.5
        probability = min(1.0, occ_score + 0.1 * (amp_std / max(amp_std, 0.1)))

        # Zone classification: simple proxy using amp_mean magnitude ranges.
        # Replace this with a trained model after calibration data is gathered.
        zone, confidence = self._zone_from_features(amp_mean, amp_std, e_delta)

        state = {
            "ts_ms": ts_ms,
            "occupancy": {
                "occupied":    occupied,
                "probability": round(probability, 3),
            },
            "position": {
                "zone":       zone,
                "confidence": round(confidence, 3),
            },
        }
        self._last_state = state
        return state

    def _zone_from_features(
        self, amp_mean: float, amp_std: float, e_delta: float
    ) -> tuple[str, float]:
        """
        Rudimentary spatial heuristic.  Swap for a trained model.
        Zones are ordered by expected signal strength:
          zone_1 (strong, close) → zone_3 (weak, far)
        """
        zones = config.ZONES
        if not zones:
            return "unknown", 0.0

        # Low signal / low variance → far zone
        if amp_mean < 5.0:
            return zones[-1], 0.55
        # High signal / high variance → near zone
        if amp_mean > 20.0 and amp_std > 3.0:
            return zones[0], 0.65
        # Mid range
        if len(zones) >= 3:
            return zones[1], 0.50
        return zones[0], 0.45

    @property
    def last_state(self) -> Optional[dict]:
        return self._last_state
