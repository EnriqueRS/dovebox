#!/usr/bin/env python3
"""
DoveBox Hermes MCP Bridge — expone tools de Hermes, Home Assistant y el agregador
al LLM del xiaozhi-server vía el protocolo MCP (transporte SSE / streamable-http).

Se ejecuta EN EL HOST (donde viven el CLI de Hermes y el token de HA) y el
xiaozhi-server (contenedor) se conecta por URL. Registrado en:
    /docker/xiaozhi-server/data/.mcp_server_settings.json
    {"mcpServers": {"dovebox-hermes": {"url": "http://192.168.100.73:18110/mcp", "transport": "sse"}}}

Run:
    /home/kike/.hermes/hermes-agent/venv/bin/python hermes_mcp_bridge.py

Requiere el paquete `mcp` (instalado en el venv de Hermes).
"""

import asyncio
import json
import os
import subprocess
import sys
import urllib.request
import urllib.error
from typing import Any

from mcp.server.fastmcp import FastMCP

AGGREGATOR_URL = os.environ.get("DOVEBOX_AGGREGATOR_URL", "http://192.168.100.73:18100")
HERMES_CLI = os.environ.get("HERMES_CLI", "hermes")
HA_TOKEN = os.environ.get("HASS_TOKEN", "")
HA_URL = os.environ.get("HA_URL", "http://localhost:8123")
HERMES_HOME = os.environ.get("HERMES_HOME", os.path.expanduser("~/.hermes"))

port = int(os.environ.get("DOVEBOX_MCP_PORT", "18110"))
mcp = FastMCP("dovebox-hermes", host="0.0.0.0", port=port)


# ── Home Assistant ──────────────────────────────────────────────────────────


def _ha_headers() -> dict:
    headers = {"Content-Type": "application/json"}
    if HA_TOKEN:
        headers["Authorization"] = f"Bearer {HA_TOKEN}"
    return headers


@mcp.tool()
async def ha_get_state(entity_id: str) -> str:
    """Get the current state of a Home Assistant entity (e.g. light.salon, sensor.temperatura_salon)."""
    try:
        req = urllib.request.Request(f"{HA_URL}/api/states/{entity_id}", headers=_ha_headers())
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
        return json.dumps({
            "entity_id": data.get("entity_id"),
            "state": data.get("state"),
            "attributes": data.get("attributes", {}),
            "last_changed": data.get("last_changed"),
        }, ensure_ascii=False)
    except urllib.error.HTTPError as e:
        return json.dumps({"error": f"HTTP {e.code}: {e.reason}"})
    except Exception as e:
        return json.dumps({"error": str(e)})


@mcp.tool()
async def ha_call_service(domain: str, service: str, entity_id: str, data: dict = None) -> str:
    """Call a Home Assistant service to control a device.
    domain: 'light'|'switch'|'climate'|'cover'|'media_player'|... service: 'turn_on'|'turn_off'|'toggle'|...
    data: optional JSON dict, e.g. {'brightness': 255} or {'temperature': 22}."""
    try:
        body = {"entity_id": entity_id}
        if data:
            body.update(json.loads(data) if isinstance(data, str) else data)
        req = urllib.request.Request(
            f"{HA_URL}/api/services/{domain}/{service}",
            data=json.dumps(body).encode(),
            headers=_ha_headers(),
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            result = json.loads(resp.read())
        return json.dumps({"success": True, "result": result}, ensure_ascii=False)
    except urllib.error.HTTPError as e:
        return json.dumps({"error": f"HTTP {e.code}: {e.reason}"})
    except Exception as e:
        return json.dumps({"error": str(e)})


# ── Hermes agent ────────────────────────────────────────────────────────────


@mcp.tool()
async def hermes_query(prompt: str) -> str:
    """Ask the Hermes AI agent a question and get a response. Use for general knowledge,
    web searches, data analysis, or any task that needs an AI agent. Returns the text response."""
    try:
        proc = await asyncio.create_subprocess_exec(
            HERMES_CLI, "chat", "-q", prompt, "--quiet",
            env={**os.environ, "HERMES_HOME": HERMES_HOME},
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=180)
        if proc.returncode != 0:
            return json.dumps({"error": f"Hermes exited {proc.returncode}: {stderr.decode()[:500]}"})
        return stdout.decode().strip()
    except asyncio.TimeoutError:
        return json.dumps({"error": "Hermes query timed out (180s)"})
    except FileNotFoundError:
        return json.dumps({"error": f"hermes CLI not found at '{HERMES_CLI}'"})
    except Exception as e:
        return json.dumps({"error": str(e)})


@mcp.tool()
async def hermes_create_cron(schedule: str, prompt: str) -> str:
    """Create a scheduled cron job for Hermes to run periodically.
    schedule: '30m' (every 30 min), 'every 2h', '0 9 * * *' (cron), or ISO timestamp.
    prompt: what the job should do each tick (self-contained)."""
    try:
        proc = await asyncio.create_subprocess_exec(
            HERMES_CLI, "cron", "create", schedule,
            env={**os.environ, "HERMES_HOME": HERMES_HOME},
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await asyncio.wait_for(
            proc.communicate(input=prompt.encode()), timeout=30
        )
        if proc.returncode != 0:
            return json.dumps({"error": f"cron create failed: {stderr.decode()[:500]}"})
        return stdout.decode().strip()
    except asyncio.TimeoutError:
        return json.dumps({"error": "cron create timed out"})
    except Exception as e:
        return json.dumps({"error": str(e)})


# ── Aggregator (DoveBox panel) ──────────────────────────────────────────────


async def _aggregator_get(path: str) -> str:
    try:
        req = urllib.request.Request(f"{AGGREGATOR_URL}{path}")
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.read().decode()
    except Exception as e:
        return json.dumps({"error": str(e)})


async def _aggregator_post(path: str, data: dict) -> str:
    try:
        req = urllib.request.Request(
            f"{AGGREGATOR_URL}{path}",
            data=json.dumps(data).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.read().decode()
    except Exception as e:
        return json.dumps({"error": str(e)})


@mcp.tool()
async def aggregator_dashboard() -> str:
    """Get the current DoveBox dashboard data: rooms (temperature/humidity), news headlines, and todo items, as JSON."""
    return await _aggregator_get("/api/dashboard")


@mcp.tool()
async def aggregator_todo_add(text: str) -> str:
    """Add a new item to the DoveBox todo list."""
    return await _aggregator_post("/api/todos", {"text": text})


@mcp.tool()
async def aggregator_news() -> str:
    """Get the latest news headlines from the aggregator (Marca, Mundo Deportivo, El Español, Xataka, Google News, BeSoccer), as JSON."""
    return await _aggregator_get("/api/news")


if __name__ == "__main__":
    print(f"DoveBox Hermes MCP Bridge listening on :{port}/mcp (streamable-http)", file=sys.stderr, flush=True)
    mcp.run(transport="streamable-http")