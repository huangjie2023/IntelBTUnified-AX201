#!/bin/sh
# bt-diag.sh - resumen del estado del Bluetooth tras un arranque en frio fallido.
#
#   sh bt-diag.sh            -> informe por pantalla
#   sh bt-diag.sh > bt.txt   -> guarda el informe para pasarlo
#
# Pensado para IntelBTUnified (AX201). No modifica nada, solo lee.
set -u

echo "===== IntelBTUnified: diagnostico de arranque ====="
date
echo
echo "--- uptime / arranque ---"
uptime
sysctl -n kern.boottime 2>/dev/null

echo
echo "--- kext cargado ---"
kmutil showloaded 2>/dev/null | grep -i -E 'IntelBTUnified|Lilu' || echo "(no aparece IntelBTUnified en kmutil)"

echo
echo "--- parches de bluetoothd/BlueTool en este arranque ---"
# Cada parche aplicado imprime una linea "patched <fichero>: <nombre> (page ..., file offset ...)".
# Si falta "vendor check" o "chipset range check", bluetoothd se ha quedado sin parchear.
log show --last boot --style compact --predicate 'process == "kernel"' 2>/dev/null \
  | grep -E 'btlfx' | tail -40

echo
echo "--- firmware / driver ---"
log show --last boot --style compact --predicate 'process == "kernel"' 2>/dev/null \
  | grep -E 'IntelFirmware|Selected ops|Firmware' | tail -30

echo
echo "--- reescritura de eventos LE (h = connection handle) ---"
# "features complete (real, pass)" -> evento real entregado a bluetoothd
# "-> fake PHY Update" / "-> fake Data Length Change (done)" -> reescrituras
# "slot table full" o "event pipe mismatch" -> el fallo de conexion esta aqui
log show --last boot --style compact --predicate 'process == "kernel"' 2>/dev/null \
  | grep -E '\[PATCH\] le |event pipe (captured|mismatch)|le slot' | tail -40

echo
echo "--- crashes recientes de bluetoothd ---"
ls -1t "$HOME/Library/Logs/DiagnosticReports" 2>/dev/null | grep -i bluetoothd | head -5 || true

echo
echo "--- controlador ---"
system_profiler SPBluetoothDataType 2>/dev/null | sed -n '1,40p'

echo
echo "--- procesos ---"
pgrep -lf 'bluetoothd|BlueTool' || echo "(ninguno)"

echo
echo "===== fin ====="
