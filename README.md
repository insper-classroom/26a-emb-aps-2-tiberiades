# 🔫 APS-2 — Controle Temático: Zumbi Blocks Ultimate

## Jogo

**Zumbi Blocks Ultimate 2.5.0** (PC, Windows)  
FPS/TPS de sobrevivência zumbi. O jogador explora um mapa aberto, coleta armas, enfrenta hordas e bosses de zumbis. Controles padrão via teclado e mouse.

---

## Ideia do Controle

Formato de **pistola/rifle compacto** segurado com duas mãos.

- **Joystick esquerdo** no corpo da arma → movimentação do personagem (WASD)
- **IMU** embutido no cano → inclinar/girar a arma controla a mira (substitui o mouse)
- **Botões** embaixo dos dedos → ações do jogo
- **Gesto de chacoalhar** a arma → IA classifica o movimento do IMU → envia tecla de recarga (R)

```
         ___________
        |           |
[BTN A]-|  [IMU]    |
[BTN B]-|           |--[JOYSTICK ESQ]
[BTN C]-|___________|
[BTN D]
   |
[gatilho / BTN A]
```

O gesto de **recarga** é o diferencial: chacoalhar fisicamente a arma → acelerômetro detecta pico → modelo de IA classifica → envia `R` ao jogo. Gesto intuitivo e temático.

---

## Inputs e Outputs

| Tipo | Componente | Função no Jogo |
|---|---|---|
| Input | Joystick (ADC) | Movimentação WASD |
| Input | IMU — MPU-6050 (I2C) | Controle de mira (mouse X/Y) |
| Input | IMU — acelerômetro | Gesto de recarga (chacoalhar → tecla R) |
| Input | Botão A (gatilho) | Atirar (LMB) |
| Input | Botão B | Mirar (RMB) |
| Input | Botão C | Pular (SPACE) |
| Input | Botão D | Interagir / Chutar (E / F) |
| Output | LED | Status de conexão Bluetooth |
| Output | Buzzer | Feedback ao tomar dano |
| Output | HC-06 (UART→BT) | Envio de comandos ao PC |

---

## Protocolo

**Bluetooth Serial (HC-06)** via UART.  
O firmware envia pacotes ao PC. Um script Python recebe os dados, roda o modelo de IA e converte para eventos de teclado/mouse via `pyserial` + `pynput`.

**Formato do pacote serial:**
```
[0xAA][TYPE 1B][VALUE 2B][0xFF]
```

| TYPE | Conteúdo |
|---|---|
| `0x01` | Joystick X/Y (movimento) |
| `0x02` | IMU ângulo X/Y (mira) |
| `0x03` | IMU aceleração XYZ (gesto) |
| `0x04` | Estado dos botões (bitmask) |

---

## IA — Classificação de Gesto de Recarga

O gesto de **chacoalhar** a arma é detectado por um modelo treinado com dados do acelerômetro (MPU-6050).

---

## Diagrama de Blocos do Firmware

![Diagrama de Blocos do Firmware](./bloco.jpeg)
```

| Elemento | Descrição |
|---|---|
| **Task ADC** | Lê joystick via ADC, posta dados na fila de envio |
| **Task IMU** | Lê MPU-6050 via I2C (ângulo + aceleração), sinaliza semáforo |
| **Task BT** | Consome fila de botões + dados ADC/IMU, monta e envia pacote UART |
| **Task Output** | Controla LED (conexão) e buzzer (dano) |
| **ISR GPIO** | Detecta borda de descida dos botões, envia para `btn_queue` |
| **btn_queue** | Fila FreeRTOS entre ISR e Task BT |
| **Semáforo IMU** | Sincroniza Task IMU com Task BT |
```

---

## Estrutura do Repositório

```
/
├── firmware/
│   ├── main.c
│   ├── task_adc.c
│   ├── task_imu.c
│   ├── task_bt.c
│   ├── task_output.c
│   └── isr_gpio.c
├── pc/
│   ├── receiver.py          # recebe pacotes BT e envia inputs ao jogo
│   ├── train_gesture.py     # coleta amostras e treina modelo de gesto
│   └── model.pkl            # modelo treinado (gerado pelo script)
└── README.md
```