import unittest
from water_setup import thresholds


class CalibrationTests(unittest.TestCase):
    def test_separated_thresholds_between_dry_and_wet(self):
        dry, wet = thresholds([50, 100, 90], [800, 900, 850])
        self.assertTrue(100 < dry < wet < 800)

    def test_rejects_overlapping_unplugged_or_reversed_sensor(self):
        for dry, wet in [([100, 300], [250, 500]), ([0, 1], [2, 3]), ([900], [0]), ([-1], [1000])]:
            with self.assertRaises(RuntimeError):
                thresholds(dry, wet)
