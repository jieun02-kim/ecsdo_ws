#!/usr/bin/env python3
"""rule이 못 푼 장치(needs_llm)를 로컬 LLM(Ollama)으로 판별하고, 검증한 결과를 모델 rule 후보로 만든다.

  python3 llm_step.py dump/*.json --out llm_out               # Ollama 호출
  python3 llm_step.py dump/*.json --out llm_out --dry-run     # LLM 입력만 저장 (호출 안 함)

장치마다 rule_engine으로 계획을 먼저 만들고, needs_llm인 장치만 LLM에 보낸다.

  경우                                    LLM이 정하는 것
  fallback (프로파일 rule 없음)            장치 형태 → 엔트리별 역할 (2단계)
  rule 적용, 분류 못 한 엔트리 있음          남은 엔트리의 역할
  cia402, 필수 객체 없음                    빠진 역할마다 대체할 벤더 객체 (없으면 none)

LLM 출력은 JSON 스키마로 선택지(llm_vocabulary.yaml + rule) 안의 값만 나오게 강제한다.
그 뒤 코드가 방향 규칙과 객체 존재를 검증한다. 결과는 <파일>.llm.json과
<VID>_<이름>.candidate.yaml(모델 rule 후보)로 저장한다. 후보는 사람이 확인한 뒤
rules/models/로 옮겨야 rule로 쓰인다 (그 전에는 아무것도 바뀌지 않는다).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

import yaml

import rule_engine as re_

DEFAULT_MODEL = "qwen3.5:4b"
DEFAULT_HOST = "http://localhost:11434"
OBJECTS_FOR_CONTEXT = 60      # LLM에 참고로 주는 객체 이름 수 (작은 모델은 입력이 길면 정확도가 떨어짐)

SYSTEM = (
    "You classify EtherCAT slave devices for a robot controller. "
    "Use only the facts given (names, data types, bit lengths, PDO direction). "
    "TxPDO = inputs to the master (measurements, status). RxPDO = outputs from the master (commands). "
    "If the facts do not support a choice, answer 'unknown' (or 'none'). Never guess an index."
)


# ---------------------------------------------------------------- Ollama

class LLMError(RuntimeError):
    pass


def ollama_chat(host: str, model: str, prompt: str, schema: dict, timeout: float = 300) -> dict:
    """JSON 스키마로 출력을 강제해서 한 번 호출한다. temperature 0, thinking 끔."""
    body = {
        "model": model,
        "stream": False,
        "think": False,
        "format": schema,
        "options": {"temperature": 0},
        "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}],
    }
    req = urllib.request.Request(f"{host}/api/chat", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            resp = json.loads(r.read())
    except (urllib.error.URLError, TimeoutError) as e:
        raise LLMError(f"Ollama 호출 실패 ({host}): {e}") from e
    if resp.get("error"):
        raise LLMError(resp["error"])
    try:
        return json.loads(resp["message"]["content"])
    except (KeyError, json.JSONDecodeError) as e:
        raise LLMError(f"JSON이 아닌 응답: {resp.get('message')}") from e


# ---------------------------------------------------------------- LLM 입력

def device_facts(dump: dict) -> dict:
    """LLM에 주는 장치 정보 (식별, 이름, 프로파일, 참고용 객체 이름)."""
    sii, od = dump.get("sii") or {}, dump.get("od") or {}
    strings = sii.get("strings") or {}
    facts = {
        "vendor": sii.get("vendor_name"),
        "device_name": (od.get("name_ver") or {}).get("device_name") or strings.get("name"),
        "order": strings.get("order"),
        "group": strings.get("group"),
        "profile": (od.get("device_type") or {}).get("profile"),
    }
    objs = (od.get("objects") or {}).get("objects") or (od.get("vendor") or {}).get("objects") or []
    if objs:
        facts["objects"] = [f"{o['index']} {o.get('name') or ''}".strip() for o in objs[:OBJECTS_FOR_CONTEXT]]
    return {k: v for k, v in facts.items() if v not in (None, [], "")}


def entry_rows(dump: dict, only: set | None = None) -> list[dict]:
    """판단할 PDO 엔트리 목록. id는 LLM이 index를 베끼지 않게 붙이는 번호다."""
    rx, tx, _ = re_.current_pdos(dump)
    rows = []
    for direction, pdos in (("TxPDO", tx), ("RxPDO", rx)):
        for p in pdos:
            for e in p["entries"]:
                key = (e["index"], e["subindex"])
                if e["index"] == 0 or (only is not None and key not in only):
                    continue
                rows.append({
                    "id": f"e{len(rows)}", "dir": direction,
                    "index": f"0x{e['index']:04X}:{e['subindex']:02X}", "bits": e["bit_len"],
                    "type": e["data_type"], "name": e["name"],
                    **({"object": e["object_name"]} if e["object_name"] and e["object_name"] != e["name"] else {}),
                    **({"pdo": p["name"]} if p.get("name") else {}),
                    "_key": key,
                })
    return rows


def public(rows: list[dict]) -> list[dict]:
    return [{k: v for k, v in r.items() if not k.startswith("_")} for r in rows]


def class_roles(vocab: dict, cls: str) -> list[str]:
    """형태별 허용 역할 + common. 어휘에 없는 형태(mixed_io 등)는 I/O 역할 전체."""
    roles = vocab["roles"].get(cls)
    if roles is None:
        roles = sorted({r for c in ("digital_io", "analog_input", "analog_output") for r in vocab["roles"].get(c, [])})
    return list(dict.fromkeys(list(roles) + list(vocab["roles"].get("common", ["unknown"]))))


# ---------------------------------------------------------------- 판단 단계

def ask_class(host, model, facts, rows, vocab) -> tuple[dict, dict]:
    classes = [c for c in vocab["device_classes"]]
    prompt = (
        "Pick the device class.\n\n"
        f"Device:\n{json.dumps(facts, ensure_ascii=False, indent=1)}\n\n"
        f"Process data entries:\n{json.dumps(public(rows), ensure_ascii=False, indent=1)}\n\n"
        f"Choices: {classes}"
    )
    schema = {"type": "object", "properties": {
        "device_class": {"type": "string", "enum": classes},
        "reason": {"type": "string"}}, "required": ["device_class", "reason"]}
    return {"prompt": prompt, "schema": schema}, (ollama_chat(host, model, prompt, schema) if host else None)


def ask_roles(host, model, facts, rows, cls, roles, direction_rules) -> tuple[dict, dict]:
    hint = {r: ("TxPDO" if r in direction_rules.get("tx", []) else "RxPDO" if r in direction_rules.get("rx", []) else "any")
            for r in roles}
    prompt = (
        f"The device is a {cls}. Give each process data entry a role.\n\n"
        f"Device:\n{json.dumps(facts, ensure_ascii=False, indent=1)}\n\n"
        f"Entries:\n{json.dumps(public(rows), ensure_ascii=False, indent=1)}\n\n"
        f"Roles and the PDO direction they belong to:\n{json.dumps(hint, indent=1)}\n"
        "A role must match the entry's PDO direction. Use 'unknown' when unsure."
    )
    schema = {"type": "object", "properties": {r["id"]: {"type": "string", "enum": roles} for r in rows},
              "required": [r["id"] for r in rows]}
    return {"prompt": prompt, "schema": schema}, (ollama_chat(host, model, prompt, schema) if host else None)


def ask_substitutes(host, model, facts, missing: list[dict], candidates: list[dict]) -> tuple[dict, dict]:
    choices = [c["index"] for c in candidates] + ["none"]
    prompt = (
        "This CiA402 drive lacks standard objects the controller needs. For each missing role, pick the vendor "
        "object that holds the same quantity, or 'none' if no object clearly does.\n\n"
        f"Device:\n{json.dumps({k: v for k, v in facts.items() if k != 'objects'}, ensure_ascii=False, indent=1)}\n\n"
        f"Missing:\n{json.dumps(missing, ensure_ascii=False, indent=1)}\n\n"
        f"Vendor objects:\n{json.dumps(candidates, ensure_ascii=False, indent=1)}"
    )
    schema = {"type": "object", "properties": {m["role"]: {"type": "string", "enum": choices} for m in missing},
              "required": [m["role"] for m in missing]}
    return {"prompt": prompt, "schema": schema}, (ollama_chat(host, model, prompt, schema) if host else None)


# ---------------------------------------------------------------- 검증, 후보

def validate_roles(rows: list[dict], answer: dict, direction_rules: dict) -> tuple[dict, list[str]]:
    """방향 규칙에 어긋나는 역할은 unknown으로 돌리고 이유를 남긴다. 반환: {_key: role}, 문제 목록."""
    out, problems = {}, []
    for r in rows:
        role = answer.get(r["id"], "unknown")
        need = "tx" if r["dir"] == "TxPDO" else "rx"
        other = "rx" if need == "tx" else "tx"
        if role in direction_rules.get(other, []):
            problems.append(f"{r['index']} {r['name']}: {role}는 {other.upper()}PDO 역할인데 {r['dir']}에 있음 → unknown")
            role = "unknown"
        out[r["_key"]] = role
    return out, problems


def ident_slug(dump: dict) -> str:
    ident = re_.identity(dump)
    name = ident["device_name"] or ident["sii_order"] or ident["sii_name"] or "device"
    return f"{ident['vendor_id'] or 0:08x}_{re.sub(r'[^A-Za-z0-9]+', '_', name).strip('_').lower()}"


def match_block(dump: dict) -> dict:
    """Vendor ID + 이름. 이름(0x1008, SII 주문번호)이 없으면 product code로 좁힌다 (벤더 전체에 걸리지 않게)."""
    ident = re_.identity(dump)
    name = ident["device_name"] or ident["sii_order"]
    if name:
        return {"vendor_id": ident["vendor_id"] or 0, "device_name": name}
    return {"vendor_id": ident["vendor_id"] or 0, "product_code": ident["product_code"] or 0}


def candidate_io(dump: dict, cls: str, roles: dict, rows: list[dict], base_rule, model: str) -> dict:
    """역할이 정해진 엔트리로 모델 rule 후보를 만든다 (unknown은 넣지 않음)."""
    req = {"tx": [], "rx": []}
    for r in rows:
        role = roles[r["_key"]]
        if role == "unknown":
            continue
        idx, sub = r["_key"]
        req["tx" if r["dir"] == "TxPDO" else "rx"].append(
            {"role": role, "index": idx, "sub": sub, "bits": r["bits"], **({"type": r["type"]} if r["type"] else {}),
             "note": r["name"]})
    cand = {"match": match_block(dump), "base_rule": base_rule, "device_class": cls,
            "source": {"by": f"llm:{model}", "approved": False}, "default": {"extras": [], "pdo": "keep_current"},
            "required": {k: v for k, v in req.items() if v}}
    return cand


def write_candidate(path: Path, cand: dict, header: str) -> None:
    text = yaml.safe_dump(cand, allow_unicode=True, sort_keys=False)
    text = re.sub(r"\b(index|vendor_id|product_code): (\d+)",
                  lambda m: f"{m.group(1)}: 0x{int(m.group(2)):0{4 if m.group(1) == 'index' else 8}X}", text)
    path.write_text(header + text, encoding="utf-8")


# ---------------------------------------------------------------- 장치 하나

def process(dump_raw: dict, rules: dict, vocab: dict, host: str | None, model: str) -> dict:
    dump = re_.normalize_dump(dump_raw)
    plan = re_.evaluate(dump, rules)
    res = {"position": plan["position"], "identity": plan["identity"], "rule": plan["rule"],
           "plan_status": plan["status"], "needs_llm": plan["needs_llm"], "steps": [],
           "result": None, "problems": [], "candidate": None}
    if not plan["needs_llm"]:
        res["result"] = "LLM 불필요"
        return res

    facts = device_facts(dump)
    direction_rules = vocab["direction"]

    if plan["status"] == "fallback" or (plan["status"] == "matched" and not plan["rule"].startswith("cia402")):
        if plan["status"] == "fallback":
            rows = entry_rows(dump)
            if not rows:
                res["result"] = "판단할 PDO 엔트리가 없음 (덤프에 PDO 없음)"
                return res
            q, a = ask_class(host, model, facts, rows, vocab)
            res["steps"].append({"step": "device_class", "input": q, "output": a})
            if a is None:
                return res
            cls, base_rule = a["device_class"], None
        else:
            # rule은 적용됐지만 역할을 못 붙인 엔트리만 묻는다
            done = {(int(r["index"], 16), r["subindex"]) for r in plan["rx"] + plan["tx"] if r["role"]}
            rx, tx, _ = re_.current_pdos(dump)
            todo = {(e["index"], e["subindex"]) for p in rx + tx for e in p["entries"]
                    if e["index"] and (e["index"], e["subindex"]) not in done}
            rows = entry_rows(dump, only=todo)
            cls, base_rule = plan["device_class"], plan["rule"]
        roles = class_roles(vocab, cls)
        q, a = ask_roles(host, model, facts, rows, cls, roles, direction_rules)
        res["steps"].append({"step": "roles", "input": q, "output": a})
        if a is None:
            return res
        decided, problems = validate_roles(rows, a, direction_rules)
        res["problems"] = problems
        res["result"] = {"device_class": cls,
                         "roles": [{"index": r["index"], "name": r["name"], "dir": r["dir"], "role": decided[r["_key"]]}
                                   for r in rows]}
        if any(v != "unknown" for v in decided.values()):
            res["candidate"] = candidate_io(dump, cls, decided, rows, base_rule, model)
        return res

    if plan["rule"] and plan["rule"].startswith("cia402") and plan["missing_required"]:
        vendor = ((dump.get("od") or {}).get("vendor") or {}).get("objects") or []
        if not vendor:
            res["result"] = "벤더 객체 목록이 없어 대체 객체를 찾을 수 없음 (SDO Info 없음)"
            return res
        mode = plan["request"]["mode"]
        mrule = rules["cia402"]["modes"][mode]
        need = {d["role"]: (direction, d) for direction in ("rx", "tx") for d in mrule.get(direction) or []}
        missing = []
        for m in plan["missing_required"]:
            role = m.split()[0].split("[")[0]
            if role in need and role not in [x["role"] for x in missing]:
                direction, d = need[role]
                missing.append({"role": role, "standard_index": f"0x{d['index']:04X}", "bits": d.get("bits"),
                                "dir": "RxPDO" if direction == "rx" else "TxPDO"})
        q, a = ask_substitutes(host, model, facts, missing, vendor)
        res["steps"].append({"step": "substitutes", "input": q, "output": a})
        if a is None:
            return res
        found = {k: v for k, v in a.items() if v != "none"}
        res["result"] = {"mode": mode, "substitutes": a}
        if len(found) < len(missing):
            res["problems"].append(f"대체 객체 없음: {[k for k, v in a.items() if v == 'none']} → 이 모드로는 매핑 불가")
        if found:
            # 모드의 필수 목록을 대체 index로 바꾼 후보. 벤더 객체의 타입·비트 길이는 덤프에 없어 표준 객체 값을 씀
            new = {direction: [dict(d, index=int(found[d["role"]], 16), note="LLM 대체 후보: 타입·비트 길이 확인 필요")
                               if d["role"] in found else d for d in mrule.get(direction) or []]
                   for direction in ("rx", "tx")}
            res["candidate"] = {"match": match_block(dump), "base_rule": "cia402",
                                "source": {"by": f"llm:{model}", "approved": False},
                                "modes": {mode: new}}
        return res

    res["result"] = "LLM으로 판단할 수 있는 경우가 아님"
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="needs_llm 장치를 Ollama로 판별하고 모델 rule 후보를 만든다.")
    ap.add_argument("dumps", nargs="+", help="device_probe 덤프 JSON")
    ap.add_argument("--out", default="llm_out", help="출력 폴더")
    ap.add_argument("--rules", default=str(re_.RULE_DIR), help="rule 폴더")
    ap.add_argument("--model", default=DEFAULT_MODEL, help=f"Ollama 모델 (기본 {DEFAULT_MODEL})")
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"Ollama 주소 (기본 {DEFAULT_HOST})")
    ap.add_argument("--dry-run", action="store_true", help="LLM을 호출하지 않고 입력(프롬프트, 스키마)만 저장")
    args = ap.parse_args(argv)

    rules = re_.load_rules(Path(args.rules))
    vocab = re_.vocabulary(rules)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    host = None if args.dry_run else args.host.rstrip("/")
    failed = 0
    for f in args.dumps:
        dump = json.loads(Path(f).read_text(encoding="utf-8"))
        try:
            res = process(dump, rules, vocab, host, args.model)
        except LLMError as e:
            print(f"{Path(f).name}: {e}", file=sys.stderr)
            failed += 1
            continue
        stem = Path(f).stem
        if res["candidate"]:
            cpath = out / f"{ident_slug(re_.normalize_dump(dump))}.candidate.yaml"
            write_candidate(cpath, res["candidate"],
                            f"# 모델 rule 후보 ({Path(f).name}, {args.model}). 확인 전에는 쓰이지 않음.\n"
                            f"# 확인 후 source.approved를 true로 바꾸고 rules/models/로 옮긴다.\n")
            res["candidate"] = str(cpath)
        (out / f"{stem}.llm.json").write_text(json.dumps(res, ensure_ascii=False, indent=2), encoding="utf-8")
        summary = res["result"] if isinstance(res["result"], str) or res["result"] is None else \
            res["result"].get("device_class") or res["result"].get("mode")
        print(f"{Path(f).name:28} {res['plan_status']:16} needs_llm={str(res['needs_llm']):5} "
              f"{'(dry-run)' if host is None and res['needs_llm'] else summary}"
              f"{'  후보: ' + res['candidate'] if res['candidate'] else ''}")
        for p in res["problems"]:
            print(f"    ! {p}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
