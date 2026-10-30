import unittest

from check_device import HandObservation


class ActualStreamObservationTests(unittest.TestCase):
    def test_never_received_input_is_not_fresh_or_moving(self):
        report = HandObservation().report(10.0, 0.15)
        self.assertFalse(report["flex_fresh"])
        self.assertFalse(report["measured_motion"])
        self.assertIsNone(report["last_received_curls_thumb_to_little"])

    def test_freshness_expires_even_if_last_values_were_open(self):
        observation = HandObservation()
        observation.observe_connection(True, 80, 2.0)
        observation.observe("/DS/HC/Hand", [1, "fixture", 1] + [0.0] * 20, 2.0)
        self.assertTrue(observation.report(2.1, 0.15)["flex_fresh"])
        self.assertFalse(observation.report(2.2, 0.15)["flex_fresh"])

    def test_motion_requires_measured_change_and_never_claims_position(self):
        observation = HandObservation()
        observation.observe_connection(True, 80, 2.0)
        observation.observe("/DS/HC/Hand", [1, "fixture", 1] + [0.0] * 20, 2.0)
        observation.observe("/DS/HC/Hand", [1, "fixture", 1] + [0.5] * 20, 2.1)
        report = observation.report(2.15, 0.15)
        self.assertTrue(report["measured_motion"])
        self.assertFalse(report["positional_tracking_verified"])
        self.assertEqual(report["measured_curl_spans"], [0.5] * 5)

    def test_disconnected_dongle_default_zero_frames_are_not_valid_input(self):
        observation = HandObservation()
        observation.observe_connection(False, 0, 2.0)
        observation.observe("/DS/HC/Hand", [1, "fixture", 1] + [0.0] * 20, 2.05)
        report = observation.report(2.1, 0.15)
        self.assertTrue(report["flex_stream_fresh"])
        self.assertTrue(report["device_status_fresh"])
        self.assertFalse(report["connected"])
        self.assertFalse(report["flex_fresh"])
        self.assertFalse(report["measured_motion"])
        self.assertEqual(report["connected_flex_samples"], 0)

    def test_fresh_hand_packets_cannot_keep_connection_status_alive(self):
        observation = HandObservation()
        observation.observe_connection(True, 80, 2.0)
        observation.observe("/DS/HC/Hand", [1, "fixture", 1] + [0.3] * 20, 3.04)
        report = observation.report(3.05, 0.15)
        self.assertTrue(report["flex_stream_fresh"])
        self.assertFalse(report["connected"])
        self.assertFalse(report["flex_fresh"])

    def test_recent_samples_survive_a_slower_connected_status(self):
        observation = HandObservation()
        observation.observe_connection(True, 80, 2.0)
        observation.observe("/DS/HC/Hand", [1, "fixture", 1] + [0.3] * 20, 2.4)
        report = observation.report(2.45, 0.15)
        self.assertTrue(report["device_status_fresh"])
        self.assertTrue(report["flex_fresh"])


if __name__ == "__main__":
    unittest.main()
