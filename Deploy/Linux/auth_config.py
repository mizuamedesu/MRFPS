"""Fail-closed public-key configuration checks; no private key is accepted here."""
import base64
import binascii
import os
import re
import sys

ED25519_SPKI_PREFIX = bytes.fromhex('302a300506032b6570032100')


def validate_environment(environment=None):
    environment = os.environ if environment is None else environment
    encoded = environment.get('MRFPS_AUTH_PUBLIC_KEY_B64', '')
    literal = environment.get('MRFPS_AUTH_PUBLIC_KEY_PEM', '')
    if bool(encoded) == bool(literal):
        return False
    audience = environment.get('MRFPS_AUTH_AUDIENCE', 'mrfps')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}', audience, re.ASCII):
        return False
    try:
        if len(encoded) > 4096 or len(literal) > 2048:
            return False
        pem = base64.b64decode(encoded, validate=True).decode('ascii') if encoded else literal
        match = re.fullmatch(r'-----BEGIN PUBLIC KEY-----\r?\n([A-Za-z0-9+/=\r\n]+)\r?\n-----END PUBLIC KEY-----\r?\n?', pem)
        if not match:
            return False
        der = base64.b64decode(match[1].replace('\r', '').replace('\n', ''), validate=True)
        return len(der) == 44 and der.startswith(ED25519_SPKI_PREFIX)
    except (ValueError, UnicodeError, binascii.Error):
        return False


if __name__ == '__main__':
    valid = validate_environment()
    if not valid:
        print('AUTH_CONFIG: exactly one valid Ed25519 public-key environment setting and a valid audience are required.', file=sys.stderr)
    sys.exit(0 if valid else 78)
