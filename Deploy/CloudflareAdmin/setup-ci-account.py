"""Store the sole Wrangler OAuth account ID as a GitHub Actions secret.

This does not create, print, or copy the Wrangler OAuth token. Create a dedicated
Cloudflare API token separately and put it in CLOUDFLARE_API_TOKEN.
"""
import json
import os
import pathlib
import subprocess
import tomllib
import urllib.request

base = pathlib.Path(os.environ['XDG_CONFIG_HOME']) if os.environ.get('XDG_CONFIG_HOME') else (pathlib.Path(os.environ['APPDATA']) / 'xdg.config' if os.name == 'nt' else pathlib.Path.home() / '.config')
config = base / '.wrangler' / 'config' / 'default.toml'
token = tomllib.loads(config.read_text())['oauth_token']
request = urllib.request.Request('https://api.cloudflare.com/client/v4/accounts', headers={'Authorization':'Bearer ' + token})
with urllib.request.urlopen(request, timeout=15) as result:
    accounts = json.load(result)['result']
if len(accounts) != 1:
    raise SystemExit('Expected exactly one Cloudflare account. Set CLOUDFLARE_ACCOUNT_ID manually.')
subprocess.run(['gh','secret','set','CLOUDFLARE_ACCOUNT_ID'],input=accounts[0]['id'].encode(),check=True)
