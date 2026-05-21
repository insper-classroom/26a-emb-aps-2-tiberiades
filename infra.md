# Infraestrutura — Conexões de Hardware

## Visão Geral

```
                    ┌─────────────┐
      MPU-6050 ────►│             │
      HC-06    ◄───►│  Pico 2     │◄──── Botões R / G / B
      LED RGB  ◄────│  (RP2350)   │
                    └─────────────┘
                           │ Bluetooth
                           ▼
                        PC / Notebook
                        (receiver.py)
                           │
                           ▼
                    The House of the Dead
```

---

## 1. MPU-6050 → Pico 2 (I2C1)

| MPU-6050 | Pico 2 (GPIO) | Descrição |
|----------|---------------|-----------|
| VCC      | 3V3           | alimentação |
| GND      | GND           | terra |
| SDA      | **GP2**       | dados I2C1 |
| SCL      | **GP3**       | clock I2C1 |
| AD0      | GND           | endereço I2C = 0x68 |
| INT      | não conectar  | não usado |

> Coloque resistores de pull-up de **4.7 kΩ** entre SDA→3V3 e SCL→3V3 caso o módulo não os tenha embutidos.

---

## 2. HC-06 → Pico 2 (UART1)

| HC-06   | Pico 2 (GPIO) | Descrição |
|---------|---------------|-----------|
| VCC     | 5V (VBUS)     | alimentação — o HC-06 opera em 5 V |
| GND     | GND           | terra |
| TXD     | **GP5**       | RX do Pico (UART1 RX) |
| RXD     | **GP4**       | TX do Pico (UART1 TX) |
| EN/KEY  | **GP6**       | modo AT (alto = AT, baixo = dados) |
| STATE   | **GP15**      | estado da conexão BT (alto = pareado) |

> **Divisor de tensão obrigatório** no pino RXD do HC-06: o Pico transmite em 3.3 V, mas o HC-06 espera 3.3 V — em alguns módulos o RXD já tolera 3.3 V; se não, use um divisor 2kΩ / 3.3kΩ.

---

## 3. LED RGB → Pico 2 (PWM)

Use um LED RGB de **cátodo comum** (ou três LEDs separados) com resistores de 220 Ω em cada pino de cor.

| LED     | Pico 2 (GPIO) | Resistor |
|---------|---------------|----------|
| R (vermelho) | **GP7** | 220 Ω |
| G (verde)    | **GP8** | 220 Ω |
| B (azul)     | **GP9** | 220 Ω |
| Cátodo (–)   | GND     | —       |

**Comportamento:**
- **Verde sólido** → HC-06 pareado (jogo pronto)
- **Vermelho pulsando** → aguardando conexão Bluetooth

---

## 4. Botões → Pico 2

Use botões de pressão momentâneos (push button). O firmware usa `gpio_pull_up` interno, então conecte simplesmente entre o GPIO e o GND.

| Botão  | Pico 2 (GPIO) | Função no jogo |
|--------|---------------|----------------|
| BTN R  | **GP16**      | Atirar (clique esquerdo) |
| BTN G  | **GP17**      | Lanterna (tecla F) |
| BTN B  | **GP18**      | Pausar (Esc) |

Cada botão: um terminal em GPxx, o outro em **GND**. Sem resistor externo necessário.

---

## 5. Diagrama de pinos do Pico 2

```
                    ┌─────────────────┐
               GP0  │ 1           40  │ VBUS ──── HC-06 VCC (5V)
               GP1  │ 2           39  │ VSYS
               GND  │ 3           38  │ GND
  MPU SDA ─── GP2  │ 4           37  │ 3V3_EN
  MPU SCL ─── GP3  │ 5           36  │ 3V3 ────── MPU VCC
 HC06 TX  ─── GP4  │ 6           35  │ ADC_VREF
 HC06 RX  ─── GP5  │ 7           34  │ GP28
 HC06 EN  ─── GP6  │ 8           33  │ GND
  LED R   ─── GP7  │ 9           32  │ GP27
  LED G   ─── GP8  │ 10          31  │ GP26
  LED B   ─── GP9  │ 11          30  │ RUN
              GP10  │ 12          29  │ GP22
              GP11  │ 13          28  │ GND
              GP12  │ 14          27  │ GP21
              GP13  │ 15          26  │ GP20
              GP14  │ 16          25  │ GP19
 HC06 STATE─ GP15  │ 17          24  │ GP18 ──── BTN B
  BTN R  ─── GP16  │ 18          23  │ GP17 ──── BTN G
              GND  │ 19          22  │ GND
              GP15  │ 20          21  │ GP10
                    └─────────────────┘
```

---

## 6. Configuração do PC

### Dependências Python

```bash
pip install pyserial pyautogui pynput
```

### Parear o HC-06

1. Ligue o Pico — o LED vermelho começa a pulsar
2. No PC, vá em **Configurações → Bluetooth**
3. Conecte ao dispositivo **HOTD-CTRL** com PIN **1234**
4. Identifique a porta serial criada (ex: `/dev/rfcomm0` no Linux, `COM3` no Windows)

### Rodar o receptor

```bash
cd pc/
python receiver.py
```

Selecione a porta do HC-06 quando solicitado. O LED do Pico fica verde sólido quando o par está ativo.

### Abrir o jogo

Abra **The House of the Dead Remake** e configure o tipo de controle para **Keyboard + Mouse**. A sensibilidade do cursor pode ser ajustada no jogo (recomendado: 3.0).

---

## 7. Checklist de montagem

- [ ] MPU-6050 conectado no I2C1 (GP2/GP3) com pull-ups
- [ ] HC-06 alimentado em 5 V (VBUS), TX/RX cruzados com Pico
- [ ] GP6 (EN) do HC-06 conectado mas em nível baixo durante o jogo
- [ ] LED RGB com resistores 220 Ω em GP7/8/9
- [ ] Três botões entre GP16/17/18 e GND
- [ ] HC-06 pareado no PC com PIN 1234
- [ ] `receiver.py` rodando antes de abrir o jogo
