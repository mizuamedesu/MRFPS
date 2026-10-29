"""Check whether the existing Wrangler OAuth can manage API-token permission groups.

Prints only the HTTP status; never prints credentials or response bodies.
"""
import os
import pathlib
import tomllib
import urllib.error
import urllib.request

base = pathlib.Path(os.environ['XDG_CONFIG_HOME']) if os.environ.get('XDG_CONFIG_HOME') else (pathlib.Path(os.environ['APPDATA']) / 'xdg.config' if os.name == 'nt' else pathlib.Path.home() / '.config')
config = base / '.wrangler' / 'config' / 'default.toml'
token = tomllib.loads(config.read_text())['oauth_token']
request = urllib.request.Request('https://api.cloudflare.com/client/v4/user/tokens/permission_groups', headers={'Authorization':'Bearer ' + token})
try:
    with urllib.request.urlopen(request, timeout=15) as result:
        print('API token permission-group access:', result.status)
except urllib.error.HTTPError as error:
    print('API token permission-group access:', error.code)
