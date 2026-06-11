"""
Receptor Serial — The House of the Dead Remake
===============================================
Funciona tanto via cabo USB (USE_BLUETOOTH=0 no firmware)
quanto via Bluetooth HC-06 (USE_BLUETOOTH=1).

Protocolo Pico → PC (4 bytes cada):
    0xFF | TYPE | val_lo | val_hi   (val = int16_t little-endian)

    TYPE 0x00 : movimento do mouse — eixo X
    TYPE 0x01 : movimento do mouse — eixo Y
    TYPE 0x02 : BTN_G (calibrar)  → centraliza o cursor na tela
    TYPE 0x10 : BTN_R (gatilho)  → clique esquerdo
    TYPE 0x11 : (OBSOLETO — não enviado pelo firmware)
    TYPE 0x12 : BTN_B (pausar)   → tecla Escape
    TYPE 0x13 : gesto chacoalhar → tecla R (recarga via IA)

Dependências:
    pip install pyserial pyautogui pynput
"""
r
import struct
import serial
import serial.tools.list_ports
import pyautogui
from pynput.keyboard import Controller as KeyboardController, Key

pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0

keyboard = KeyboardController()

# Suavização — média móvel exponencial (0 < ALPHA < 1)
# Menor = mais suave, mais lento. Maior = mais responsivo, menos suave.
ALPHA = 0.3
smooth_x = 0.0
smooth_y = 0.0


def selecionar_porta():
    portas = serial.tools.list_ports.comports()

    print("\nPortas disponíveis:")
    for i, p in enumerate(portas):
        print(f"  [{i}] {p.device} — {p.description}")

    print("\nDigite o número da porta ou o caminho direto (ex: /dev/rfcomm0, /dev/ttyACM0):")
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
    global smooth_x, smooth_y

    if type_byte == 0x00:
        smooth_x = ALPHA * value + (1 - ALPHA) * smooth_x
        if abs(smooth_x) >= 0.5:
            pyautogui.moveRel(int(smooth_x), 0)

    elif type_byte == 0x01:
        smooth_y = ALPHA * value + (1 - ALPHA) * smooth_y
        if abs(smooth_y) >= 0.5:
            pyautogui.moveRel(0, int(smooth_y))

    elif type_byte == 0x02:
        largura, altura = pyautogui.size()
        pyautogui.moveTo(largura // 2, altura // 2)
        smooth_x = 0.0
        smooth_y = 0.0   # zera a suavização para não "arrastar" após recentralizar
        print("  → calibrar / centralizar mouse (BTN_G)")

    elif type_byte == 0x10:
        pyautogui.click()
        print("  → atirar (BTN_R)")

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
        print("Conectado! Movimente a pistola para controlar a mira.\n")

        # Posiciona o cursor no centro da tela
        largura, altura = pyautogui.size()
        pyautogui.moveTo(largura // 2, altura // 2)
        print(f"  cursor posicionado no centro ({largura // 2}, {altura // 2})")

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
