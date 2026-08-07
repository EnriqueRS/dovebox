#!/usr/bin/env python3
"""Leer la consola serie del ESP32 (DoveBox) durante N segundos.

Uso local (con el ESP32 enchufado):        python3 esp32_serial.py /dev/ttyACM0 115200 15
Uso remoto (ESP32 en el portátil Ubuntu):  ssh ubuntu-laptop 'python3 - <<EOF ... EOF'  (o copiar este script y ejecutarlo)

Detalle: abrir el puerto del ESP32-S3 (USB-JTAG) con DTR alto REINICIA el chip.
Este script abre con DTR/RTS bajos para NO reiniciar tras la apertura inicial;
el propio open() ya provoca un reset = boot log fresco en cada captura (útil).
"""
import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
dur = float(sys.argv[3]) if len(sys.argv) > 3 else 15

ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.timeout = 0.5
ser.dsrdtr = False
ser.rtscts = False
ser.open()
ser.dtr = False
ser.rts = False

print(f"[{port} @ {baud} baud - capturando {dur}s...]", flush=True)
deadline = time.time() + dur
got = 0
while time.time() < deadline:
    n = ser.in_waiting
    if n:
        data = ser.read(n)
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
        got += n
    else:
        time.sleep(0.1)
ser.close()
print(f"\n[bytes capturados: {got}]", flush=True)
