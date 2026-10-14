#!/usr/bin/env python3
"""EtherCAT 장치 rule 기반 PDO 매핑 엔진 (프로토타입, rule v2).

덤프 JSON(ecsdo 덤프 명세)을 읽어 장치 형태에 맞는 rule을 고르고,
매핑 요청(mode, extras)에 따라 매핑 계획(plan)을 JSON으로 출력한다. 버스에는 아무것도 쓰지 않는다.
매핑을 바꿔야 하면 ecsdo로 실행할 SDO 쓰기 순서(sdo_writes)를 계획에 담는다.

매핑 결정 순서 (rules/README.md)
  1. 요청 확인. 없으면 rule의 default
  2. (CiA402) 0x6502에서 요청 모드 지원 확인
  3. 필수 항목이 장치에 모두 있는지 확인. 없으면 실패 → LLM
  4. 요청한 extras 중 장치에 있는 객체만 선택. 없는 객체는 보고
  5. CoE Details로 매핑 방법 결정 (remap / select_fixed / keep_current)
  6. 출력: 매핑 계획 + on_event SDO 조회 목록 + 빠진 항목 보고

결과 status
  matched          rule 적용 성공
  fallback         맞는 rule이 없음 → 기본 PDO 유지 + 일반 라벨 (LLM 대상)
  failed           rule은 있으나 적용 불가 (필수 객체 없음, 모드 미지원, 고정 PDO 등)
  no_process_data  인프라 장치 (커플러 등)
"""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

import yaml

RULE_DIR = Path(__file__).resolve().parent / "rules"

# rule이나 모델 rule에 remap이 없을 때 쓰는 PDO 번호
DEFAULT_REMAP = {"rx_pdo": 0x1600, "tx_pdo": 0x1A00, "axis_step": 0x10}

# 매핑 요청(mode, extras)을 받는 rule
PROFILE_RULES = ("cia402", "mdp5001", "cia401", "non_coe_io")


# ---------------------------------------------------------------- 공통 유틸

def to_int(v):
    """정수 또는 '0x..' 문자열을 정수로 바꾼다. None은 그대로."""
    if v is None or isinstance(v, bool):
        return v
    if isinstance(v, int):
        return v
    s = str(v).strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def hx(v, width=4):
    return None if v is None else f"0x{v:0{width}X}"


def as_list(v):
    return v if isinstance(v, list) else [v]


def deep_merge(base: dict, extra: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in extra.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def load_rules(rule_dir: Path = RULE_DIR) -> dict:
    """rule 파일을 {파일 이름: 내용}으로 읽는다. models/*.yaml은 rules['models'] 목록에 담는다.
    이름이 '_'로 시작하는 모델 파일(예시)과 승인 전 LLM 후보(source.approved: false)는 읽지 않는다."""
    rules: dict = {}
    for p in sorted(rule_dir.glob("*.yaml")):
        rules[p.stem] = yaml.safe_load(p.read_text(encoding="utf-8")) or {}
    models = []
    for p in sorted((rule_dir / "models").glob("*.yaml")):
        if p.name.startswith("_"):
            continue
        m = yaml.safe_load(p.read_text(encoding="utf-8")) or {}
        if (m.get("source") or {}).get("approved") is False:
            print(f"warning: {p.name}: 승인 전 후보(source.approved: false)라 읽지 않음", file=sys.stderr)
            continue
        m["_file"] = p.name
        models.append(m)
    rules["models"] = models
    return rules


def normalize_dump(raw: dict) -> dict:
    """device_probe 출력을 엔진 내부 형식으로 바꾼다. 이미 내부 형식이면 그대로 둔다.

    device_probe: 평평한 키("sii.mailbox", "od.rxpdo"), od.rxpdo = {basis, pdos},
                  sii.pdo = {rxpdo, txpdo}, 읽지 못한 목록은 {"unavailable": 이유}
    내부 형식:     dump["sii"]["mailbox"], od.rxpdo = PDO 목록, sii.pdo = {rx, tx}, 읽지 못한 목록은 None
    """
    if not any("." in k for k in raw):
        return raw
    d: dict = {"sii": {}, "od": {}}
    for k, v in raw.items():
        if "." in k:
            top, sub = k.split(".", 1)
            d.setdefault(top, {})[sub] = v
        else:
            d[k] = v
    sii, od = d["sii"], d["od"]
    for key in ("rxpdo", "txpdo"):
        if isinstance(od.get(key), dict):
            od[key] = od[key].get("pdos") or []
    sp = sii.get("pdo")
    if isinstance(sp, dict):
        sii["pdo"] = {} if "unavailable" in sp else {"rx": sp.get("rxpdo"), "tx": sp.get("txpdo")}
    for key in ("profile_axis0", "profile_axes_extra", "vendor", "objects"):
        if isinstance(od.get(key), dict) and "unavailable" in od[key]:
            od[key] = None
    for a in od.get("profile_axes_extra") or []:
        a.setdefault("count", a.get("objects"))
    return d


def norm_pdos(lst, assigned_only: bool = False) -> list[dict]:
    """PDO 목록을 {index, name, assigned, entries:[{index, subindex, bit_len, name, object_name, data_type}]}로 정규화.
    엔트리 name은 엔트리 이름(device_probe의 entry_name)이 있으면 그것, 없으면 객체 이름이다.
    assigned: SII PDO 중 기본으로 할당된 것("sm"이 null이 아님). sm 키가 없으면 할당된 것으로 본다."""
    if not isinstance(lst, list):
        return []
    out = []
    for p in lst:
        assigned = p.get("sm", 0) is not None
        if assigned_only and not assigned:
            continue
        ents = []
        for e in p.get("entries", []):
            ents.append({
                "index": 0 if e.get("gap") else (to_int(e.get("index")) or 0),
                "subindex": to_int(e.get("subindex")) or 0,
                "bit_len": to_int(e.get("bit_len")) or 0,
                "name": e.get("entry_name") or e.get("name") or "",
                "object_name": e.get("name") or "",
                "data_type": e.get("data_type"),
            })
        out.append({"index": to_int(p.get("index")), "name": p.get("name"), "assigned": assigned, "entries": ents})
    return out


def sii_pdos(dump: dict, assigned_only: bool = True):
    """SII PDO. assigned_only면 기본 할당된 것만 (= 장치의 기본 매핑), 아니면 고를 수 있는 세트 전부."""
    sp = (dump.get("sii") or {}).get("pdo") or {}
    return norm_pdos(sp.get("rx"), assigned_only), norm_pdos(sp.get("tx"), assigned_only)


def current_pdos(dump: dict, prefer_sii: bool = False):
    """현재(또는 기본) PDO를 가져온다. 방향마다 OD에서 읽었으면 OD, 못 읽었으면 SII 기본 PDO.
    prefer_sii면 SII PDO를 먼저 본다 (CoE 미지원 장치). 출처: od / sii / od+sii / None."""
    od = dump.get("od") or {}
    srx, stx = sii_pdos(dump)
    if prefer_sii and (srx or stx):
        return srx, stx, "sii"
    out, srcs = [], []
    for key, sii_list in (("rxpdo", srx), ("txpdo", stx)):
        if isinstance(od.get(key), list):
            out.append(norm_pdos(od[key]))
            srcs.append("od")
        else:
            out.append(sii_list)
            if sii_list:
                srcs.append("sii")
    src = "+".join(dict.fromkeys(srcs)) or None
    return out[0], out[1], src


def layout(pdos: list[dict], roles: dict) -> tuple[list[dict], int]:
    """PDO 순서대로 비트 오프셋을 계산한다. roles: {(index, sub): role}."""
    rows, bit = [], 0
    for p in pdos:
        for e in p["entries"]:
            rows.append({
                "pdo": hx(p["index"]),
                "index": hx(e["index"]),
                "subindex": e["subindex"],
                "bit_len": e["bit_len"],
                "bit_offset": bit,
                "name": e["name"],
                "role": roles.get((e["index"], e["subindex"])),
            })
            bit += e["bit_len"]
    return rows, bit


def identity(dump: dict) -> dict:
    sii = dump.get("sii") or {}
    od = dump.get("od") or {}
    ident = sii.get("identity") or {}
    strings = sii.get("strings") or {}
    return {
        "vendor_id": to_int(ident.get("vendor_id")),
        "product_code": to_int(ident.get("product_code")),
        "device_name": (od.get("name_ver") or {}).get("device_name"),   # 0x1008
        "sii_name": strings.get("name"),
        "sii_order": strings.get("order"),
    }


def has_coe(dump: dict) -> bool:
    mailbox = (dump.get("sii") or {}).get("mailbox")
    if isinstance(mailbox, list):
        return "CoE" in mailbox
    return dump.get("branch") != "non_coe"


def new_plan(dump: dict) -> dict:
    ident = identity(dump)
    return {
        "position": dump.get("position"),
        "branch": dump.get("branch"),
        "identity": {
            "vendor_id": hx(ident["vendor_id"], 8),
            "product_code": hx(ident["product_code"], 8),
            "device_name": ident["device_name"] or ident["sii_name"],
        },
        "rule": None,
        "model_rule": None,
        "request": None,
        "status": None,
        "device_class": None,
        "count": None,
        "action": None,          # keep_current | keep_sii | remap | select_fixed | none
        "pdo_source": None,      # od | sii | rule
        "rx": [], "tx": [],
        "rx_bits": 0, "tx_bits": 0,
        "missing_required": [],
        "missing_extras": [],
        "on_event": [],
        "sdo_writes": [],
        "needs_llm": False,
        "notes": [],
    }


# ---------------------------------------------------------------- rule 선택, 모델 rule, 요청

def select_rule(dump: dict) -> str | None:
    branch = dump.get("branch")
    profile = to_int(((dump.get("od") or {}).get("device_type") or {}).get("profile"))
    if branch == "infrastructure":
        return "infrastructure"
    if branch == "cia402" or profile == 402:
        return "cia402"
    if branch == "mdp" or profile == 5001:
        return "mdp5001"
    if profile == 401:
        return "cia401"
    rx, tx, _ = current_pdos(dump)
    if not rx and not tx:
        return "infrastructure"
    if not has_coe(dump):
        return "non_coe_io"
    return None


def find_model(dump: dict, rules: dict, rule_id: str | None, notes: list) -> dict | None:
    """Vendor ID + 0x1008(없으면 SII 주문번호)로 모델 rule을 찾는다.
    base_rule이 선택된 rule과 같은 모델만 쓴다. base_rule이 없는 모델은 프로파일 rule이 없는 장치용이다."""
    ident = identity(dump)
    name = ident["device_name"] or ident["sii_order"]
    for m in rules.get("models") or []:
        mt = m.get("match") or {}
        if to_int(mt.get("vendor_id")) != ident["vendor_id"]:
            continue
        if mt.get("device_name") and mt["device_name"] != name:
            continue
        if mt.get("product_code") is not None and to_int(mt["product_code"]) != ident["product_code"]:
            continue
        if m.get("base_rule") != rule_id:
            notes.append(f"모델 rule {m.get('_file')}의 base_rule({m.get('base_rule')})이 "
                         f"선택된 rule({rule_id})과 다름 → 무시")
            continue
        return m
    return None


def rule_with_model(rules: dict, rule_id: str, dump: dict, plan: dict) -> dict:
    rule = rules[rule_id]
    m = find_model(dump, rules, rule_id, plan["notes"])
    if m:
        plan["model_rule"] = m.get("_file")
        rule = deep_merge(rule, {k: v for k, v in m.items() if k not in ("match", "base_rule", "_file")})
    return rule


def resolve_extras(rule: dict, extras, plan: dict) -> list[str]:
    """요청 extras를 정한다. 없으면 default.extras. rule에 없는 이름은 빼고 보고한다."""
    default = rule.get("default") or {}
    names = list(default.get("extras") or []) if extras is None else list(extras)
    known = rule.get("extras") or {}
    unknown = [n for n in names if n not in known]
    if unknown:
        plan["notes"].append(f"rule에 없는 extras는 무시: {unknown} (선택지: {list(known)})")
        for n in unknown:
            plan["missing_extras"].append({"extra": n, "reason": "rule에 없음"})
    return [n for n in names if n in known]


def on_event_rows(rule: dict, extras: list[str], axes: int = 1, offset: int = 0) -> list[dict]:
    """요청한 extras의 on_event 객체 (PDO에 넣지 않고 필요할 때 SDO로 읽음)."""
    rows = []
    for name in extras:
        for d in (rule["extras"][name].get("on_event") or []):
            if "index_range" in d:
                lo, hi = (to_int(v) for v in d["index_range"])
                rows.append({"extra": name, "role": d["role"], "index_range": [hx(lo), hx(hi)]})
                continue
            for ax in range(axes):
                o = rule_obj(d, ax, offset)
                if ax and o["index"] == o["base"]:
                    break          # 축별 객체가 아니면 한 번만
                row = {"extra": name, "role": label(o, axes), "index": hx(o["index"])}
                if "sub" in d:
                    row["subindex"] = o["sub"]
                for k in ("bits", "type", "note"):
                    if d.get(k) is not None:
                        row[k] = d[k]
                rows.append(row)
    return rows


# ---------------------------------------------------------------- CiA402

def rule_obj(d: dict, axis: int = 0, offset: int = 0) -> dict:
    """rule 항목을 정규화한다. 프로파일 객체(0x6000~0x67FF)는 축 k에서 k * axis_offset만큼 옮긴다."""
    base = to_int(d["index"])
    idx = base + axis * offset if axis and 0x6000 <= base < 0x6800 else base
    return {"role": d["role"], "index": idx, "base": base, "sub": to_int(d.get("sub")) or 0,
            "bits": to_int(d.get("bits")), "axis": axis}


def label(o: dict, axes: int) -> str:
    per_axis = axes > 1 and 0x6000 <= o["base"] < 0x6800
    return o["role"] + (f"[{o['axis']}]" if per_axis else "")


def apply_cia402(dump: dict, rules: dict, plan: dict, mode: str | None, extras) -> dict:
    notes = plan["notes"]
    rule = rule_with_model(rules, "cia402", dump, plan)
    mode = mode or (rule.get("default") or {}).get("mode")
    extras = resolve_extras(rule, extras, plan)
    plan["request"] = {"mode": mode, "extras": extras}
    plan["rule"] = f"cia402/{mode}"
    od = dump.get("od") or {}

    # 축 수 (모드 확인보다 먼저: 실패해도 장치 형태와 축 수는 남긴다)
    extra_axes = od.get("profile_axes_extra") or []
    axes = 1 + sum(1 for a in extra_axes if isinstance(a, dict) and (to_int(a.get("count")) or 0) > 0)
    plan.update(device_class="servo_drive", count=axes)

    if mode not in rule["modes"]:
        plan.update(status="failed", action="none")
        notes.append(f"rule에 없는 모드: {mode} (선택지: {list(rule['modes'])})")
        return plan
    m = rule["modes"][mode]

    # 2) 모드 지원 확인 (0x6502)
    supported = (od.get("drive_modes") or {}).get("modes")
    if isinstance(supported, list) and m["drive_mode_bit"] not in supported:
        plan.update(status="failed", action="none")
        notes.append(f"0x6502에 {m['drive_mode_bit']} 모드가 없음 (지원: {supported})")
        return plan
    if not isinstance(supported, list):
        notes.append("0x6502를 읽지 못해 모드 지원 여부를 확인하지 못함")

    # 3) 필수 항목 존재 확인
    prof = od.get("profile_axis0")
    known = {to_int(o.get("index")) for o in prof} if isinstance(prof, list) else None
    if known is None:
        notes.append("OD 목록(SDO Info) 없음: PDO에 없는 객체의 존재 여부를 확인하지 못함")
    rx, tx, src = current_pdos(dump)
    plan["pdo_source"] = src
    mapped = {(e["index"], e["subindex"]) for p in rx + tx for e in p["entries"]}
    offset = to_int(rule.get("axis_offset")) or 0

    def exists(o: dict):
        if (o["index"], o["sub"]) in mapped:
            return True
        if known is None:
            return None
        # 축 1 이상은 덤프에 개수만 있으므로 축 0 기준으로 추정
        return o["base"] in known

    sel = []      # 매핑할 객체 (필수 → extras 순)
    for ax in range(axes):
        for direction in ("rx", "tx"):
            for d in m.get(direction) or []:
                sel.append({**rule_obj(d, ax, offset), "dir": direction, "required": True})

    missing = [o for o in sel if exists(o) is False]
    plan["missing_required"] = [f"{label(o, axes)} {hx(o['index'])}" for o in missing]
    if missing:
        plan.update(status="failed", action="none", needs_llm=True)
        notes.append("필수 객체가 없어 rule 적용 불가 → LLM 경로로 보냄")
        return plan
    unknown = [label(o, axes) for o in sel if exists(o) is None]
    if unknown:
        notes.append(f"존재를 확인하지 못한 필수 객체: {unknown}")
    if axes > 1:
        notes.append("축 1 이상의 객체 존재 여부는 축 0 기준으로 추정함")

    # 4) 요청한 extras 중 장치에 있는 객체만 선택
    seen = {(o["index"], o["sub"]) for o in sel}
    for name in extras:
        x = rule["extras"][name]
        for ax in range(axes):
            for direction in ("rx", "tx"):
                for d in x.get(direction) or []:
                    o = {**rule_obj(d, ax, offset), "dir": direction, "required": False}
                    key = (o["index"], o["sub"])
                    if key in seen:
                        continue
                    ex = exists(o)
                    if ex:
                        sel.append(o)
                        seen.add(key)
                    else:
                        plan["missing_extras"].append({
                            "extra": name, "role": label(o, axes), "index": hx(o["index"]),
                            "reason": "장치에 없음" if ex is False else "존재 확인 불가 (OD 목록 없음)"})
    plan["on_event"] = on_event_rows(rule, extras, axes, offset)
    roles = {(o["index"], o["sub"]): label(o, axes) for o in sel}

    # 5) CoE Details로 매핑 방법 결정
    caps = (dump.get("sii") or {}).get("coe_details") or {}
    cfg, asg = caps.get("pdo_config"), caps.get("pdo_assign")
    if cfg is True:
        method = "remap"
    elif asg is True:
        method = "select_fixed"
    elif cfg is False and asg is False:
        method = "keep_current"
    else:
        all_mapped = all((o["index"], o["sub"]) in mapped for o in sel)
        method = "keep_current" if all_mapped else "remap"
        notes.append("CoE Details(PDO 변경 가능 여부)가 덤프에 없음: "
                     + ("현재 매핑에 선택 객체가 모두 있어 그대로 씀" if all_mapped
                        else "변경 가능하다고 가정하고 remap. 실제 쓰기 전에 확인 필요"))

    if method == "keep_current":
        not_mapped = [o for o in sel if (o["index"], o["sub"]) not in mapped]
        req_nm = [o for o in not_mapped if o["required"]]
        if req_nm:
            plan["missing_required"] = [f"{label(o, axes)} {hx(o['index'])} (현재 PDO에 없음)" for o in req_nm]
            plan.update(status="failed", action="none")
            notes.append("PDO 변경 불가 장치인데 현재 매핑에 필수 객체가 없음")
            return plan
        for o in not_mapped:
            plan["missing_extras"].append({"extra": None, "role": label(o, axes), "index": hx(o["index"]),
                                           "reason": "현재 PDO에 없음 (PDO 변경 불가)"})
        new_rx, new_tx = rx, tx
    elif method == "select_fixed":
        chosen = select_fixed_sets(dump, sel, axes, plan)
        if chosen is None:
            plan.update(status="failed", action="none")
            return plan
        new_rx, new_tx = chosen
        plan["pdo_source"] = "sii"
        plan["sdo_writes"] = assign_writes(new_rx, new_tx)
    else:
        remap = {**DEFAULT_REMAP, **{k: to_int(v) for k, v in (rule.get("remap") or {}).items()}}
        new_rx, new_tx = [], []
        for ax in range(axes):
            for direction, base, out in (("rx", remap["rx_pdo"], new_rx), ("tx", remap["tx_pdo"], new_tx)):
                ents = [{"index": o["index"], "subindex": o["sub"], "bit_len": o["bits"], "name": label(o, axes)}
                        for o in sel if o["axis"] == ax and o["dir"] == direction]
                out.append({"index": base + ax * remap["axis_step"], "entries": ents})
        if axes > 1 and not rule.get("remap"):
            notes.append("다축 PDO 번호(axis_step)는 벤더마다 다름 → models/에 모델 rule로 지정")
        plan["pdo_source"] = "rule"
        plan["sdo_writes"] = remap_writes(new_rx, new_tx)

    plan["action"] = method
    plan["rx"], plan["rx_bits"] = layout(new_rx, roles)
    plan["tx"], plan["tx_bits"] = layout(new_tx, roles)

    # 운전 모드 설정 (init_sdo): PDO에 0x6060이 없으면 초기화 때 SDO로 한 번 쓴다
    final = {(e["index"], e["subindex"]) for p in new_rx + new_tx for e in p["entries"]}
    for item in rule.get("init_sdo") or []:
        value = m.get(item["value"], item["value"]) if isinstance(item.get("value"), str) else item.get("value")
        for ax in range(axes):
            o = rule_obj({"role": "init", **item}, ax, offset)
            if (o["index"], o["sub"]) in final:
                notes.append(f"{hx(o['index'])}이 PDO에 있으므로 주기 데이터로 값 {value}를 써야 함")
                continue
            plan["sdo_writes"].append({"index": hx(o["index"]), "subindex": o["sub"], "value": value,
                                       "bits": 8, "note": f"{item.get('note', '')} {mode.upper()}".strip()})
    plan["status"] = "matched"
    return plan


def select_fixed_sets(dump: dict, sel: list[dict], axes: int, plan: dict):
    """PDO 할당만 바꿀 수 있는 장치: 필요한 객체를 포함하는 고정 PDO 세트를 SII PDO 목록에서 고른다."""
    notes = plan["notes"]
    cand_rx, cand_tx = sii_pdos(dump, assigned_only=False)
    if not cand_rx and not cand_tx:
        notes.append("PDO 할당만 변경 가능한데 고정 PDO 세트 목록(sii.pdo)이 덤프에 없음")
        return None
    def keys(p):
        return {(e["index"], e["subindex"]) for e in p["entries"]}

    chosen = {"rx": [], "tx": []}
    for direction, cands in (("rx", cand_rx), ("tx", cand_tx)):
        got = chosen[direction]
        have: set = set()
        # 필수 먼저, 그다음 extras: 남은 객체를 가장 많이 덮는 세트부터 고른다 (이미 고른 세트와 겹치는 세트는 제외)
        for required in (True, False):
            need = [(o["index"], o["sub"]) for o in sel if o["dir"] == direction and o["required"] == required]
            while True:
                rest = set(need) - have
                pool = [p for p in cands if p not in got and not (keys(p) & have) and keys(p) & rest]
                if not rest or not pool:
                    break
                best = max(pool, key=lambda p: (len(keys(p) & rest), -len(p["entries"])))
                got.append(best)
                have |= keys(best)
        for o in (o for o in sel if o["dir"] == direction and (o["index"], o["sub"]) not in have):
            if o["required"]:
                plan["missing_required"].append(f"{label(o, axes)} {hx(o['index'])} (고정 PDO 세트에 없음)")
            else:
                plan["missing_extras"].append({"extra": None, "role": label(o, axes), "index": hx(o["index"]),
                                               "reason": "고정 PDO 세트에 없음"})
    if plan["missing_required"]:
        notes.append("필수 객체를 포함하는 고정 PDO 세트가 없음")
        return None
    notes.append("고정 PDO 세트 사이의 배타 조건(SII Exclude)은 확인하지 않음")
    return chosen["rx"], chosen["tx"]


def sdo_write(index, sub, value, bits, note=""):
    return {"index": hx(index), "subindex": sub,
            "value": hx(value, 8) if bits == 32 else value, "bits": bits, "note": note}


def assign_writes(new_rx: list[dict], new_tx: list[dict]) -> list[dict]:
    """PRE-OP에서 실행할 PDO 할당 쓰기 순서 (0x1C12/0x1C13)."""
    w = []
    for assign, pdos in ((0x1C12, new_rx), (0x1C13, new_tx)):
        w.append(sdo_write(assign, 0, 0, 8, "PDO 할당 해제"))
        for i, p in enumerate(pdos, 1):
            w.append(sdo_write(assign, i, p["index"], 16, "PDO 할당"))
        w.append(sdo_write(assign, 0, len(pdos), 8, "할당 수"))
    return w


def remap_writes(new_rx: list[dict], new_tx: list[dict]) -> list[dict]:
    """PRE-OP에서 실행할 매핑 + 할당 쓰기 순서 (ecsdo download로 실행)."""
    w = []
    for assign, pdos in ((0x1C12, new_rx), (0x1C13, new_tx)):
        w.append(sdo_write(assign, 0, 0, 8, "PDO 할당 해제"))
        for p in pdos:
            w.append(sdo_write(p["index"], 0, 0, 8, "매핑 비우기"))
            for i, e in enumerate(p["entries"], 1):
                w.append(sdo_write(p["index"], i, (e["index"] << 16) | (e["subindex"] << 8) | e["bit_len"], 32, e["name"]))
            w.append(sdo_write(p["index"], 0, len(p["entries"]), 8, "엔트리 수"))
        for i, p in enumerate(pdos, 1):
            w.append(sdo_write(assign, i, p["index"], 16, "PDO 할당"))
        w.append(sdo_write(assign, 0, len(pdos), 8, "할당 수"))
    return w


# ---------------------------------------------------------------- I/O (MDP, 비CoE, CiA401)

def match_pattern(patterns: list[dict], direction: str, e: dict) -> str | None:
    name = f"{e['object_name']} {e['name']}".lower()
    for p in patterns:
        if p["dir"] != direction or e["bit_len"] not in as_list(p["bits"]):
            continue
        if p.get("name_any") and not any(k.lower() in name for k in p["name_any"]):
            continue
        return p["role"]
    return None


def apply_io(dump: dict, rules: dict, plan: dict, rule_id: str, extras, model: dict | None = None) -> dict:
    """I/O rule(mdp5001, non_coe_io, cia401) 적용. model이 있으면 프로파일 rule 없이 그 모델 rule만 쓴다."""
    notes = plan["notes"]
    if model is None:
        rule = rule_with_model(rules, rule_id, dump, plan)
    else:
        rule = model
        plan["model_rule"] = model.get("_file")
    extras = resolve_extras(rule, extras, plan)
    plan["request"] = {"extras": extras}
    plan["rule"] = rule_id
    pdo_policy = (rule.get("default") or {}).get("pdo", "keep_current")
    rx, tx, src = current_pdos(dump, prefer_sii=(pdo_policy == "keep_sii"))
    plan["pdo_source"] = src
    if not rx and not tx:
        plan.update(status="failed", action="none", needs_llm=True)
        notes.append("PDO 정보가 덤프에 없음 (mdp 분기는 덤프에 PDO를 포함해야 함)")
        return plan

    req = rule.get("required") or {}
    status_ext = (rule.get("extras") or {}).get("channel_status") or {}
    status_kw = [k.lower() for k in status_ext.get("name_any") or []]
    want_status = "channel_status" in extras

    roles: dict = {}
    counts: dict = {}
    channel_map = []
    unclassified = 0
    status_skipped = 0

    def push(role, idx, sub, bit_len, bit_in_group=None):
        ch = counts.get(role, 0)
        counts[role] = ch + 1
        channel_map.append({"role": role, "channel": ch, "index": hx(idx), "subindex": sub,
                            "bit_len": bit_len, **({"bit": bit_in_group} if bit_in_group is not None else {})})
        return f"{role}[{ch}]"

    if "channel_patterns" not in req:
        # 객체 index로 찾는 rule (cia401, 모델 rule). sub가 있으면 sub까지 맞춘다.
        objs = {}
        for direction in ("tx", "rx"):
            for o in req.get(direction) or []:
                objs[(direction, to_int(o["index"]), to_int(o.get("sub")))] = o
        for direction, pdos in (("tx", tx), ("rx", rx)):
            for p in pdos:
                for e in p["entries"]:
                    if e["index"] == 0:
                        continue
                    o = objs.get((direction, e["index"], e["subindex"])) or objs.get((direction, e["index"], None))
                    if not o:
                        unclassified += 1
                        continue
                    if o.get("group_bits"):
                        labels = [push(o["role"], e["index"], e["subindex"], 1, b) for b in range(e["bit_len"])]
                        roles[(e["index"], e["subindex"])] = f"{labels[0]}..{labels[-1]}"
                    else:
                        roles[(e["index"], e["subindex"])] = push(o["role"], e["index"], e["subindex"], e["bit_len"])
    else:
        patterns = req.get("channel_patterns") or []
        for direction, pdos, rng in (("tx", tx, req.get("input_range")), ("rx", rx, req.get("output_range"))):
            lo, hi = (to_int(v) for v in rng) if rng else (None, None)
            for p in pdos:
                for e in p["entries"]:
                    if e["index"] == 0:          # 패딩
                        continue
                    key = (e["index"], e["subindex"])
                    if lo is not None and not (lo <= e["index"] <= hi):
                        notes.append(f"{hx(e['index'])}가 {direction} 범위({hx(lo)}~{hx(hi)}) 밖에 있음")
                    # 상태 비트는 요청과 관계없이 채널 패턴에서 뺀다 (1비트 상태를 디지털 채널로 오인하지 않게)
                    if status_kw and any(k in e["name"].lower() for k in status_kw):
                        if want_status:
                            roles[key] = push("channel_status", e["index"], e["subindex"], e["bit_len"])
                        else:
                            status_skipped += 1
                        continue
                    role = match_pattern(patterns, direction, e)
                    if role:
                        roles[key] = push(role, e["index"], e["subindex"], e["bit_len"])
                    else:
                        unclassified += 1
        if status_skipped:
            notes.append(f"상태 비트 {status_skipped}개는 channel_status 미요청이라 역할을 붙이지 않음")

    kinds = set(counts) - {"channel_status"}
    cls = None
    if kinds == {"digital_input"}:
        cls = "digital_input"
    elif kinds == {"digital_output"}:
        cls = "digital_output"
    elif kinds == {"digital_input", "digital_output"}:
        cls = "digital_io"
    elif kinds == {"analog_input"}:
        cls = "analog_input"
    elif kinds == {"analog_output"}:
        cls = "analog_output"
    elif kinds:
        cls = "mixed_io"

    if rule.get("device_class") and kinds:
        cls = rule["device_class"]
    if cls is None:
        return apply_fallback(dump, plan, reason=f"{rule_id} 채널 패턴으로 분류할 엔트리가 없음")

    plan.update(device_class=cls, count=sum(counts[k] for k in kinds), status="matched",
                action="keep_sii" if pdo_policy == "keep_sii" else "keep_current")
    plan["rx"], plan["rx_bits"] = layout(rx, roles)
    plan["tx"], plan["tx_bits"] = layout(tx, roles)
    plan["channels"] = channel_map
    plan["on_event"] = on_event_rows(rule, extras)
    if unclassified:
        plan["needs_llm"] = True
        notes.append(f"역할을 정하지 못한 엔트리 {unclassified}개 → LLM으로 보완")
    return plan


# ---------------------------------------------------------------- 인프라, rule 없음

def apply_infrastructure(dump: dict, rules: dict, plan: dict) -> dict:
    rule = rules.get("infrastructure") or {}
    ident = identity(dump)
    values = {
        "vendor_id": hx(ident["vendor_id"], 8),
        "product_code": hx(ident["product_code"], 8),
        "sii_name": ident["sii_name"],
        "sii_order": ident["sii_order"],
        "position": dump.get("position"),
    }
    plan.update(rule="infrastructure", status="no_process_data", action="none", device_class="infrastructure")
    plan["record"] = {k: values.get(k) for k in rule.get("record") or values}
    return plan


def apply_fallback(dump: dict, plan: dict, reason: str) -> dict:
    """기본 PDO를 그대로 두고 방향·비트 길이로 일반 라벨만 붙인다."""
    rx, tx, src = current_pdos(dump)
    roles = {}
    n = {"in": 0, "out": 0}
    for key, pdos in (("in", tx), ("out", rx)):
        for p in pdos:
            for e in p["entries"]:
                if e["index"] == 0:
                    continue
                roles[(e["index"], e["subindex"])] = f"{key}_{n[key]}_{e['bit_len']}bit"
                n[key] += 1
    plan.update(status="fallback", needs_llm=True,
                action="keep_current" if (rx or tx) else "none", pdo_source=src,
                device_class=plan["device_class"] or "unknown")
    plan["rx"], plan["rx_bits"] = layout(rx, roles)
    plan["tx"], plan["tx_bits"] = layout(tx, roles)
    plan["notes"].append(f"맞는 rule 없음: {reason}. 기본 PDO 유지 + 일반 라벨")
    return plan


# ---------------------------------------------------------------- LLM 선택지, 방향 검증

def rule_roles(node) -> list[str]:
    """rule 안의 role 이름을 순서대로 (중복 제거) 모은다."""
    out: list[str] = []

    def walk(n):
        if isinstance(n, dict):
            if isinstance(n.get("role"), str) and n["role"] not in out:
                out.append(n["role"])
            for v in n.values():
                walk(v)
        elif isinstance(n, list):
            for v in n:
                walk(v)
    walk(node)
    return out


def vocabulary(rules: dict) -> dict:
    """LLM 출력 JSON 스키마의 선택지 (llm_vocabulary.yaml + 각 rule의 mode/extras/role)."""
    v = rules.get("llm_vocabulary") or {}
    roles = {}
    for cls, r in (v.get("roles") or {}).items():
        roles[cls] = rule_roles(rules[r["from_rule"]]) if isinstance(r, dict) else list(r)
    requests = {rid: {"modes": list((rules[rid].get("modes") or {})), "extras": list(rules[rid].get("extras") or {})}
                for rid in PROFILE_RULES if rid in rules}
    return {"device_classes": list(v.get("device_classes") or []), "roles": roles,
            "requests": requests, "direction": v.get("direction") or {}}


def check_directions(plan: dict, rules: dict) -> None:
    """측정·상태는 TxPDO, 지령·출력은 RxPDO에 있어야 한다 (llm_vocabulary.yaml의 direction)."""
    direction = (rules.get("llm_vocabulary") or {}).get("direction") or {}
    for side, other in (("rx", "tx"), ("tx", "rx")):
        wrong = set(direction.get(other) or [])
        for row in plan[side]:
            base = (row["role"] or "").split("[")[0]
            if base in wrong:
                plan["notes"].append(f"방향 경고: {row['role']}({row['index']})가 {side.upper()}PDO에 있음")


# ---------------------------------------------------------------- 진입점

def evaluate(dump: dict, rules: dict, mode: str | None = None, extras=None) -> dict:
    """dump: device_probe 출력 또는 내부 형식. mode: CiA402 운전 모드 (None이면 rule default),
    extras: 추가 항목 이름 목록 (None이면 rule default)."""
    dump = normalize_dump(dump)
    plan = new_plan(dump)
    rule_id = select_rule(dump)
    if rule_id == "infrastructure":
        plan = apply_infrastructure(dump, rules, plan)
    elif rule_id == "cia402":
        plan = apply_cia402(dump, rules, plan, mode, extras)
    elif rule_id in ("mdp5001", "cia401", "non_coe_io"):
        plan = apply_io(dump, rules, plan, rule_id, extras)
    else:
        model = find_model(dump, rules, None, plan["notes"])
        if model:
            # 프로파일 rule이 없는 장치: 승인된 모델 rule(LLM 결과 등)만으로 처리
            plan = apply_io(dump, rules, plan, f"model:{model.get('_file')}", extras, model=model)
        else:
            profile = ((dump.get("od") or {}).get("device_type") or {}).get("profile")
            plan = apply_fallback(dump, plan, reason=f"branch={dump.get('branch')}, profile={profile}")
    check_directions(plan, rules)
    return plan


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="EtherCAT 덤프 JSON에 rule을 적용해 매핑 계획을 만든다.")
    ap.add_argument("dumps", nargs="*", help="덤프 JSON 파일")
    ap.add_argument("--mode", default=None, help="CiA402 운전 모드 (없으면 rule default, 현재 cst)")
    ap.add_argument("--extras", default=None, help="추가 항목 이름, 쉼표로 구분 (예: state_monitor,error_detect)")
    ap.add_argument("--out", default="plans", help="계획 JSON 출력 폴더")
    ap.add_argument("--rules", default=str(RULE_DIR), help="rule 폴더")
    ap.add_argument("--vocab", action="store_true", help="LLM 출력 선택지를 JSON으로 출력하고 끝냄")
    args = ap.parse_args(argv)

    rules = load_rules(Path(args.rules))
    if args.vocab:
        print(json.dumps(vocabulary(rules), ensure_ascii=False, indent=2))
        return 0
    if not args.dumps:
        ap.error("덤프 JSON 파일을 지정해야 함")
    extras = [x.strip() for x in args.extras.split(",") if x.strip()] if args.extras is not None else None

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    print(f"{'file':28} {'branch':14} {'rule':14} {'status':16} {'class':15} {'action':12}")
    for f in args.dumps:
        dump = json.loads(Path(f).read_text(encoding="utf-8"))
        plan = evaluate(dump, rules, args.mode, extras)
        (out / (Path(f).stem + ".plan.json")).write_text(
            json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"{Path(f).name:28} {str(plan['branch']):14} {str(plan['rule']):14} "
              f"{plan['status']:16} {str(plan['device_class']):15} {str(plan['action']):12}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
