"""Open-Meteo forecast data, with an explicitly marked offline cache."""
from datetime import datetime, timezone
import json
from pathlib import Path
import urllib.error
import urllib.parse
import urllib.request


def condition(code):
    if code == 0: return 'Clear sky'
    if code == 1: return 'Mostly clear'
    if code == 2: return 'Partly cloudy'
    if code == 3: return 'Overcast'
    if code in (45, 48): return 'Fog'
    if code in (51, 53, 55, 56, 57): return 'Drizzle'
    if code in (61, 63, 65, 66, 67): return 'Rain'
    if code in (71, 73, 75, 77, 85, 86): return 'Snow'
    if code in (80, 81, 82): return 'Rain showers'
    if code in (95, 96, 97, 99): return 'Thunderstorm'
    return 'Unavailable'


def normalize(payload):
    current, daily = payload['current'], payload['daily']
    # Do not replace a usable cache with an incomplete response.
    if not isinstance(current.get('temperature_2m'), (int, float)):
        raise ValueError('Weather response has no current temperature')
    days = []
    for i, date in enumerate(daily['time'][:5]):
        days.append({'date': date, 'code': daily['weather_code'][i],
                     'high': daily['temperature_2m_max'][i], 'low': daily['temperature_2m_min'][i],
                     'rain': daily['precipitation_probability_max'][i]})
    return {'temperature': current['temperature_2m'], 'feels_like': current['apparent_temperature'],
            'humidity': current['relative_humidity_2m'], 'wind': current['wind_speed_10m'],
            'code': current['weather_code'], 'condition': condition(current['weather_code']),
            'is_day': current['is_day'], 'observed_at': current['time'], 'days': days}


def collect(config, cache, offline=False):
    cache = Path(cache)
    location = config['location']
    identity = {key: location[key] for key in ('latitude', 'longitude', 'timezone')}
    try:
        if offline:
            raise OSError('Offline rendering requested')
        query = dict(identity, forecast_days=5,
                     current='temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day',
                     daily='weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max')
        url = 'https://api.open-meteo.com/v1/forecast?' + urllib.parse.urlencode(query)
        request = urllib.request.Request(url, headers={'User-Agent': 'KoboOpenDashboard/0.1'})
        with urllib.request.urlopen(request, timeout=20) as response:
            result = normalize(json.load(response))
        result.update(cached=False, fetched_at=datetime.now(timezone.utc).isoformat(), location=identity)
        cache.parent.mkdir(parents=True, exist_ok=True)
        temporary = cache.with_suffix('.new')
        temporary.write_text(json.dumps(result, indent=2) + '\n')
        temporary.replace(cache)
        return result
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        if cache.exists():
            result = json.loads(cache.read_text())
            if result.get('location') == identity:
                return dict(result, cached=True)
        return {'condition': 'Weather unavailable', 'cached': True, 'days': []}
