"""Collision-safe Flash planning/reconciliation through the installed CLI.

No login, billing, DNS, registry credentials, existing unrelated service, or
visibility is changed. Current tenant port allocation cannot fulfill UDP7777.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

from server_bundle import flash_manifest


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".incoming")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


class HeteroCLI:
    def __init__(self, executable, endpoint, organization):
        if not endpoint.startswith("https://") or "/" in endpoint.removeprefix("https://").strip("/"):
            raise ValueError("HTTPS_ENDPOINT_ORIGIN_REQUIRED")
        uuid.UUID(organization)
        self.prefix = [executable, "--endpoint", endpoint, "--organization-id", organization, "--output", "json"]

    def call(self, *arguments, payload=None):
        result = subprocess.run(self.prefix + list(arguments), input=json.dumps(payload).encode() if payload is not None else None,
                                capture_output=True, timeout=60)
        if result.returncode:
            # CLI errors may include account information. Do not serialize them.
            raise ValueError("HETEROCLOUD_COMMAND_FAILED_AUTH_OR_OPERATION_REQUIRED")
        try:
            return json.loads(result.stdout)
        except ValueError:
            raise ValueError("HETEROCLOUD_JSON_RESPONSE_REQUIRED") from None


def owned_service(client, state, project, name):
    # HeteroCloud CLI v0.1.110 unwraps the API's {items: [...]} response and
    # prints a JSON array. An envelope could be an incomplete page and is not
    # the verified CLI format. The CLI also discards API pagination metadata,
    # so verify the live API's list contract before applying changes.
    items = client.call("flash", "list", "--project-id", project)
    if not isinstance(items, list):
        raise ValueError("FLASH_LIST_RESPONSE_SHAPE_UNVERIFIED")
    if any(not isinstance(item, dict) or not isinstance(item.get("name"), str) for item in items):
        raise ValueError("FLASH_SERVICE_NAME_RESPONSE_UNVERIFIED")
    matches = [item for item in items if item["name"] == name]
    if len(matches) > 1:
        raise ValueError("FLASH_SERVICE_NAME_AMBIGUOUS")
    if not matches:
        if state and state.get("service_id"):
            raise ValueError("OWNED_FLASH_SERVICE_DISAPPEARED")
        return None
    match = matches[0]
    marker = match.get("spec", {}).get("metadata", {})
    if (not state or marker.get("managed_by") != "MRFPS-cloud-deploy"
        or marker.get("deployment_id") != state.get("deployment_id")
        or marker.get("service_name") != name or match.get("project_id") != project
        or (state.get("service_id") and match.get("id") != state["service_id"])):
        raise ValueError("FLASH_NAME_COLLISION_OR_OWNERSHIP_MISMATCH")
    uuid.UUID(match["id"])
    return match


def summary(service):
    live = (service.get("status") or {}).get("status") or {}
    ports = [port.get("service_port") for port in service.get("spec", {}).get("ports", [])
             if port.get("protocol") == "udp" and port.get("container_port") == 7777]
    return {"service_id": service["id"], "state": service.get("state"), "generation": service.get("generation"),
            "observed_generation": live.get("observed_generation"), "live_status_unavailable": live.get("live_status_unavailable", False),
            "ready_replicas": live.get("ready_replicas"), "assigned_udp_ports": ports,
            "fixed_public_udp7777_satisfied": ports == [7777] and live.get("live_status_unavailable") is not True,
            "game_health_verified": False, "internet_udp_verified": False, "runtime_fingerprint_verified": False}


def reconcile(client, manifest, state_path, *, validation_allocated_port=False, operator_private_pull_ready=False):
    if not validation_allocated_port:
        raise ValueError("FLASH_FIXED_PUBLIC_UDP7777_UNSUPPORTED_BY_CURRENT_TENANT_API")
    if not operator_private_pull_ready:
        raise ValueError("OPERATOR_PRIVATE_GHCR_INSPECTION_AND_PULL_SECRET_REQUIRED")
    project, name = manifest["project_id"], manifest["name"]
    state = json.loads(state_path.read_text(encoding="utf-8-sig")) if state_path.exists() else None
    if state and (state.get("project_id") != project or state.get("service_name") != name):
        raise ValueError("FLASH_LOCAL_STATE_SCOPE_MISMATCH")
    service = owned_service(client, state, project, name)
    if state is None:
        state = {"project_id": project, "service_name": name, "deployment_id": str(uuid.uuid4()), "service_id": None}
    desired = copy.deepcopy(manifest)
    desired["spec"]["metadata"].update(deployment_id=state["deployment_id"], service_name=name)
    desired_hash = digest(desired["spec"])
    operation = "unchanged"
    if service:
        current_hash = digest(service["spec"])
        if state.get("observed_spec_sha256") and current_hash != state["observed_spec_sha256"]:
            raise ValueError("OWNED_FLASH_SPEC_DRIFT_REVIEW_REQUIRED")
        if state.get("desired_spec_sha256") != desired_hash:
            client.call("flash", "update", service["id"], "--file", "-", "--no-wait", payload=desired)
            operation = "update-accepted"
    else:
        # Keep an intent before the request: a timeout after acceptance can be
        # reconciled by its exact deployment UUID, never by adopting a name.
        state["desired_spec_sha256"] = desired_hash
        save(state_path, state)
        client.call("flash", "create", "--file", "-", "--no-wait", payload=desired)
        operation = "create-accepted"
    service = owned_service(client, state, project, name)
    if not service:
        return {"operation": operation, "status": "accepted_not_yet_visible", "fixed_public_udp7777_satisfied": False}
    state.update(service_id=service["id"], desired_spec_sha256=desired_hash, observed_spec_sha256=digest(service["spec"]))
    save(state_path, state)
    result = summary(service)
    result.update(operation=operation, validation_only=True, operator_private_pull_asserted=True, runtime_private_pull_verified=False)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("plan", "status", "apply"))
    parser.add_argument("--image")
    parser.add_argument("--public-key", type=Path)
    parser.add_argument("--project-id", required=True)
    parser.add_argument("--name", default="mrfps-server")
    parser.add_argument("--region", default="heteronet-global")
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--endpoint", default="https://heterocloud.mizuame.app")
    parser.add_argument("--organization-id", default="019fbda8-eff3-7610-bdd5-8fe89e746c14")
    parser.add_argument("--cli", default="heterocloud")
    parser.add_argument("--validation-allocated-port", action="store_true")
    parser.add_argument("--operator-private-pull-ready", action="store_true")
    args = parser.parse_args()
    uuid.UUID(args.project_id)
    if args.mode == "status":
        if not args.state.exists():
            print(json.dumps({"status": "no_owned_service_state", "deployed": False}))
            return
        state = json.loads(args.state.read_text(encoding="utf-8-sig"))
        service = owned_service(HeteroCLI(args.cli, args.endpoint, args.organization_id), state, args.project_id, args.name)
        print(json.dumps(summary(service) if service else {"status": "owned_intent_not_created", "deployed": False}))
        return
    if not args.image or not args.public_key:
        raise ValueError("IMMUTABLE_IMAGE_AND_PUBLIC_KEY_REQUIRED")
    manifest = flash_manifest(args.image, args.public_key.read_bytes(), args.project_id, args.name, args.region)
    if args.mode == "plan":
        print(json.dumps({"mode": "plan", "project_id": args.project_id, "name": args.name, "image": args.image,
                          "spec_sha256": digest(manifest["spec"]), "public_key_only": True, "cloud_verified": False,
                          "fixed_public_udp7777_supported": False, "mutations": False}))
        return
    result = reconcile(HeteroCLI(args.cli, args.endpoint, args.organization_id), manifest, args.state,
                       validation_allocated_port=args.validation_allocated_port, operator_private_pull_ready=args.operator_private_pull_ready)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        value = str(error)
        safe = value if isinstance(error, ValueError) and value.isascii() and value.replace("_", "").isalnum() else type(error).__name__
        raise SystemExit("MRFPS_FLASH_ERROR " + safe)
