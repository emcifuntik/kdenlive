#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Minimal Kdenlive Streamable HTTP MCP client, using only Python's standard library.

List tools, inspect JSON schemas, call tools, or save a returned PNG. This client
does not start Kdenlive. It connects to the local endpoint automatically started
by the editor. No authentication is required.
"""

import argparse
import base64
import http.client
import json
from pathlib import Path
import sys
from urllib.parse import urlsplit


class McpClient:
    def __init__(self, url, timeout=120):
        self.url = urlsplit(url)
        if (self.url.scheme != "http" or self.url.hostname not in ("127.0.0.1", "localhost")
                or self.url.path != "/mcp" or self.url.query or self.url.fragment
                or self.url.username or self.url.password or not self.url.port):
            raise ValueError("Use http://127.0.0.1:PORT/mcp or http://localhost:PORT/mcp")
        self.timeout = timeout
        self.session = None
        self.version = None
        self.next_id = 0

    def _exchange(self, body=None, method="POST"):
        headers = {"Accept": "application/json, text/event-stream",
                   "Content-Type": "application/json"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        if self.version:
            headers["Mcp-Protocol-Version"] = self.version
        connection = http.client.HTTPConnection(self.url.hostname, self.url.port, timeout=self.timeout)
        try:
            payload = json.dumps(body).encode("utf-8") if body is not None else b""
            connection.request(method, self.url.path, body=payload, headers=headers)
            response = connection.getresponse()
            data = response.read()
            if not 200 <= response.status < 300:
                raise RuntimeError(f"MCP HTTP {response.status}: {data.decode('utf-8', errors='replace')}")
            new_session = response.getheader("Mcp-Session-Id")
            if new_session:
                self.session = new_session
            return json.loads(data) if data else None
        finally:
            connection.close()

    def request(self, method, params=None):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        response = self._exchange(message)
        if not response or response.get("jsonrpc") != "2.0" or response.get("id") != self.next_id:
            raise RuntimeError("Invalid JSON-RPC response")
        if "error" in response:
            raise RuntimeError(json.dumps(response["error"]))
        return response["result"]

    def __enter__(self):
        result = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                             "clientInfo": {"name": "kdenlive-example", "version": "1"}})
        self.version = result["protocolVersion"]
        self._exchange({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return self

    def __exit__(self, exc_type, exc, traceback):
        if self.session:
            try:
                self._exchange(method="DELETE")
            except (OSError, RuntimeError):
                pass

    def call(self, name, arguments=None):
        return self.request("tools/call", {"name": name, "arguments": arguments or {}})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:9250/mcp")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="Print every tool and its JSON schema")
    call = sub.add_parser("call", help="Call a tool; prints its MCP result")
    call.add_argument("name")
    arguments = call.add_mutually_exclusive_group()
    arguments.add_argument("--arguments", default="{}", help="JSON object")
    arguments.add_argument("--arguments-file", type=Path, help="UTF-8 JSON object file")
    call.add_argument("--image", type=Path, help="Save the first returned image to this new PNG file")
    args = parser.parse_args()
    with McpClient(args.url) as client:
        if args.command == "list":
            result = client.request("tools/list")
        else:
            raw = args.arguments_file.read_text(encoding="utf-8-sig") if args.arguments_file else args.arguments
            arguments = json.loads(raw)
            if not isinstance(arguments, dict):
                raise ValueError("Tool arguments must be a JSON object")
            result = client.call(args.name, arguments)
            if args.image and not result.get("isError"):
                images = [entry for entry in result.get("content", []) if entry.get("type") == "image" and entry.get("mimeType") == "image/png"]
                if not images:
                    raise RuntimeError("The tool returned no PNG image")
                with args.image.open("xb") as output:
                    output.write(base64.b64decode(images[0]["data"], validate=True))
                # Avoid printing the entire base64 payload after saving the image.
                result = {"image": str(args.image.resolve()), "isError": False}
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 1 if result.get("isError") else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
