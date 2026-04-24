#!/usr/bin/env python3
"""Minimal Redfish mock server for rfbrowse.efi integration tests.

Serves static Redfish-like JSON at well-known paths. Validates session
authentication. The UEFI guest reaches this via QEMU gateway at 10.0.2.2.

Usage: python3 redfish-mock-server.py <port>
"""

from __future__ import annotations

import json
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

MOCK_TOKEN = "MockToken12345"
MOCK_USER = "admin"
MOCK_PASS = "password"

# --- Static Redfish JSON Responses ---

SERVICE_ROOT = {
    "@odata.id": "/redfish/v1/",
    "@odata.type": "#ServiceRoot.v1_5_0.ServiceRoot",
    "Id": "RootService",
    "Name": "Root Service",
    "RedfishVersion": "1.8.0",
    "Systems": {"@odata.id": "/redfish/v1/Systems"},
    "Chassis": {"@odata.id": "/redfish/v1/Chassis"},
    "Managers": {"@odata.id": "/redfish/v1/Managers"},
    "SessionService": {"@odata.id": "/redfish/v1/SessionService"},
}

SYSTEMS_COLLECTION = {
    "@odata.id": "/redfish/v1/Systems",
    "@odata.type": "#ComputerSystemCollection.ComputerSystemCollection",
    "Name": "Computer System Collection",
    "Members@odata.count": 1,
    "Members": [
        {"@odata.id": "/redfish/v1/Systems/1"},
    ],
}

SYSTEM_1 = {
    "@odata.id": "/redfish/v1/Systems/1",
    "@odata.type": "#ComputerSystem.v1_13_0.ComputerSystem",
    "Id": "1",
    "Name": "System",
    "Manufacturer": "AximCode",
    "Model": "QEMU Virtual Machine",
    "SerialNumber": "AXL-TEST-001",
    "PowerState": "On",
    "Status": {"State": "Enabled", "Health": "OK"},
    "ProcessorSummary": {"Count": 4, "Model": "QEMU Virtual CPU"},
    "MemorySummary": {"TotalSystemMemoryGiB": 8},
}

CHASSIS_COLLECTION = {
    "@odata.id": "/redfish/v1/Chassis",
    "@odata.type": "#ChassisCollection.ChassisCollection",
    "Name": "Chassis Collection",
    "Members@odata.count": 1,
    "Members": [
        {"@odata.id": "/redfish/v1/Chassis/1"},
    ],
}

THERMAL = {
    "@odata.id": "/redfish/v1/Chassis/1/Thermal",
    "@odata.type": "#Thermal.v1_7_0.Thermal",
    "Name": "Thermal",
    "Temperatures": [
        {
            "Name": "CPU Temp",
            "ReadingCelsius": 42,
            "Status": {"State": "Enabled", "Health": "OK"},
        },
        {
            "Name": "Inlet Temp",
            "ReadingCelsius": 25,
            "Status": {"State": "Enabled", "Health": "OK"},
        },
    ],
    "Fans": [
        {
            "Name": "Fan 1",
            "Reading": 5400,
            "ReadingUnits": "RPM",
            "Status": {"State": "Enabled", "Health": "OK"},
        },
    ],
}

MANAGERS_COLLECTION = {
    "@odata.id": "/redfish/v1/Managers",
    "@odata.type": "#ManagerCollection.ManagerCollection",
    "Name": "Manager Collection",
    "Members@odata.count": 1,
    "Members": [
        {"@odata.id": "/redfish/v1/Managers/1"},
    ],
}

MANAGER_1 = {
    "@odata.id": "/redfish/v1/Managers/1",
    "@odata.type": "#Manager.v1_11_0.Manager",
    "Id": "1",
    "Name": "BMC",
    "ManagerType": "BMC",
    "FirmwareVersion": "1.0.0-mock",
    "Status": {"State": "Enabled", "Health": "OK"},
}

# Public endpoints (no auth required)
PUBLIC_ROUTES: dict[str, object] = {
    "/redfish/v1/": SERVICE_ROOT,
    "/redfish/v1": SERVICE_ROOT,
}

# Authenticated endpoints
AUTH_ROUTES: dict[str, object] = {
    "/redfish/v1/Systems": SYSTEMS_COLLECTION,
    "/redfish/v1/Systems/1": SYSTEM_1,
    "/redfish/v1/Chassis": CHASSIS_COLLECTION,
    "/redfish/v1/Chassis/1/Thermal": THERMAL,
    "/redfish/v1/Managers": MANAGERS_COLLECTION,
    "/redfish/v1/Managers/1": MANAGER_1,
}

REDFISH_ERROR_401 = {
    "error": {
        "code": "Base.1.0.GeneralError",
        "message": "Authentication required",
    }
}

REDFISH_ERROR_404 = {
    "error": {
        "code": "Base.1.0.GeneralError",
        "message": "Resource not found",
    }
}


def make_error(code: int, message: str) -> dict[str, object]:
    return {
        "error": {
            "code": "Base.1.0.GeneralError",
            "message": message,
        }
    }


class RedfishHandler(BaseHTTPRequestHandler):
    def send_json(self, code: int, data: object) -> None:
        body = json.dumps(data, indent=2).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("OData-Version", "4.0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def check_auth(self) -> bool:
        token = self.headers.get("X-Auth-Token")
        return token == MOCK_TOKEN

    def do_GET(self) -> None:
        # Public routes (no auth)
        if self.path in PUBLIC_ROUTES:
            self.send_json(200, PUBLIC_ROUTES[self.path])
            return

        # Authenticated routes
        if self.path in AUTH_ROUTES:
            if not self.check_auth():
                self.send_json(401, REDFISH_ERROR_401)
                return
            self.send_json(200, AUTH_ROUTES[self.path])
            return

        # Not found
        self.send_json(404, REDFISH_ERROR_404)

    def do_POST(self) -> None:
        if self.path == "/redfish/v1/SessionService/Sessions":
            length = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(length) if length > 0 else b""

            try:
                creds = json.loads(raw)
            except (json.JSONDecodeError, ValueError):
                self.send_json(400, make_error(400, "Invalid JSON"))
                return

            user = creds.get("UserName", "")
            password = creds.get("Password", "")

            if user != MOCK_USER or password != MOCK_PASS:
                self.send_json(401, make_error(401, "Invalid credentials"))
                return

            session = {
                "@odata.id": "/redfish/v1/SessionService/Sessions/1",
                "Id": "1",
                "Name": "User Session",
                "UserName": user,
            }
            body = json.dumps(session, indent=2).encode()
            self.send_response(201)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Auth-Token", MOCK_TOKEN)
            self.send_header("Location", "/redfish/v1/SessionService/Sessions/1")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_json(404, REDFISH_ERROR_404)

    def do_DELETE(self) -> None:
        if self.path == "/redfish/v1/SessionService/Sessions/1":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            return

        self.send_json(404, REDFISH_ERROR_404)

    def log_message(self, fmt: str, *args: object) -> None:
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18082
    server = HTTPServer(("0.0.0.0", port), RedfishHandler)
    print(f"Redfish mock server on port {port}", flush=True)
    server.serve_forever()
