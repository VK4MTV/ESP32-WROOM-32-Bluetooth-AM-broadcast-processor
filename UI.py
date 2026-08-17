import serial
import serial.tools.list_ports
import tkinter as tk
from tkinter import ttk

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

def save(): cmd("SAVE")
def master(v=None):
    cmd(f"GAIN={gain.get():.2f}"); cmd(f"PCLIP={pclip.get():.2f}"); cmd(f"NCLIP={nclip.get():.2f}")
def rot(): cmd(f"ROT_EN={1 if rotv.get() else 0}")
def band(n, th, rt, at, re, gt=None):
    g = float(gt.get()) if gt else 0.01
    try: cmd(f"COMP={n},{float(th.get()):.2f},{float(rt.get()):.1f},{float(at.get()):.1f},{float(re.get()):.1f},{g:.3f}")
    except: pass
def gen():
    if genv.get(): cmd(f"TONE_FREQ={fe.get()}"); cmd("TONE_EN=1")
    else: cmd("TONE_EN=0")

root = tk.Tk()
root.title("AM Processor")
root.geometry("420x360")

tk.Label(root, text=f"Online: {PORT}" if ser else "OFFLINE", 
         bg="#2ecc71" if ser else "#e74c3c", fg="white").pack(fill="x")

# SCROLLABLE AREA
canvas = tk.Canvas(root, highlightthickness=0, height=300)
sb = ttk.Scrollbar(root, orient="vertical", command=canvas.yview)
canvas.configure(yscrollcommand=sb.set)

sb.pack(side="right", fill="y")
canvas.pack(side="left", fill="both", expand=True)

content = ttk.Frame(canvas)
canvas.create_window((0,0), window=content, anchor="nw")

def update_scrollregion(event=None):
    canvas.configure(scrollregion=canvas.bbox("all"))
content.bind("<Configure>", update_scrollregion)

def on_mousewheel(event):
    canvas.yview_scroll(int(-1*(event.delta/120)), "units")
canvas.bind_all("<MouseWheel>", on_mousewheel)

# CONTENT
left = ttk.Frame(content)
left.pack(fill="x", padx=4, pady=2)

f1 = ttk.LabelFrame(left, text="Drive + Limiter", padding=4)
f1.pack(fill="x", pady=2)
gain = ttk.Scale(f1, from_=0.5, to=3.0, length=140, orient="h"); gain.set(1); gain.pack(anchor="w")
ttk.Label(f1, text="Gain").pack(anchor="w")
pclip = ttk.Scale(f1, from_=1, to=1.4, length=140, orient="h"); pclip.set(1.25); pclip.pack(anchor="w", pady=1)
ttk.Label(f1, text="+Clip").pack(anchor="w")
nclip = ttk.Scale(f1, from_=0.5, to=0.99, length=140, orient="h"); nclip.set(0.95); nclip.pack(anchor="w", pady=1)
ttk.Label(f1, text="-Clip").pack(anchor="w")
gain.config(command=master); pclip.config(command=master); nclip.config(command=master)

# Final output level
out_gain = ttk.Scale(f1, from_=0.0, to=1.5, length=140, orient="h"); out_gain.set(1.0); out_gain.pack(anchor="w", pady=(4,0))
ttk.Label(f1, text="Output Level").pack(anchor="w")
def send_out(v=None): cmd(f"OUTGAIN={out_gain.get():.2f}")
out_gain.config(command=send_out)

ttk.Button(f1, text="Save Flash", command=save).pack(fill="x", pady=3)

def mkband(p, name):
    f = ttk.LabelFrame(p, text=name+" Band", padding=3)
    f.pack(fill="x", pady=2)
    
    # Row 1: Threshold + Ratio
    row1 = ttk.Frame(f); row1.pack(fill="x")
    th = ttk.Scale(row1, from_=0.05, to=1, length=90, orient="h"); th.set(0.3); th.pack(side="left")
    ttk.Label(row1, text="Th").pack(side="left")
    rt = ttk.Scale(row1, from_=1, to=10, length=60, orient="h"); rt.set(4); rt.pack(side="left", padx=2)
    ttk.Label(row1, text="R").pack(side="left")
    
    # Row 2: Attack / Release / Gate
    row2 = ttk.Frame(f); row2.pack(fill="x", pady=1)
    at = ttk.Entry(row2, width=4); at.insert(0,"10"); at.pack(side="left")
    ttk.Label(row2, text="At").pack(side="left")
    re = ttk.Entry(row2, width=4); re.insert(0,"80"); re.pack(side="left", padx=1)
    ttk.Label(row2, text="Re").pack(side="left")
    gt = ttk.Entry(row2, width=5); gt.insert(0,"0.01"); gt.pack(side="left", padx=2)
    ttk.Label(row2, text="Gate").pack(side="left")
    
    def live(v=None): band(name, th, rt, at, re, gt)
    th.config(command=live); rt.config(command=live)
    ttk.Button(f, text="Apply", width=5, command=lambda: band(name, th, rt, at, re, gt)).pack(side="right", padx=2)

mkband(left, "LOW"); mkband(left, "MID"); mkband(left, "HIGH")

# Slow single-band AGC / gain rider
slowf = ttk.LabelFrame(left, text="Slow AGC (Gain Rider)", padding=3)
slowf.pack(fill="x", pady=3)

row1 = ttk.Frame(slowf); row1.pack(fill="x")
s_th = ttk.Scale(row1, from_=0.05, to=1, length=90, orient="h"); s_th.set(0.25); s_th.pack(side="left")
ttk.Label(row1, text="Th").pack(side="left")
s_rt = ttk.Scale(row1, from_=1, to=6, length=60, orient="h"); s_rt.set(3); s_rt.pack(side="left", padx=2)
ttk.Label(row1, text="R").pack(side="left")

row2 = ttk.Frame(slowf); row2.pack(fill="x", pady=1)
s_at = ttk.Entry(row2, width=5); s_at.insert(0,"200"); s_at.pack(side="left")
ttk.Label(row2, text="At ms").pack(side="left")
s_re = ttk.Entry(row2, width=5); s_re.insert(0,"800"); s_re.pack(side="left", padx=2)
ttk.Label(row2, text="Re ms").pack(side="left")
s_gt = ttk.Entry(row2, width=5); s_gt.insert(0,"0.005"); s_gt.pack(side="left", padx=2)
ttk.Label(row2, text="Gate").pack(side="left")

def live_slow(v=None): band("SLOW", s_th, s_rt, s_at, s_re, s_gt)
s_th.config(command=live_slow); s_rt.config(command=live_slow)
ttk.Button(slowf, text="Apply Slow", width=9, command=lambda: band("SLOW", s_th, s_rt, s_at, s_re, s_gt)).pack(fill="x", pady=2)

right = ttk.Frame(content)
right.pack(fill="x", padx=4, pady=2)

f2 = ttk.LabelFrame(right, text="Symmetry", padding=4)
f2.pack(fill="x")
rotv = tk.BooleanVar(value=True)
ttk.Checkbutton(f2, text="Phase Rotator", variable=rotv, command=rot).pack(anchor="w")

f3 = ttk.LabelFrame(right, text="Mask kHz", padding=4)
f3.pack(fill="x", pady=2)
mc = ttk.Combobox(f3, values=["5","9","10","12","15"], width=3, state="readonly"); mc.set("10")
def msk(e=None): cmd(f"MASK={['5','9','10','12','15'].index(mc.get())}")
mc.bind("<<ComboboxSelected>>", msk); mc.pack(anchor="w")

f4 = ttk.LabelFrame(right, text="Cal", padding=4)
f4.pack(fill="x", pady=2)
ff = ttk.Frame(f4); ff.pack(fill="x")
ttk.Label(ff, text="Hz").pack(side="left")
fe = ttk.Entry(ff, width=4); fe.insert(0,"400"); fe.pack(side="left")

# Waveform type
wave_var = tk.IntVar(value=0)
wrow = ttk.Frame(f4); wrow.pack(fill="x", pady=1)
ttk.Radiobutton(wrow, text="Sine", variable=wave_var, value=0, command=lambda: cmd("WAVE=0")).pack(side="left")
ttk.Radiobutton(wrow, text="Square", variable=wave_var, value=1, command=lambda: cmd("WAVE=1")).pack(side="left")
ttk.Radiobutton(wrow, text="Saw", variable=wave_var, value=2, command=lambda: cmd("WAVE=2")).pack(side="left")

# Injection point
post_var = tk.BooleanVar(value=False)
ttk.Checkbutton(f4, text="Post-Clipper", variable=post_var, command=lambda: cmd(f"TONE_POST={1 if post_var.get() else 0}")).pack(anchor="w")

genv = tk.BooleanVar(value=False)
ttk.Checkbutton(f4, text="Tone", variable=genv, command=gen).pack(anchor="w")
tiltv = tk.BooleanVar(value=False)
ttk.Checkbutton(f4, text="Tilt75", variable=tiltv, command=lambda: cmd(f"TILT_EN={1 if tiltv.get() else 0}")).pack(anchor="w")
ttk.Button(f4, text="Save Flash", command=save).pack(fill="x", pady=2)

root.after(150, master)
root.after(250, rot)
root.after(350, msk)

root.mainloop()
