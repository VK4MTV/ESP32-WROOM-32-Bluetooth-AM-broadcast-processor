import serial
import serial.tools.list_ports
import tkinter as tk
from tkinter import ttk, filedialog

def find_port():
    for p in serial.tools.list_ports.comports():
        d = (str(p.description)+str(p.device)).lower()
        if any(k in d for k in ["usb","uart","cp210","ch340","acm"]):
            return p.device
    return None

PORT = find_port()
ser = serial.Serial(PORT, 115200, timeout=1) if PORT else None

def cmd(s):
    if ser and ser.is_open: ser.write((s+"\n").encode())
    else: print("OFF:", s)

def save_to_esp(): cmd("SAVE")

def send_master(v=None):
    cmd(f"GAIN={gain.get():.2f}")
    cmd(f"PCLIP={pclip.get():.2f}")
    cmd(f"NCLIP={nclip.get():.2f}")
    cmd(f"OUTGAIN={outg.get():.2f}")

def send_rot(): cmd(f"ROT_EN={1 if rotv.get() else 0}")

def send_band(name, th, rt, at, re, gt):
    try:
        cmd(f"COMP={name},{float(th.get()):.2f},{float(rt.get()):.1f},{float(at.get()):.1f},{float(re.get()):.1f},{float(gt.get()):.3f}")
    except: pass

def send_slow():
    try:
        cmd(f"COMP=SLOW,{float(s_th.get()):.2f},{float(s_rt.get()):.1f},{float(s_at.get()):.1f},{float(s_re.get()):.1f},{float(s_gt.get()):.3f}")
    except: pass

def send_gen():
    if genv.get():
        cmd(f"TONE_FREQ={fe.get()}")
        cmd("TONE_EN=1")
    else:
        cmd("TONE_EN=0")

def send_wave(): cmd(f"WAVE={wave_var.get()}")
def send_post(): cmd(f"TONE_POST={1 if post_var.get() else 0}")
def send_tilt(): cmd(f"TILT_EN={1 if tiltv.get() else 0}")

def update_label(slider, lbl, fmt="{:.2f}"):
    lbl.config(text=fmt.format(float(slider.get())))

# ============== PRESET SYSTEM ==============
def save_preset():
    path = filedialog.asksaveasfilename(defaultextension=".txt", filetypes=[("Preset","*.txt")])
    if not path: return
    with open(path, "w") as f:
        f.write("# AM Broadcast Processor Preset\n")
        f.write(f"GAIN={gain.get():.3f}\n")
        f.write(f"PCLIP={pclip.get():.3f}\n")
        f.write(f"NCLIP={nclip.get():.3f}\n")
        f.write(f"OUTGAIN={outg.get():.3f}\n")
        f.write(f"ROT={1 if rotv.get() else 0}\n")
        f.write(f"MASK={mc.get()}\n")
        f.write(f"LOW_TH={low_th.get():.3f}\nLOW_RT={low_rt.get():.2f}\nLOW_AT={low_at.get()}\nLOW_RE={low_re.get()}\nLOW_GATE={low_gt.get()}\n")
        f.write(f"MID_TH={mid_th.get():.3f}\nMID_RT={mid_rt.get():.2f}\nMID_AT={mid_at.get()}\nMID_RE={mid_re.get()}\nMID_GATE={mid_gt.get()}\n")
        f.write(f"HIGH_TH={high_th.get():.3f}\nHIGH_RT={high_rt.get():.2f}\nHIGH_AT={high_at.get()}\nHIGH_RE={high_re.get()}\nHIGH_GATE={high_gt.get()}\n")
        f.write(f"SLOW_TH={s_th.get():.3f}\nSLOW_RT={s_rt.get():.2f}\nSLOW_AT={s_at.get()}\nSLOW_RE={s_re.get()}\nSLOW_GATE={s_gt.get()}\n")
    print("Preset saved:", path)

def load_preset():
    path = filedialog.askopenfilename(filetypes=[("Preset","*.txt")])
    if not path: return
    vals = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or not line: continue
            k, v = line.split("=", 1)
            vals[k] = v
    if "GAIN" in vals: gain.set(float(vals["GAIN"]))
    if "PCLIP" in vals: pclip.set(float(vals["PCLIP"]))
    if "NCLIP" in vals: nclip.set(float(vals["NCLIP"]))
    if "OUTGAIN" in vals: outg.set(float(vals["OUTGAIN"]))
    if "ROT" in vals: rotv.set(int(vals["ROT"]))
    if "MASK" in vals: mc.set(vals["MASK"])
    if "LOW_TH" in vals: low_th.set(float(vals["LOW_TH"]))
    if "LOW_RT" in vals: low_rt.set(float(vals["LOW_RT"]))
    if "LOW_AT" in vals: low_at.delete(0,"end"); low_at.insert(0, vals["LOW_AT"])
    if "LOW_RE" in vals: low_re.delete(0,"end"); low_re.insert(0, vals["LOW_RE"])
    if "LOW_GATE" in vals: low_gt.delete(0,"end"); low_gt.insert(0, vals["LOW_GATE"])
    if "MID_TH" in vals: mid_th.set(float(vals["MID_TH"]))
    if "MID_RT" in vals: mid_rt.set(float(vals["MID_RT"]))
    if "MID_AT" in vals: mid_at.delete(0,"end"); mid_at.insert(0, vals["MID_AT"])
    if "MID_RE" in vals: mid_re.delete(0,"end"); mid_re.insert(0, vals["MID_RE"])
    if "MID_GATE" in vals: mid_gt.delete(0,"end"); mid_gt.insert(0, vals["MID_GATE"])
    if "HIGH_TH" in vals: high_th.set(float(vals["HIGH_TH"]))
    if "HIGH_RT" in vals: high_rt.set(float(vals["HIGH_RT"]))
    if "HIGH_AT" in vals: high_at.delete(0,"end"); high_at.insert(0, vals["HIGH_AT"])
    if "HIGH_RE" in vals: high_re.delete(0,"end"); high_re.insert(0, vals["HIGH_RE"])
    if "HIGH_GATE" in vals: high_gt.delete(0,"end"); high_gt.insert(0, vals["HIGH_GATE"])
    if "SLOW_TH" in vals: s_th.set(float(vals["SLOW_TH"]))
    if "SLOW_RT" in vals: s_rt.set(float(vals["SLOW_RT"]))
    if "SLOW_AT" in vals: s_at.delete(0,"end"); s_at.insert(0, vals["SLOW_AT"])
    if "SLOW_RE" in vals: s_re.delete(0,"end"); s_re.insert(0, vals["SLOW_RE"])
    if "SLOW_GATE" in vals: s_gt.delete(0,"end"); s_gt.insert(0, vals["SLOW_GATE"])

    send_master()
    send_rot()
    send_band("LOW", low_th, low_rt, low_at, low_re, low_gt)
    send_band("MID", mid_th, mid_rt, mid_at, mid_re, mid_gt)
    send_band("HIGH", high_th, high_rt, high_at, high_re, high_gt)
    send_slow()
    print("Preset loaded:", path)

# ============== UI ==============
root = tk.Tk()
root.title("AM Broadcast Processor")
root.geometry("520x520")

tk.Label(root, text=f"Online: {PORT}" if ser else "OFFLINE", 
         bg="#2ecc71" if ser else "#e74c3c", fg="white").pack(fill="x")

canvas = tk.Canvas(root, highlightthickness=0, height=460)
sb = ttk.Scrollbar(root, orient="vertical", command=canvas.yview)
canvas.configure(yscrollcommand=sb.set)
sb.pack(side="right", fill="y")
canvas.pack(side="left", fill="both", expand=True)

content = ttk.Frame(canvas)
canvas.create_window((0,0), window=content, anchor="nw")

def update_scroll(e=None): canvas.configure(scrollregion=canvas.bbox("all"))
content.bind("<Configure>", update_scroll)
canvas.bind_all("<MouseWheel>", lambda e: canvas.yview_scroll(int(-1*(e.delta/120)), "units"))

left = ttk.Frame(content)
left.pack(fill="x", padx=6, pady=4)

# === DRIVE + LIMITER ===
f1 = ttk.LabelFrame(left, text="Drive + Limiter", padding=5)
f1.pack(fill="x", pady=3)

def make_slider(parent, text, frm, to, default, cmd_func, fmt="{:.2f}"):
    row = ttk.Frame(parent); row.pack(fill="x", pady=1)
    ttk.Label(row, text=text, width=10).pack(side="left")
    sl = ttk.Scale(row, from_=frm, to=to, length=160, orient="h")
    sl.set(default)
    sl.pack(side="left", padx=3)
    lbl = ttk.Label(row, text=fmt.format(default), width=6)
    lbl.pack(side="left")
    sl.config(command=lambda v: (cmd_func(), update_label(sl, lbl, fmt)))
    return sl, lbl

gain, _ = make_slider(f1, "Gain", 0.5, 3.0, 1.0, send_master)
pclip, _ = make_slider(f1, "+Clip", 1.0, 1.4, 1.25, send_master)
nclip, _ = make_slider(f1, "-Clip", 0.5, 0.99, 0.95, send_master)
outg, _ = make_slider(f1, "OutLevel", 0.0, 1.5, 1.0, send_master)

ttk.Button(f1, text="Save to ESP32 Flash", command=save_to_esp).pack(fill="x", pady=4)

# === 3-BAND COMPRESSOR ===
def make_band(parent, name, th_def=0.3, rt_def=4.0):
    f = ttk.LabelFrame(parent, text=name+" Band", padding=4)
    f.pack(fill="x", pady=2)
    
    row1 = ttk.Frame(f); row1.pack(fill="x")
    th = ttk.Scale(row1, from_=0.05, to=1, length=100, orient="h"); th.set(th_def); th.pack(side="left")
    th_lbl = ttk.Label(row1, text=f"{th_def:.2f}", width=5); th_lbl.pack(side="left")
    th.config(command=lambda v: (send_band(name, th, rt, at, re, gt), update_label(th, th_lbl)))
    
    rt = ttk.Scale(row1, from_=1, to=10, length=70, orient="h"); rt.set(rt_def); rt.pack(side="left", padx=3)
    rt_lbl = ttk.Label(row1, text=f"{rt_def:.1f}", width=4); rt_lbl.pack(side="left")
    rt.config(command=lambda v: (send_band(name, th, rt, at, re, gt), update_label(rt, rt_lbl, "{:.1f}")))
    
    row2 = ttk.Frame(f); row2.pack(fill="x", pady=1)
    at = ttk.Entry(row2, width=5); at.insert(0,"10"); at.pack(side="left")
    re = ttk.Entry(row2, width=5); re.insert(0,"80"); re.pack(side="left", padx=2)
    gt = ttk.Entry(row2, width=6); gt.insert(0,"0.01"); gt.pack(side="left", padx=2)
    
    ttk.Button(f, text="Apply", width=6, command=lambda: send_band(name, th, rt, at, re, gt)).pack(side="right")
    return th, rt, at, re, gt

low_th, low_rt, low_at, low_re, low_gt   = make_band(left, "LOW")
mid_th, mid_rt, mid_at, mid_re, mid_gt   = make_band(left, "MID")
high_th, high_rt, high_at, high_re, high_gt = make_band(left, "HIGH")

# === SLOW AGC ===
sf = ttk.LabelFrame(left, text="Slow AGC (Gain Rider)", padding=4)
sf.pack(fill="x", pady=3)

row1 = ttk.Frame(sf); row1.pack(fill="x")
s_th = ttk.Scale(row1, from_=0.05, to=1, length=100, orient="h"); s_th.set(0.25); s_th.pack(side="left")
s_th_lbl = ttk.Label(row1, text="0.25", width=5); s_th_lbl.pack(side="left")
s_th.config(command=lambda v: (send_slow(), update_label(s_th, s_th_lbl)))

s_rt = ttk.Scale(row1, from_=1, to=6, length=70, orient="h"); s_rt.set(3); s_rt.pack(side="left", padx=3)
s_rt_lbl = ttk.Label(row1, text="3.0", width=4); s_rt_lbl.pack(side="left")
s_rt.config(command=lambda v: (send_slow(), update_label(s_rt, s_rt_lbl, "{:.1f}")))

row2 = ttk.Frame(sf); row2.pack(fill="x", pady=1)
s_at = ttk.Entry(row2, width=5); s_at.insert(0,"200"); s_at.pack(side="left")
s_re = ttk.Entry(row2, width=5); s_re.insert(0,"800"); s_re.pack(side="left", padx=2)
s_gt = ttk.Entry(row2, width=6); s_gt.insert(0,"0.005"); s_gt.pack(side="left", padx=2)
ttk.Button(sf, text="Apply Slow", command=send_slow).pack(side="right")

# === RIGHT COLUMN ===
right = ttk.Frame(content)
right.pack(fill="x", padx=6, pady=4)

f_sym = ttk.LabelFrame(right, text="Symmetry", padding=4)
f_sym.pack(fill="x")
rotv = tk.BooleanVar(value=True)
ttk.Checkbutton(f_sym, text="Phase Rotator", variable=rotv, command=send_rot).pack(anchor="w")

f_mask = ttk.LabelFrame(right, text="Mask kHz", padding=4)
f_mask.pack(fill="x", pady=2)
mc = ttk.Combobox(f_mask, values=["5","9","10","12","15"], width=4, state="readonly"); mc.set("10")
mc.bind("<<ComboboxSelected>>", lambda e: cmd(f"MASK={['5','9','10','12','15'].index(mc.get())}"))
mc.pack(anchor="w")

f_cal = ttk.LabelFrame(right, text="Test Generator", padding=4)
f_cal.pack(fill="x", pady=2)

ff = ttk.Frame(f_cal); ff.pack(fill="x")
ttk.Label(ff, text="Hz").pack(side="left")
fe = ttk.Entry(ff, width=5); fe.insert(0,"400"); fe.pack(side="left")

wave_var = tk.IntVar(value=0)
wrow = ttk.Frame(f_cal); wrow.pack(fill="x", pady=1)
ttk.Radiobutton(wrow, text="Sine", variable=wave_var, value=0, command=send_wave).pack(side="left")
ttk.Radiobutton(wrow, text="Square", variable=wave_var, value=1, command=send_wave).pack(side="left")
ttk.Radiobutton(wrow, text="Saw", variable=wave_var, value=2, command=send_wave).pack(side="left")

post_var = tk.BooleanVar(value=False)
ttk.Checkbutton(f_cal, text="Inject AFTER clipper", variable=post_var, command=send_post).pack(anchor="w")

genv = tk.BooleanVar(value=False)
ttk.Checkbutton(f_cal, text="Tone On", variable=genv, command=send_gen).pack(anchor="w")
tiltv = tk.BooleanVar(value=False)
ttk.Checkbutton(f_cal, text="Tilt 75Hz Square", variable=tiltv, command=send_tilt).pack(anchor="w")

f_pre = ttk.LabelFrame(right, text="Presets", padding=4)
f_pre.pack(fill="x", pady=4)
ttk.Button(f_pre, text="Save Preset File", command=save_preset).pack(fill="x", pady=1)
ttk.Button(f_pre, text="Load Preset File", command=load_preset).pack(fill="x", pady=1)

root.after(200, send_master)
root.after(300, send_rot)
root.mainloop()
