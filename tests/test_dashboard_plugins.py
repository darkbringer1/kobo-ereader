import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from dashboard.plugins import device, weather


class DashboardPluginTests(unittest.TestCase):
    def test_actual_radio_reading_and_battery_are_preserved(self):
        values = device.parse('BATTERY=58\nSTATUS=Discharging\nRSSI=-55\n')
        self.assertEqual((values['battery'], values['rssi']), (58, -55))
        self.assertFalse(values['cached'])

    def test_missing_radio_is_not_reported_as_full_strength(self):
        self.assertIsNone(device.parse('BATTERY=0\nRSSI=\n')['rssi'])
        with self.assertRaises(ValueError):
            device.parse('BATTERY=101\nRSSI=-55\n')

    def test_failed_weather_fetch_keeps_cache_and_marks_it_stale(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'weather.json'
            location = {'name': 'Berlin', 'latitude': 52.52, 'longitude': 13.405, 'timezone': 'Europe/Berlin'}
            identity = {key: location[key] for key in ('latitude', 'longitude', 'timezone')}
            original = json.dumps({'temperature': 13, 'location': identity, 'cached': False})
            cache.write_text(original)
            with patch('dashboard.plugins.weather.urllib.request.urlopen', side_effect=OSError('Offline')):
                values = weather.collect({'location': location}, cache)
            self.assertEqual(values['temperature'], 13)
            self.assertTrue(values['cached'])
            self.assertEqual(cache.read_text(), original)

    def test_another_city_never_uses_the_berlin_weather_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'weather.json'
            cache.write_text(json.dumps({'temperature': 13, 'location': {'latitude': 52.52}}))
            values = weather.collect({'location': {'latitude': 1, 'longitude': 1, 'timezone': 'UTC'}}, cache, offline=True)
            self.assertEqual(values['condition'], 'Weather unavailable')
            self.assertNotIn('temperature', values)
