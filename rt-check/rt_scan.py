#!/usr/bin/env python3
"""Static RT(PREEMPT_RT)-compatibility scan for Allwinner BSP vendor drivers.

On 6.6-rt, spinlock_t becomes a sleeping lock. Truly-atomic contexts on RT are:
hardirq (irq_chip callbacks, IRQF_NO_THREAD/PERCPU/chained handlers, hrtimer
callbacks, NMI), raw_spinlock-held regions, preempt_disable and local_irq_save
regions. Taking a non-raw spinlock_t (or calling a sleeping API) there triggers
"BUG: sleeping function called from invalid context" / scheduling-while-atomic.

Findings taxonomy:
  A. irq_chip callback uses non-raw spinlock or sleeping call
  B. IRQF_NO_THREAD/PERCPU/chained handler uses non-raw spinlock or sleeping call
  C. hrtimer callback uses non-raw spinlock or sleeping call
  E. non-raw spinlock taken inside raw_spinlock / preempt_disable / local_irq_save region
"""
import os, re, sys, json
from collections import defaultdict

ROOT = "/home/helios/Desktop/orangepi-build/kernel/orange-pi-6.6-sun60iw2"
SCANDIRS = ["bsp/drivers", "bsp/modules", "bsp/platform",
            "drivers/soc/sunxi", "drivers/clk/sunxi-ng", "sound/soc/sunxi",
            "drivers/media/platform/sunxi"]

NONRAW_LOCK = re.compile(r"\bspin_(lock|lock_irq|lock_irqsave|lock_bh|trylock)\s*\(")
RAW_LOCK = re.compile(r"\braw_spin_(lock|lock_irqsave|lock_irq|trylock)\s*\(")
SLEEP = re.compile(
    r"\b(msleep|usleep_range|wait_event\w*|wait_for_completion\w*|mutex_(lock|lock_interruptible|lock_nested)"
    r"|down(_interruptible|_killable|_trylock)?\s*\(|schedule\s*\(|schedule_timeout\w*|dma_alloc_\w+"
    r"|pm_runtime_(get_sync|put_sync|resume|suspend)|regulator_(enable|disable)|i2c_transfer|i2c_smbus_\w+"
    r"|spi_(sync|write_then_read|read|write)\s*\(|clk_prepare|gpiod_set_value_cansleep|vfree|kvfree)\b")
ATOMIC_ENTER = re.compile(
    r"\b(preempt_disable|local_irq_save|local_irq_disable|raw_spin_lock_irqsave|nmi_enter)\s*\(")
ATOMIC_EXIT = re.compile(
    r"\b(preempt_enable|local_irq_restore|local_irq_enable|raw_spin_unlock_irqrestore)\s*\(")

CHIP_FIELDS = ["irq_ack", "irq_mask", "irq_unmask", "irq_eoi", "irq_enable",
               "irq_disable", "irq_retrigger", "irq_set_affinity", "irq_set_type", "irq_set_wake"]

def parse_functions(lines):
    """Heuristic top-level function splitter via brace depth."""
    funcs, i, n, depth, cur, opened = [], 0, len(lines), 0, None, False
    while i < n:
        line = lines[i]
        if cur is None and line and not line[0].isspace() and "(" in line \
           and not line.startswith("#") and not line.startswith("}") \
           and ";" not in line.split("(")[0] and "=" not in line.split("(")[0]:
            m = re.match(r"[\w\*][\w\s\*]*[\s\*]([\w]+)\s*\(", line)
            if m and ("{" in line or (i + 1 < n and lines[i + 1].strip().startswith("{"))):
                cur = {"name": m.group(1), "start": i}
        depth += line.count("{") - line.count("}")
        if cur is not None and (line.count("{") or "{" in line):
            opened = True
        if cur is not None and opened and depth <= 0:
            cur["end"] = i
            funcs.append(cur)
            cur, depth, opened = None, 0, False
        i += 1
    return funcs

def scan_file(path):
    try:
        with open(path, errors="replace") as fh:
            raw = fh.readlines()
    except OSError:
        return []
    lines = [re.sub(r"//.*", "", l.rstrip()) for l in raw]
    text = "\n".join(lines)
    funcs = parse_functions(lines)
    findings = []

    chip_ops = defaultdict(set)
    for m in re.finditer(r"struct\s+irq_chip\s+(\w+)\s*=\s*\{(.*?)\n\};", text, re.S):
        for fm in re.finditer(r"\.(\w+)\s*=\s*(\w+)", m.group(2)):
            if fm.group(1) in CHIP_FIELDS:
                chip_ops[fm.group(2)].add(fm.group(1))

    hard_handlers = set()
    for m in re.finditer(r"request_(?:irq|percpu_nmi|nmi)\s*\(([^;]*?IRQF_(?:NO_THREAD|PERCPU)[^;]*?)\);", text, re.S):
        args = re.sub(r"\s+", " ", m.group(1))
        parts = [p.strip() for p in args.split(",")]
        if len(parts) >= 2:
            hard_handlers.add(parts[1].split()[-1])
    for m in re.finditer(r"irq_set_chained_handler(?:_and_data)?\s*\(\s*[^,]+,\s*([A-Za-z_]\w*)", text):
        hard_handlers.add(m.group(1))

    hrt_fns = set(re.findall(r"\.function\s*=\s*([A-Za-z_]\w*)", text))

    by_name = defaultdict(list)
    for f in funcs:
        by_name[f["name"]].append(f)

    def check_ctx(fn, ctx, detail):
        body = lines[fn["start"]:fn["end"] + 1]
        atomic = 0
        for j, l in enumerate(body):
            if ATOMIC_ENTER.search(l):
                atomic += 1
            if ATOMIC_EXIT.search(l):
                atomic = max(0, atomic - 1)
            hard = ctx in ("irqchip", "hardirq", "hrtimer")
            lm = NONRAW_LOCK.search(l)
            if lm and (hard or atomic > 0):
                why = ("non-raw spinlock in %s context" % ctx) if hard \
                      else "non-raw spinlock inside raw/preempt-disabled region"
                findings.append({"file": path, "func": fn["name"], "line": fn["start"] + j + 1,
                                 "ctx": ctx, "what": lm.group(0), "why": why, "detail": detail})
            sm = SLEEP.search(l)
            if sm and (hard or atomic > 0):
                why = ("sleeping call in %s context" % ctx) if hard \
                      else "sleeping call inside raw/preempt-disabled region"
                findings.append({"file": path, "func": fn["name"], "line": fn["start"] + j + 1,
                                 "ctx": ctx, "what": sm.group(0), "why": why, "detail": detail})

    for name, fields in sorted(chip_ops.items()):
        for fn in by_name.get(name, []):
            check_ctx(fn, "irqchip", "irq_chip ops: %s" % ",".join(sorted(fields)))
    for name in sorted(hard_handlers):
        for fn in by_name.get(name, []):
            check_ctx(fn, "hardirq", "IRQF_NO_THREAD/PERCPU or chained handler")
    for name in sorted(hrt_fns):
        for fn in by_name.get(name, []):
            check_ctx(fn, "hrtimer", ".function callback (hrtimer?)")
    for fn in funcs:
        check_ctx(fn, "normal", "")
    return findings

def main():
    allf, files = [], 0
    for d in SCANDIRS:
        base = os.path.join(ROOT, d)
        for dirpath, _, filenames in os.walk(base):
            for fn in filenames:
                if fn.endswith(".c") and not fn.endswith(".mod.c"):
                    files += 1
                    allf.extend(scan_file(os.path.join(dirpath, fn)))
    seen, dedup = set(), []
    for f in allf:
        key = (f["file"], f["line"], f["why"])
        if key not in seen:
            seen.add(key)
            dedup.append(f)
    prio = {"irqchip": 0, "hardirq": 1, "hrtimer": 2, "normal": 3}
    dedup.sort(key=lambda f: (prio[f["ctx"]], f["file"], f["line"]))
    print("scanned %d .c files under %s" % (files, ", ".join(SCANDIRS)))
    print("findings: %d\n" % len(dedup))
    for f in dedup:
        rel = os.path.relpath(f["file"], ROOT)
        print("[%s] %s:%d %s(): %s -- %s (%s)" % (
            f["ctx"], rel, f["line"], f["func"], f["what"], f["why"], f["detail"]))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rt_scan_findings.json")
    with open(out, "w") as fh:
        json.dump(dedup, fh, indent=1)

if __name__ == "__main__":
    main()
