#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""JitterBuffer target 选型离线仿真（纯 Python，不进 core，不参与构建）。

用途：在改 TargetController 参数之前，先在离线模型里看 target 会落在哪、
欠载/丢帧预算是否守得住。模型口径见下，参数取自 aqua_core/doc/modules/
jitter_buffer.md「自适应 target」一节。

模型
  server  : capture block 480 帧 / 10ms，packet = 175 帧（默认档）-> 每个 callback
            吐 2/3/3 个包；AudioNetworkDispatcher::drain() 背靠背发完，
            所以串内到达间隔 ≈ 0（这是 jit_ms 的主要来源，不是网络）。
  client  : WASAPI 周期实测 512 帧 = 10.667ms，每次 pull 吃掉 2.844 个包。
  JB      : lead 以帧计；pull 时 lead < 512 即排空；水位带相对 target 的
            倍率 0.25 / 0.50 / 1.25 / 1.50（client_runtime 自适应路径口径）。
  相位    : 发包周期 10ms 与 callback 周期 10.667ms 的拍频约 160ms 扫过全部
            相位，因此必须扫相位取最坏值，只看一个相位会严重低估欠载。

用法
  python tools/sim_jb_target.py                 # 扫 k（默认场景表）
  python tools/sim_jb_target.py --k 6 --jit 3   # 指定 k 与附加网络抖动
"""
from __future__ import annotations

import argparse
import math
import random
from dataclasses import dataclass, replace

SR = 48000
PACKET = 175  # aligned with doc default (was 180) 帧/包
CAP_BLOCK = 480  # 帧 / capture callback
CAP_PERIOD_MS = 10.0
PULL_FRAMES = 512  # WASAPI 周期（实测 default=min=max=512）
PULL_PERIOD_MS = PULL_FRAMES * 1000.0 / SR
PKT_MS = PACKET * 1000.0 / SR

BAND_WL, BAND_NL, BAND_NH, BAND_WH = 0.25, 0.50, 1.25, 1.50


def burst_sizes() -> list[int]:
    """每个 capture callback 产生的包数（480/180 的余数累积 => 2,3,3 循环）。"""
    pending, out = 0, []
    for _ in range(3):
        pending += CAP_BLOCK
        n = pending // PACKET
        pending -= n * PACKET
        out.append(n)
    return out


@dataclass
class Est:
    """RFC 3550 A.8 J + 相对 transit + 累积最小 transit(base)。"""
    j_ms: float = 0.0
    base_ms: float = 0.0
    _ref_arr: float | None = None
    _ref_ts: float = 0.0
    _prev_arr: float | None = None
    _prev_ts: float = 0.0

    def push(self, arr_ms: float, ts_ms: float) -> None:
        if self._ref_arr is None:
            self._ref_arr, self._ref_ts = arr_ms, ts_ms
            self._prev_arr, self._prev_ts = arr_ms, ts_ms
        d = (arr_ms - self._prev_arr) - (ts_ms - self._prev_ts)
        self.j_ms += (abs(d) - self.j_ms) / 16.0
        self._prev_arr, self._prev_ts = arr_ms, ts_ms
        transit = (arr_ms - self._ref_arr) - (ts_ms - self._ref_ts)
        self.base_ms = min(self.base_ms, transit)


@dataclass
class CtlParams:
    capacity: int = 30
    min_target: int = 3
    initial: int = 4
    k: float = 5.0
    fall_rate: float = 1.0
    deadband: int = 0
    geometric_floor: int = 0  # ceil(pull_frames / packet)，几何地板（target 硬下限 - 1，对应 C++ geometric_floor_slots）
    use_base: bool = True  # base<0 时按 0 处理（与 C++ 实现一致）


class Ctl:
    def __init__(self, p: CtlParams) -> None:
        self.p = p
        self.cur = max(p.min_target, p.geometric_floor + 1, min(p.initial, p.capacity))
        self.carry = 0.0
        self.last_t: float | None = None

    def update(self, est: Est, t_ms: float) -> int:
        p = self.p
        base = (est.base_ms / PKT_MS) if (p.use_base and est.base_ms > 0) else 0.0
        margin = p.k * est.j_ms / PKT_MS
        floor = max(p.min_target, p.geometric_floor + 1)
        desired = int(min(max(math.ceil(base + margin), floor), p.capacity))
        if desired > self.cur + p.deadband:
            self.cur, self.carry = desired, 0.0
        elif desired < self.cur:
            if self.last_t is None or t_ms <= self.last_t:
                self.cur, self.carry = desired, 0.0
            else:
                self.carry += (t_ms - self.last_t) / 1000.0 * p.fall_rate
                step = int(min(self.carry, self.cur - desired))
                self.cur -= step
                self.carry = 0.0 if self.cur == desired else self.carry - step
        else:
            self.carry = 0.0
        self.last_t = t_ms
        return self.cur


@dataclass
class Result:
    target: int
    und: float
    drop: float
    max_run_ms: float
    ev_s: float
    lead_p05: float
    lead_p50: float
    lead_p95: float
    j_ms: float
    base_ms: float


def run(p: CtlParams, seconds: float = 40.0, phase_ms: float = 0.0,
        net_jitter_ms: float = 0.0, loss: float = 0.0,
        drift_ppm: float = 0.0, seed: int = 7, warm_ms: float = 2000.0) -> Result:
    rng = random.Random(seed)
    ev: list[tuple[float, int, float]] = []  # (t_ms, 0=arr/1=pull, ts_ms)
    t, i, ts_ms, sizes = 0.0, 0, 0.0, burst_sizes()
    while t < seconds * 1000.0:
        for _ in range(sizes[i % len(sizes)]):
            if rng.random() >= loss:
                jit = rng.gauss(0.0, net_jitter_ms) if net_jitter_ms else 0.0
                ev.append((t + jit, 0, ts_ms))
            ts_ms += PKT_MS
        i += 1
        t += CAP_PERIOD_MS
    tp = phase_ms
    while tp < seconds * 1000.0:
        ev.append((tp, 1, 0.0))
        tp += PULL_PERIOD_MS * (1.0 - drift_ppm * 1e-6)
    ev.sort(key=lambda e: (e[0], e[1]))  # 同刻先到达后消费

    est, ctl = Est(), Ctl(p)
    target = ctl.cur
    lead = target * PACKET
    und = drop = pulled = ev_cnt = run_ = max_run = 0
    samples: list[float] = []
    up = down = False

    for tm, kind, ts in ev:
        if kind == 0:
            est.push(tm, ts)
            target = ctl.update(est, tm)
            lead += PACKET
            continue
        need = PULL_FRAMES
        pulled += need
        ok = tm > warm_ms
        if up:  # Fill：停住等 lead 回到 target
            if lead >= target * PACKET:
                up = False
            else:
                if ok:
                    und += need;
                    run_ += need;
                    ev_cnt += 1
                    max_run = max(max_run, run_)
                continue
        if down:
            if lead <= target * PACKET:
                down = False
            else:
                step = min(need, max(PACKET, lead - target * PACKET))
                lead -= step
                drop += step
        nl = max(1, round(target * BAND_NL * PACKET))
        nh = max(nl + 1, round(target * BAND_NH * PACKET))
        wh = max(nh + 1, round(target * BAND_WH * PACKET))
        if lead < nl:
            up = True
            if ok:
                und += need;
                run_ += need;
                ev_cnt += 1
                max_run = max(max_run, run_)
            continue
        if lead > nh:
            down = True
            if lead > wh:
                step = min(lead - target * PACKET, lead)
                lead -= step
                drop += step
        if lead >= need:
            lead -= need
            run_ = 0
        else:
            if ok:
                und += need;
                run_ += need;
                ev_cnt += 1
                max_run = max(max_run, run_)
            lead = 0
        samples.append(lead / PACKET)

    samples.sort()
    q = lambda f: samples[min(len(samples) - 1, int(len(samples) * f))] if samples else 0.0
    return Result(target, und / max(1, pulled), drop / max(1, pulled + drop),
                  max_run * 1000.0 / SR, ev_cnt / max(0.001, seconds - warm_ms / 1000.0),
                  q(0.05), q(0.50), q(0.95), est.j_ms, est.base_ms)


def worst(p: CtlParams, phases: int = 16, **kw) -> Result:
    rs = [run(replace(p), phase_ms=x * 0.7, **kw) for x in range(phases)]
    return max(rs, key=lambda r: r.und)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--k", type=float, default=None, help="只跑指定 k，否则扫 4..7")
    ap.add_argument("--jit", type=float, default=None, help="只看指定附加网络抖动(ms)")
    args = ap.parse_args()

    grant = -(-PULL_FRAMES // PACKET)
    print(f"packet={PKT_MS:.3f}ms pull={PULL_PERIOD_MS:.3f}ms burst={burst_sizes()} "
          f"geometric_floor={grant} -> 几何地板 {grant + 1} slots ({(grant + 1) * PKT_MS:.1f}ms)")

    ks = [args.k] if args.k else [4.0, 5.0, 6.0, 7.0]
    conds = [("理想有线", 0.0, 0.0), ("+1ms抖动", 1.0, 0.0),
             ("+3ms抖动(WiFi)", 3.0, 0.0), ("+1%丢包", 1.0, 0.01)]
    if args.jit is not None:
        conds = [(f"+{args.jit}ms抖动", args.jit, 0.0)]

    print("\n每格 = 最坏相位下的 [target / 欠载% / 最大单次断流ms]（欠载预算 0.1%）")
    print("%-5s" % "k" + "".join("%-24s" % c[0] for c in conds))
    for k in ks:
        row = "%-5.1f" % k
        for _, jit, loss in conds:
            w = worst(CtlParams(k=k, min_target=3, initial=4, geometric_floor=grant),
                      net_jitter_ms=jit, loss=loss)
            flag = "OK " if w.und < 0.001 else "!! "
            row += "%-24s" % (flag + "T=%d %.3f%% %.0fms" % (w.target, w.und * 100, w.max_run_ms))
        print(row)


if __name__ == "__main__":
    main()
