#!/usr/bin/env python3
"""Opt-in BookOrbit transport for the owned X3 simulator adapter.

Reads one JSON request from stdin; writes one JSON response to stdout. Configure
CROSSFADE_SIM_BOOKORBIT_CONFIG with origin, document, provision_zip, log, and
optionally write_guard (a saved GET response body) and device_id. No credentials
are printed. The supported BookOrbit plugin package supplies x-auth credentials;
firmware progress decisions, mapping and JSON serialization are not replaced.
"""
import json
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.request
import zipfile


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def main():
    config = json.loads(Path(os.environ["CROSSFADE_SIM_BOOKORBIT_CONFIG"]).read_text())
    request = json.loads(sys.stdin.read(1024 * 1024))
    origin = config["origin"].rstrip("/")
    if not origin.startswith("https://"):
        raise ValueError("HTTPS required")
    base = origin + "/api/v1/koreader"
    document = config["document"]
    if not re.fullmatch(r"[a-f0-9]{32}", document):
        raise ValueError("Invalid document scope")
    progress_url = base + "/syncs/progress/" + document
    method, url = request["method"], request["url"]
    reading = method == "GET" and url in (progress_url, base + "/plugin/version", base + "/users/auth")
    writing = method == "PUT" and url == base + "/syncs/progress"
    if not (reading or writing):
        raise ValueError("Outside allowed method/path scope")
    with zipfile.ZipFile(config["provision_zip"]) as archive:
        provision = archive.read("bookorbit.koplugin/bookorbit_provision.lua").decode()

    def field(name):
        match = re.search(r'\b' + name + r'\s*=\s*("(?:[^"\\]|\\.)*")', provision)
        if not match:
            raise ValueError("Missing provision field")
        return json.loads(match.group(1))

    if field("server_url").rstrip("/") != origin:
        raise ValueError("Provision origin does not match configured origin")
    headers = {k: v for k, v in request.get("headers", {}).items()
               if k.lower() in ("accept", "content-type")}
    headers.update({"x-auth-user": field("username"), "x-auth-key": field("userkey")})
    opener = urllib.request.build_opener(NoRedirect())

    def send(verb, target, body=None):
        req = urllib.request.Request(target, data=body, headers=headers, method=verb)
        try:
            response = opener.open(req, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read(1024 * 1024 + 1)
            if len(data) > 1024 * 1024:
                raise ValueError("Response too large")
            return {"status": response.code, "body": data.decode(),
                    "headers": {k: v for k, v in response.headers.items()
                                if k.lower() in ("content-type", "content-length", "date")}}

    before = None
    if writing:
        body = json.loads(request["body"])
        if body.get("document") != document or body.get("device_id") != config.get("device_id"):
            raise ValueError("Outside allowed document/device scope")
        guard = Path(config["write_guard"])
        before = send("GET", progress_url)
        if before["status"] != 200 or json.loads(before["body"]) != json.loads(guard.read_text()):
            raise ValueError("Concurrent progress change: refusing PUT")
    response = send(method, url, request.get("body", "").encode() if writing else None)
    record = {"case": config.get("case"), "observed_at": time.time(),
              "method": method, "url": url, "status": response["status"],
              "request_body": json.loads(request["body"]) if writing else None,
              "response_body": json.loads(response["body"])}
    if writing and 200 <= response["status"] < 300:
        after = send("GET", progress_url)
        record.update(before=json.loads(before["body"]), readback_status=after["status"],
                      readback=json.loads(after["body"]))
        # A concurrent writer after our PUT must not become our new ownership guard.
        observed = record["readback"]
        if after["status"] == 200 and all(observed.get(k) == body.get(k)
                                          for k in ("document", "progress", "timestamp", "device_id")):
            guard.write_text(json.dumps(observed))
    with Path(config["log"]).open("a") as log:
        log.write(json.dumps(record) + "\n")
    print(json.dumps(response))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Never include exception text: HTTP library errors may contain auth material.
        print(json.dumps({"status": -1, "body": "", "headers": {}, "error": type(error).__name__}))
