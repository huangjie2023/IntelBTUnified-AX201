#!/bin/sh
# bt-diag.sh - resumen del estado del Bluetooth tras un arranque en frio.
#
#   sudo sh bt-diag.sh              -> informe por pantalla
#   sudo sh bt-diag.sh > ~/bt.txt   -> guarda el informe para pasarlo
#
# Necesita sudo: en macOS dmesg solo es legible por root.
# No modifica nada, solo lee.
set -u

echo "===== IntelBTUnified: diagnostico ====="
date
uptime

echo
echo "--- kext cargado ---"
kmutil showloaded 2>/dev/null | grep -i -E 'IntelBTUnified|Lilu' || echo "(no aparece en kmutil)"

echo
echo "--- dmesg: tamano y ultimas lineas ---"
DMESG=$(sudo -n dmesg 2>/dev/null || dmesg 2>/dev/null || true)
if [ -z "${DMESG}" ]; then
	echo "(dmesg vacio o sin permisos: ejecuta este script con sudo)"
else
	printf 'lineas: %s\n' "$(printf '%s\n' "${DMESG}" | wc -l | tr -d ' ')"
	printf '%s\n' "${DMESG}" | tail -15
fi

echo
echo "--- firmware / driver ---"
printf '%s\n' "${DMESG}" | grep -E 'IntelFirmware|Selected ops|Firmware|Intel boot' | tail -30

echo
echo "--- parches bluetoothd/BlueTool (btlfx) ---"
# Cada parche aplicado imprime: "btlfx: patched <fichero>: <nombre> (page .., file offset ..)"
# Si falta "vendor check" o "chipset range check", bluetoothd se quedo sin parchear.
printf '%s\n' "${DMESG}" | grep -F 'btlfx' | tail -40

echo
echo "--- reescritura de eventos LE (h = connection handle) ---"
# Esperado por conexion, mismo handle:
#   [PATCH] le h=0xNNN features complete (real, pass)
#   [PATCH] le h=0xNNN -> fake PHY Update
#   [PATCH] le h=0xNNN -> fake Data Length Change (done)
# Anomalias: "slot table full", "event pipe mismatch", "features event without slot".
printf '%s\n' "${DMESG}" | grep -F -e 'PATCH' -e 'event pipe' -e 'le slot' | tail -40

echo
echo "--- busqueda alternativa en el log unificado ---"
log show --last boot --style compact 2>/dev/null | grep -F -e 'IntelBTUnified' -e 'IntelFirmware' | tail -15

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
