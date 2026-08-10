#!/bin/bash
# DoveBox Hermes MCP Bridge — systemd user service wrapper
# Instalar:
#   mkdir -p ~/.config/systemd/user
#   cp scripts/hermes-mcp-bridge.service ~/.config/systemd/user/
#   systemctl --user daemon-reload
#   systemctl --user enable --now hermes-mcp-bridge

BRIDGE_DIR="$(dirname "$(readlink -f "$0")")"
exec /home/kike/.hermes/hermes-agent/venv/bin/python \
    /docker/dovebox/scripts/hermes_mcp_bridge.py