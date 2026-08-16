import serial
import tkinter as tk
from tkinter import ttk

# --- Serial Port Configuration ---
# Update to match the serial port of your Jaycar ESP32 on your Pi 5
SERIAL_PORT = '/dev/ttyUSB1' 
try:
    ser = serial.Serial(SERIAL_PORT, 115200, timeout=1)
except Exception:
    print(f"Could not open serial port {SERIAL_PORT}. Running GUI in offline demo mode.")
    ser = None

def send_cmd(cmd_string):
    if ser:
        ser.write(f"{cmd_string}\n".encode())
    else:
        print(f"Offline Command Stream: {cmd_string}")

def update_master():
    # Only try to read sliders if they have actually been created yet
    if 'gain_slider' in globals() and 'pclip_slider' in globals() and 'nclip_slider' in globals():
        send_cmd(f"GAIN={float(gain_slider.get()):.2f}")
        send_cmd(f"PCLIP={float(pclip_slider.get()):.2f}")
        send_cmd(f"NCLIP={float(nclip_slider.get()):.2f}")

def toggle_rotator():
    val = 1 if rot_var.get() else 0
    send_cmd(f"ROT_EN={val}")

def send_band_settings(band_name, th_s, rt_s, at_s, re_s):
    th = float(th_s.get())
    rt = float(rt_s.get())
    try:
        at = float(at_s.get())
        re = float(re_s.get())
    except ValueError:
        return 
    send_cmd(f"COMP={band_name},{th:.2f},{rt:.1f},{at:.1f},{re:.1f}")

def toggle_generator():
    if gen_var.get():
        send_cmd(f"TONE_FREQ={freq_entry.get()}")
        send_cmd("TONE_EN=1")
        gen_btn.config(text="Stop Calibration Tone")
    else:
        send_cmd("TONE_EN=0")
        gen_btn.config(text="Start Calibration Tone")

# --- UI Layout Design ---
root = tk.Tk()
root.title("AM Broadcast Studio Master Controller")
root.geometry("680x640")

# 1. Master & Asymmetry Panel
f_master = ttk.LabelFrame(root, text=" Master Drive & Asymmetry Ceilings ", padding=10)
f_master.pack(fill="x", padx=15, pady=5)

# Sliders are declared first without live updates until they are fully bound
gain_slider = ttk.Scale(f_master, from_=0.5, to=3.0, orient='horizontal')
gain_slider.set(1.0)
gain_slider.pack(fill='x')
ttk.Label(f_master, text="Master Drive (Input Gain)").pack(pady=(0, 10))

pclip_slider = ttk.Scale(f_master, from_=1.0, to=1.4, orient='horizontal')
pclip_slider.set(1.25)
pclip_slider.pack(fill='x')
ttk.Label(f_master, text="Positive Ceiling (+125% Loudness Boost)").pack(pady=(0, 10))

nclip_slider = ttk.Scale(f_master, from_=0.7, to=0.99, orient='horizontal')
nclip_slider.set(0.95)
nclip_slider.pack(fill='x')
ttk.Label(f_master, text="Negative Base Floor (Carrier Protection)").pack(pady=(0, 5))

# Attach the live update commands now that all components exist safely in memory
gain_slider.config(command=lambda v: update_master())
pclip_slider.config(command=lambda v: update_master())
nclip_slider.config(command=lambda v: update_master())

# 2. Phase Rotator & Filtering Row
f_opts = ttk.Frame(root)
f_opts.pack(fill="x", padx=15, pady=5)

f_rot = ttk.LabelFrame(f_opts, text=" Vocal Symmetry ", padding=10)
f_rot.pack(side="left", fill="both", expand=True, padx=(0, 5))
rot_var = tk.BooleanVar(value=True)
rot_chk = ttk.Checkbutton(f_rot, text="Enable 4-Stage Phase Rotator", variable=rot_var, command=toggle_rotator)
rot_chk.pack(anchor="w")

f_filter = ttk.LabelFrame(f_opts, text=" Final Post-Clip Spectral Mask ", padding=10)
f_filter.pack(side="right", fill="both", expand=True, padx=(5, 0))
mask_combo = ttk.Combobox(f_filter, values=["5 kHz", "9 kHz", "10 kHz", "12 kHz", "15 kHz"], state="readonly")
mask_combo.set("10 kHz")
mask_combo.bind("<<ComboboxSelected>>", lambda e: send_cmd(f"MASK={['5 kHz','9 kHz','10 kHz','12 kHz','15 kHz'].index(mask_combo.get())}"))
mask_combo.pack(fill="x")

# 3. Multiband Dynamics Column Utility
def create_band_ui(parent, name):
    frame = ttk.LabelFrame(parent, text=f" {name} Band Dynamics ", padding=10)
    frame.pack(side="left", fill="both", expand=True, padx=5, pady=5)
    
    ttk.Label(frame, text="Threshold").pack()
    th = ttk.Scale(frame, from_=0.05, to=1.0, orient='horizontal')
    th.set(0.3)
    th.pack(fill='x', pady=2)
    
    ttk.Label(frame, text="Compression Ratio").pack()
    rt = ttk.Scale(frame, from_=1.0, to=10.0, orient='horizontal')
    rt.set(4.0)
    rt.pack(fill='x', pady=2)

    ttk.Label(frame, text="Attack (ms)").pack(anchor="w")
    at = ttk.Entry(frame, width=8)
    at.insert(0, "10")
    at.pack(fill='x', pady=2)

    ttk.Label(frame, text="Release (ms)").pack(anchor="w")
    re = ttk.Entry(frame, width=8)
    re.insert(0, "100")
    re.pack(fill='x', pady=2)
    
    ttk.Button(frame, text="Apply Band", command=lambda: send_band_settings(name, th, rt, at, re)).pack(fill='x', pady=5)

f_bands = ttk.Frame(root)
f_bands.pack(fill="both", expand=True, padx=10, pady=5)
create_band_ui(f_bands, "LOW")
create_band_ui(f_bands, "MID")
create_band_ui(f_bands, "HIGH")

# 4. Calibration Deck Panel
f_gen = ttk.LabelFrame(root, text=" Transmitter Calibration Deck ", padding=10)
f_gen.pack(fill="x", padx=15, pady=10)

freq_frame = ttk.Frame(f_gen)
freq_frame.pack(fill="x", pady=5)
ttk.Label(freq_frame, text="Test Waveform Freq (Hz):").pack(side="left")
freq_entry = ttk.Entry(freq_frame, width=10)
freq_entry.insert(0, "400")
freq_entry.pack(side="left", padx=10)

gen_var = tk.BooleanVar(value=False)
gen_btn = ttk.Checkbutton(f_gen, text="Start Calibration Tone", variable=gen_var, command=toggle_generator, style="Toggle.TButton")
gen_btn.pack(fill="x", pady=5)

root.mainloop()

