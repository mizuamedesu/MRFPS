"""Update only the owned, direct-mode Flash service without printing secrets.

The complete current spec is preserved; only the immutable image and the
roster HMAC environment variable can change. A pending intent prevents a
blind retry after an ambiguous cloud response.
"""
import argparse
import copy
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import time
from urllib.request import urlopen

from flash_service import HeteroCLI, digest, owned_service, save, summary

IMAGE = re.compile(r"ghcr\.io/[a-z0-9_.-]+/[a-z0-9_.-]+@sha256:[0-9a-f]{64}\Z")
ROSTER_KEY = re.compile(r"[0-9a-f]{64}\Z")


def cloudflare_cidrs():
    networks = []
    with urlopen("https://api.cloudflare.com/client/v4/ips", timeout=10) as response:
        body = response.read(8193)
    if len(body) > 8192:
        raise ValueError("CLOUDFLARE_CIDR_RESPONSE_TOO_LARGE")
    document = json.loads(body)
    if document.get("success") is not True or not isinstance(document.get("result"), dict):
        raise ValueError("CLOUDFLARE_CIDR_RESPONSE_INVALID")
    for version in (4, 6):
        ranges = document["result"].get(f"ipv{version}_cidrs")
        if not isinstance(ranges, list) or not ranges:
            raise ValueError("CLOUDFLARE_CIDR_RESPONSE_INVALID")
        for item in ranges:
            network = ipaddress.ip_network(item, strict=True)
            if network.version != version or network.prefixlen < (8 if version == 4 else 16):
                raise ValueError("CLOUDFLARE_CIDR_RESPONSE_INVALID")
            networks.append(network)
    if not networks or len(networks) > 64 or len(set(networks)) != len(networks):
        raise ValueError("CLOUDFLARE_CIDR_RESPONSE_INVALID")
    addresses = {ipaddress.ip_address(item[4][0]) for item in socket.getaddrinfo("mrfps-admin.mizuame.app", 443, type=socket.SOCK_STREAM)}
    if not addresses or not any(ip.version == 4 for ip in addresses):
        raise ValueError("ADMIN_DNS_NOT_RESOLVED")
    if any(not any(ip in network for network in networks if ip.version == network.version) for ip in addresses):
        raise ValueError("ADMIN_DNS_OUTSIDE_CLOUDFLARE_CIDRS")
    return [str(network) for network in networks]


def update(client, state_path, image, key, cidrs):
    if not IMAGE.fullmatch(image) or not ROSTER_KEY.fullmatch(key):
        raise ValueError("IMMUTABLE_IMAGE_AND_256_BIT_HEX_KEY_REQUIRED")
    state = json.loads(state_path.read_text(encoding="utf-8-sig"))
    project, name = state["project_id"], state["service_name"]
    service = owned_service(client, state, project, name)
    if not service:
        raise ValueError("OWNED_FLASH_SERVICE_REQUIRED")
    live = service["spec"]
    if live.get("exposure", {}).get("traffic_mode") != "direct" or live.get("exposure", {}).get("type") != "public":
        raise ValueError("DIRECT_PUBLIC_FLASH_MODE_REQUIRED")
    game_ports = [p for p in live.get("ports", []) if p.get("protocol") == "udp" and p.get("container_port") == 7777]
    if len(game_ports) != 1 or type(game_ports[0].get("service_port")) is not int or not 1 <= game_ports[0]["service_port"] <= 65535:
        raise ValueError("DIRECT_GAME_PORT_UNVERIFIED")
    desired = copy.deepcopy(live)
    if not isinstance(desired.get("env"), dict):
        raise ValueError("FLASH_ENVIRONMENT_SHAPE_UNVERIFIED")
    desired["image"] = image
    desired["env"]["MRFPS_ROSTER_HMAC_KEY"] = key
    if not isinstance(desired.get("egress"), dict) or not cidrs or len(cidrs) > 64:
        raise ValueError("FLASH_EGRESS_CONFIGURATION_UNVERIFIED")
    networks = [ipaddress.ip_network(cidr, strict=True) for cidr in cidrs]
    denied = [ipaddress.ip_network(cidr, strict=True) for cidr in desired["egress"].get("denied_destination_cidrs", [])]
    if any(a.overlaps(b) for a in networks for b in denied if a.version == b.version):
        raise ValueError("CLOUDFLARE_EGRESS_DENIED_BY_EXISTING_POLICY")
    desired["egress"]["mode"] = "restricted"
    desired["egress"]["allowed_destination_cidrs"] = [str(network) for network in networks]
    old_hash, wanted_hash = digest(live), digest(desired)
    pending = state.get("pending_spec_sha256")
    if pending:
        if pending != wanted_hash:
            raise ValueError("FLASH_PENDING_DIFFERENT_UPDATE_REQUIRES_REVIEW")
        if old_hash != pending:
            raise ValueError("FLASH_PENDING_UPDATE_NOT_YET_OBSERVED")
        state.pop("pending_spec_sha256", None)
        state.update(desired_spec_sha256=wanted_hash, observed_spec_sha256=old_hash)
        save(state_path, state)
        return {**summary(service), "operation": "pending-reconciled", "traffic_mode": "direct"}
    if old_hash != state.get("observed_spec_sha256"):
        raise ValueError("OWNED_FLASH_SPEC_DRIFT_REVIEW_REQUIRED")
    if old_hash == wanted_hash:
        return {**summary(service), "operation": "unchanged", "traffic_mode": "direct"}
    # Persist only a digest, never the key or manifest. On any ambiguous
    # response a later invocation must observe this exact intent before acting.
    state["pending_spec_sha256"] = wanted_hash
    save(state_path, state)
    client.call("flash", "update", service["id"], "--file", "-", "--no-wait",
                payload={"project_id": project, "name": name, "spec": desired})
    for _ in range(20):
        observed = owned_service(client, state, project, name)
        if observed and digest(observed["spec"]) == wanted_hash:
            state.pop("pending_spec_sha256", None)
            state.update(desired_spec_sha256=wanted_hash, observed_spec_sha256=wanted_hash)
            save(state_path, state)
            return {**summary(observed), "operation": "update-accepted", "traffic_mode": "direct"}
        time.sleep(0.5)
    raise ValueError("FLASH_PENDING_UPDATE_NOT_YET_OBSERVED")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--image", required=True)
    parser.add_argument("--roster-key-file", required=True, type=Path)
    parser.add_argument("--cli", default=str(Path(os.environ.get("USERPROFILE", "")) / ".local/bin/heterocloud.exe"))
    parser.add_argument("--endpoint", default="https://heterocloud.mizuame.app")
    parser.add_argument("--organization-id", default="019fbda8-eff3-7610-bdd5-8fe89e746c14")
    args = parser.parse_args()
    key = args.roster_key_file.read_text(encoding="ascii").strip()
    result = update(HeteroCLI(args.cli, args.endpoint, args.organization_id), args.state, args.image, key, cloudflare_cidrs())
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        value = str(error)
        safe = value if isinstance(error, ValueError) and value.isascii() and value.replace("_", "").isalnum() else type(error).__name__
        raise SystemExit("MRFPS_FLASH_UPDATE_ERROR " + safe)
