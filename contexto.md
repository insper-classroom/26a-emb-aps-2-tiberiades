# Contexto do Projeto — APS-2

## O que é

Controle físico em formato de pistola para jogar **The House of the Dead Remake** (PC, Windows).  
O jogador segura a pistola e mira fisicamente — o IMU traduz o movimento em mira do mouse.  
É um on-rails shooter: não há WASD, só mira e disparo.

## Hardware

| Componente | Função | Pinos |
|---|---|---|
| Raspberry Pi Pico 2 (RP2350) | microcontrolador central | — |
| MPU-6050 | IMU — giroscópio + acelerômetro | GP2 (SDA), GP3 (SCL) — I2C1 |
| HC-06 | módulo Bluetooth Serial | GP4 (TX), GP5 (RX), GP6 (EN), GP15 (STATE) — UART1 |
| LED RGB | feedback de conexão BT | GP7 (R), GP8 (G), GP9 (B) — PWM |
| Botão R | gatilho — atirar | GP16 |
| Botão G | lanterna on/off | GP17 |
| Botão B | pausar | GP18 |

## Firmware (`main/main.c`)

FreeRTOS com 5 tasks:

| Task | O que faz |
|---|---|
| `task_mpu` | Lê MPU-6050 via I2C1 a cada 10 ms, posta `mpu_data_t` em `xQueueMPU` |
| `task_fusion` | Consome `xQueueMPU`, roda Fusion AHRS → Euler angles → move mouse (pitch=X, roll=Y). Detecta chacoalhar pela magnitude do acelerômetro |
| `task_btn` | ISR nos GPIOs dos botões + debounce 50 ms, envia evento via `bt_send()` |
| `task_bt_tx` | Consome `xQueueTX`, envia bytes via UART1 (HC-06) |
| `task_led` | PWM fade vermelho = desconectado, verde sólido = BT pareado |

## Flag de modo (`USE_BLUETOOTH`)

Definida em `main/main.c`:

```c
#define USE_BLUETOOTH  0   // 0 = cabo USB (teste), 1 = HC-06 (jogo)
```

- `0` → `bt_send()` usa `putchar_raw` → sai no UART0 (GP0/GP1) → adaptador USB-UART
- `1` → `bt_send()` enfileira em `xQueueTX` → sai pelo HC-06 via Bluetooth

## Protocolo serial (`0xFF | TYPE | val_lo | val_hi`)

| TYPE | Evento | Ação no PC |
|---|---|---|
| `0x00` | movimento X do mouse | `pyautogui.moveRel(val, 0)` |
| `0x01` | movimento Y do mouse | `pyautogui.moveRel(0, val)` |
| `0x10` | botão R (gatilho) | clique esquerdo |
| `0x11` | botão G (lanterna) | tecla `F` |
| `0x12` | botão B (pausar) | tecla `Esc` |
| `0x13` | gesto chacoalhar | tecla `R` (recarga) |

`val` é `int16_t` little-endian.

## Script Python (`pc/receiver.py`)

Abre a porta serial, lê pacotes de 4 bytes e converte para eventos de mouse/teclado via `pyautogui` e `pynput`.  
Funciona tanto no modo cabo (`/dev/ttyUSB0`) quanto no modo BT (`/dev/rfcomm0`).

```bash
pip install pyserial pyautogui pynput
cd pc/
python receiver.py
```

## Biblioteca Fusion (`Fusion/`)

Implementa o algoritmo AHRS (Attitude and Heading Reference System) de Seb Madgwick.  
Funde giroscópio + acelerômetro sem magnetômetro (`FusionAhrsUpdateNoMagnetometer`) e retorna quaternion → ângulos de Euler (pitch, roll, yaw).

## Estrutura de arquivos

```
/
├── main/
│   ├── main.c            ← firmware principal
│   ├── pins.h            ← mapeamento de todos os pinos
│   ├── hc06.h / hc06.c  ← driver AT do HC-06
│   └── mpu6050.h         ← defines de registradores do MPU-6050
├── Fusion/               ← biblioteca AHRS
├── FreeRTOS-Kernel/      ← submodule
├── pc/
│   └── receiver.py       ← script Python do PC
├── infra.md              ← guia de conexão física do hardware
├── contexto.md           ← este arquivo
└── README.md             ← descrição do projeto e controles
```
