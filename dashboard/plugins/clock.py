from datetime import datetime
from zoneinfo import ZoneInfo


def collect(config):
    now = datetime.now(ZoneInfo(config['location']['timezone']))
    return {'time': now.strftime('%H:%M'), 'weekday': now.strftime('%A'),
            'date': now.strftime('%d %b'), 'year': now.year, 'timezone': now.tzname(),
            'rendered_at': now.isoformat()}
