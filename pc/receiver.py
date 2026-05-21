"""
Receptor Bluetooth — The House of the Dead Remake
===================================================
Protocolo Pico → PC (4 bytes cada):
    0xFF | TYPE | val_lo | val_hi   (val = int16_t little-endian)

    TYPE 0x00 : move mouse X
    TYPE 0x01 : move mouse Y
    TYPE 0x10 : BTN_R → clique esquerdo (atirar)
    TYPE 0x11 : BTN_G → tecla R (recarregar)
    TYPE 0x12 : BTN_B → tecla Escape (pausar)
    TYPE 0x13 : gesto chacoalhar → tecla R (recarregar)

Dependências:
    pip install pyserial pyautogui pynput
"""

import struct
import serial
import serial.tools.list_ports
import pyautogui
from pynput.keyboard import Controller as KeyboardController, Key

pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0

keyboard = KeyboardController()


def selecionar_porta():
    portas = serial.tools.list_ports.comports()
    print("\nPortas disponíveis:")
    for i, p in enumerate(portas):
        print(f"  [{i}] {p.device} — {p.description}")
    print("\nDigite o número da porta ou o caminho (ex: /dev/rfcomm0):")
    entrada = input("> ").strip()
    if entrada.startswith("/") or entrada.upper().startswith("COM"):
        return entrada
    try:
        idx = int(entrada)
        if 0 <= idx < len(portas):
            return portas[idx].device
    except ValueError:
        pass
    return entrada


def handle_packet(type_byte, value):
    if type_byte == 0x00:
        pyautogui.moveRel(value, 0)

    elif type_byte == 0x01:
        pyautogui.moveRel(0, value)

    elif type_byte == 0x10:
        pyautogui.click()

    elif type_byte == 0x11:
        keyboard.press('r')
        keyboard.release('r')
        print("  → recarga (BTN_G)")

    elif type_byte == 0x12:
        keyboard.press(Key.esc)
        keyboard.release(Key.esc)
        print("  → pausa (BTN_B)")

    elif type_byte == 0x13:
        keyboard.press('r')
        keyboard.release('r')
        print("  → recarga (gesto chacoalhar)")


def main():
    porta = selecionar_porta()
    print(f"\nConectando em {porta} @ 115200 baud...")

    with serial.Serial(porta, 115200, timeout=1) as ser:
        print("Conectado! Controle o jogo com a pistola BT.\n")

        buf = bytearray()
        while True:
            data = ser.read(64)
            if not data:
                continue
            buf.extend(data)

            while len(buf) >= 4:
                if buf[0] != 0xFF:
                    buf.pop(0)
                    continue

                pkt = buf[:4]
                buf = buf[4:]

                type_byte = pkt[1]
                value = struct.unpack('<h', bytes([pkt[2], pkt[3]]))[0]
                handle_packet(type_byte, value)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nEncerrando.")
