"""Parts and connectivity of the AC IR Matter module (single source for the schematic and the board).

Every part lists the net of each used pin. Pins not listed are marked no-connect in the schematic.
Net names are the names that end up in the netlist and on the board.
"""

PROJECT = "ac_ir_s3"
TITLE = "AC IR Matter module (ESP32-S3-WROOM-1-N16R8)"
REV = "1.0"
DATE = "2026-11-09"
COMPANY = "DIY - air-connditioner-control-matter"

# Power nets drawn with power symbols. Every other net uses net labels.
POWER_SYMBOLS = {"GND": "power:GND", "+3V3": "power:+3V3", "+5V": "power:+5V", "VBUS": "power:VBUS"}
# Nets that need a PWR_FLAG (driven only by passive pins).
PWR_FLAG_NETS = ["GND", "VBUS", "+5V", "LED_VDD", "IR_VS"]

R0603 = "Resistor_SMD:R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"
C0603 = "Capacitor_SMD:C_0603_1608Metric_Pad1.08x0.95mm_HandSolder"
C0805 = "Capacitor_SMD:C_0805_2012Metric_Pad1.18x1.45mm_HandSolder"
R1206 = "Resistor_SMD:R_1206_3216Metric_Pad1.30x1.75mm_HandSolder"

# ref: lib symbol, value, footprint, {pin: net}, extra fields (MPN, LCSC, Note)
PARTS = {
    "U1": dict(lib="RF_Module:ESP32-S3-WROOM-1", value="ESP32-S3-WROOM-1-N16R8", fp="RF_Module:ESP32-S3-WROOM-1",
               pins={"1": "GND", "40": "GND", "41": "GND", "2": "+3V3", "3": "EN", "27": "IO0",
                     "4": "IR_TX", "5": "IR_RX", "12": "SDA", "17": "SCL", "13": "USB_DN", "14": "USB_DP",
                     "25": "RGB", "36": "RXD0", "37": "TXD0"},
               fields={"Spec": "Wi-Fi/BLE module, 16 MB flash, 8 MB octal PSRAM", "MPN": "ESP32-S3-WROOM-1-N16R8"}),
    "J1": dict(lib="Connector:USB_C_Receptacle_USB2.0_16P", value="USB-C", fp="Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
               pins={"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "S1": "GND",
                     "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
                     "A5": "CC1", "B5": "CC2", "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DN", "B7": "USB_DN"},
               fields={"Spec": "USB-C receptacle 16P, SMD + 4 shell pins", "MPN": "HRO TYPE-C-31-M-12", "LCSC": "C165948"}),
    "F1": dict(lib="Device:Polyfuse", value="PTC 0.75A hold", fp="Fuse:Fuse_1206_3216Metric_Pad1.42x1.75mm_HandSolder",
               pins={"1": "VBUS", "2": "+5V"}, fields={"Spec": "PTC resettable fuse 1206, I_hold 0.75 A, >= 6 V"}),
    "U3": dict(lib="Power_Protection:USBLC6-2SC6", value="USBLC6-2SC6", fp="Package_TO_SOT_SMD:SOT-23-6",
               pins={"1": "USB_DP", "6": "USB_DP", "3": "USB_DN", "4": "USB_DN", "2": "GND", "5": "VBUS"},
               fields={"Spec": "USB ESD protection SOT-23-6", "MPN": "USBLC6-2SC6", "LCSC": "C7519"}),
    "U2": dict(lib="Regulator_Linear:AP2112K-3.3", value="AP2112K-3.3", fp="Package_TO_SOT_SMD:SOT-23-5",
               pins={"1": "+5V", "3": "+5V", "2": "GND", "5": "+3V3"},
               fields={"Spec": "LDO 3.3 V 600 mA SOT-23-5", "MPN": "AP2112K-3.3TRG1", "LCSC": "C51118"}),
    "Q1": dict(lib="Transistor_FET:AO3400A", value="AO3400A", fp="Package_TO_SOT_SMD:SOT-23",
               pins={"1": "IR_GATE", "2": "GND", "3": "IR_K"}, fields={"Spec": "N-MOSFET 30 V, logic level, SOT-23", "MPN": "AO3400A", "LCSC": "C20917"}),
    "D1": dict(lib="Diode:1N4148W", value="1N4148W", fp="Diode_SMD:D_SOD-123",
               pins={"1": "LED_VDD", "2": "+5V"}, fields={"Spec": "small-signal diode SOD-123", "MPN": "1N4148W", "LCSC": "C81598"}),
    "D2": dict(lib="Device:LED", value="IR 940nm", fp="LED_THT:LED_D5.0mm_Horizontal_O1.27mm_Z3.0mm_IRBlack",
               pins={"2": "IR_A", "1": "IR_M"}, fields={"Spec": "IR LED 5 mm 940 nm, >= 100 mA", "MPN": "TSAL6200"}),
    "D3": dict(lib="Device:LED", value="IR 940nm", fp="LED_THT:LED_D5.0mm_Horizontal_O1.27mm_Z3.0mm_IRBlack",
               pins={"2": "IR_M", "1": "IR_K"}, fields={"Spec": "IR LED 5 mm 940 nm, >= 100 mA", "MPN": "TSAL6200"}),
    "D4": dict(lib="LED:SK6812MINI", value="SK6812MINI", fp="LED_SMD:LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm",
               pins={"4": "LED_VDD", "2": "GND", "3": "RGB_DIN"}, fields={"Spec": "addressable RGB LED 3.5x3.5 mm", "MPN": "SK6812MINI"}),
    "U4": dict(lib="Interface_Optical:TSOP382xx", value="TSOP38238", fp="OptoDevice:Vishay_MINICAST-3Pin",
               pins={"1": "IR_RX", "2": "GND", "3": "IR_VS"},
               fields={"Spec": "IR receiver 38 kHz, pinout OUT-GND-VS (TSOP38238, VS1838B)", "MPN": "TSOP38238"}),
    "SW1": dict(lib="Switch:SW_Push", value="BOOT", fp="Button_Switch_SMD:SW_SPST_PTS810",
                pins={"1": "IO0", "2": "GND"}, fields={"Spec": "tactile switch 4.2x3.2 mm SMD", "MPN": "C&K PTS810 SJM 250 SMTR LFS"}),
    "SW2": dict(lib="Switch:SW_Push", value="RESET", fp="Button_Switch_SMD:SW_SPST_PTS810",
                pins={"1": "EN", "2": "GND"}, fields={"Spec": "tactile switch 4.2x3.2 mm SMD", "MPN": "C&K PTS810 SJM 250 SMTR LFS"}),
    "J2": dict(lib="Connector_Generic:Conn_01x06", value="UART0+I2C", fp="Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
               pins={"1": "GND", "2": "+3V3", "3": "TXD0", "4": "RXD0", "5": "SDA", "6": "SCL"},
               fields={"Spec": "male pin header 1x6 2.54 mm (optional)"}),
    "R1": dict(lib="Device:R", value="10k", fp=R0603, spec="0603 1%", pins={"1": "+3V3", "2": "EN"}, fields={"LCSC": "C25804"}),
    "R2": dict(lib="Device:R", value="10k", fp=R0603, spec="0603 1%", pins={"1": "+3V3", "2": "IO0"}, fields={"LCSC": "C25804"}),
    "R3": dict(lib="Device:R", value="5.1k", fp=R0603, spec="0603 1%", pins={"1": "CC1", "2": "GND"}, fields={"LCSC": "C23186"}),
    "R4": dict(lib="Device:R", value="5.1k", fp=R0603, spec="0603 1%", pins={"1": "CC2", "2": "GND"}, fields={"LCSC": "C23186"}),
    "R5": dict(lib="Device:R", value="100", fp=R0603, spec="0603 1%", pins={"1": "+3V3", "2": "IR_VS"}, fields={"LCSC": "C22775"}),
    "R6": dict(lib="Device:R", value="100", fp=R0603, spec="0603 1%", pins={"1": "IR_TX", "2": "IR_GATE"}, fields={"LCSC": "C22775"}),
    "R7": dict(lib="Device:R", value="10k", fp=R0603, spec="0603 1%", pins={"1": "IR_GATE", "2": "GND"}, fields={"LCSC": "C25804"}),
    "R8": dict(lib="Device:R", value="22 1/4W", fp=R1206, spec="1206 1% 0.25 W", pins={"1": "+5V", "2": "IR_A"},
               fields={}),
    "R9": dict(lib="Device:R", value="100", fp=R0603, spec="0603 1%", pins={"1": "RGB", "2": "RGB_DIN"}, fields={"LCSC": "C22775"}),
    "C1": dict(lib="Device:C", value="1u", fp=C0603, spec="0603 X5R 25 V", pins={"1": "+3V3", "2": "GND"}, fields={"LCSC": "C15849"}),
    "C2": dict(lib="Device:C", value="4.7u", fp=C0805, spec="0805 X5R 25 V", pins={"1": "+5V", "2": "GND"}, fields={"LCSC": "C1779"}),
    "C3": dict(lib="Device:C", value="10u", fp=C0805, spec="0805 X5R 25 V", pins={"1": "+3V3", "2": "GND"}, fields={"LCSC": "C15850"}),
    "C4": dict(lib="Device:C", value="100n", fp=C0603, spec="0603 X7R 50 V", pins={"1": "+3V3", "2": "GND"}, fields={"LCSC": "C14663"}),
    "C5": dict(lib="Device:C", value="1u", fp=C0603, spec="0603 X5R 25 V", pins={"1": "EN", "2": "GND"}, fields={"LCSC": "C15849"}),
    "C6": dict(lib="Device:C", value="4.7u", fp=C0805, spec="0805 X5R 25 V", pins={"1": "IR_VS", "2": "GND"}, fields={"LCSC": "C1779"}),
    "C7": dict(lib="Device:C", value="100n", fp=C0603, spec="0603 X7R 50 V", pins={"1": "LED_VDD", "2": "GND"}, fields={"LCSC": "C14663"}),
    "C8": dict(lib="Device:C", value="4.7u", fp=C0805, spec="0805 X5R 25 V", pins={"1": "+5V", "2": "GND"}, fields={"LCSC": "C1779"}),
    "H1": dict(lib="Mechanical:MountingHole", value="M2", fp="MountingHole:MountingHole_2.2mm_M2", pins={}, fields={}),
    "H2": dict(lib="Mechanical:MountingHole", value="M2", fp="MountingHole:MountingHole_2.2mm_M2", pins={}, fields={}),
}


def fields(ref):
    """Extra symbol fields (Spec, MPN, LCSC) of a part."""
    p = PARTS[ref]
    f = dict(p.get("fields", {}))
    if "spec" in p:
        f = dict({"Spec": p["spec"]}, **f)
    return f


def nets():
    """net name -> sorted list of (ref, pin)."""
    out = {}
    for ref, p in PARTS.items():
        for pin, net in p["pins"].items():
            out.setdefault(net, []).append((ref, pin))
    return {k: sorted(v) for k, v in sorted(out.items())}
