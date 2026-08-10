FROM python:3.11-slim

RUN pip install --no-cache-dir "mcp==1.26.0" uvicorn>=0.27.0

COPY hermes_mcp_bridge.py /app/bridge.py

EXPOSE 18110

ENTRYPOINT ["python", "/app/bridge.py"]