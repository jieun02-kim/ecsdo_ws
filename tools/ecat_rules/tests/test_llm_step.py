"""llm_step 테스트 (Ollama 없이: LLM 응답을 가짜로 넣는다)."""
import copy
import json
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import llm_step  # noqa: E402
import rule_engine as re_  # noqa: E402

RULES = re_.load_rules(ROOT / "rules")
VOCAB = re_.vocabulary(RULES)
HOST = "http://fake"


def dump_of(name):
    return json.loads((ROOT / "samples" / name).read_text(encoding="utf-8"))


def fake_llm(*answers):
    """호출 순서대로 answers를 돌려주는 ollama_chat 대체. 받은 프롬프트와 스키마는 calls에 남는다."""
    calls = []

    def chat(host, model, prompt, schema, timeout=300):
        calls.append({"prompt": prompt, "schema": schema})
        return answers[len(calls) - 1]
    return chat, calls


class TestFallback(unittest.TestCase):
    def test_class_then_roles_and_candidate(self):
        chat, calls = fake_llm({"device_class": "force_torque_sensor", "reason": "-"},
                               {"e0": "force_x", "e1": "force_y", "e2": "force_z", "e3": "sensor_control"})
        with mock.patch.object(llm_step, "ollama_chat", chat):
            res = llm_step.process(dump_of("ft_sensor_other_profile.json"), RULES, VOCAB, HOST, "m")
        self.assertEqual(len(calls), 2)
        # 2단계: 역할 선택지는 정한 형태의 역할 + unknown만
        self.assertEqual(calls[1]["schema"]["properties"]["e0"]["enum"], VOCAB["roles"]["force_torque_sensor"] + ["unknown"])
        self.assertEqual(res["result"]["device_class"], "force_torque_sensor")
        cand = res["candidate"]
        self.assertEqual(cand["match"], {"vendor_id": 0x5678, "device_name": "SimFT6"})
        self.assertIsNone(cand["base_rule"])
        self.assertFalse(cand["source"]["approved"])
        self.assertEqual([(o["role"], o["index"], o["sub"]) for o in cand["required"]["tx"]],
                         [("force_x", 0x6000, 1), ("force_y", 0x6000, 2), ("force_z", 0x6000, 3)])

    def test_wrong_direction_becomes_unknown(self):
        chat, _ = fake_llm({"device_class": "force_torque_sensor", "reason": "-"},
                           {"e0": "force_x", "e1": "sensor_control", "e2": "force_z", "e3": "force_x"})
        with mock.patch.object(llm_step, "ollama_chat", chat):
            res = llm_step.process(dump_of("ft_sensor_other_profile.json"), RULES, VOCAB, HOST, "m")
        roles = {r["index"]: r["role"] for r in res["result"]["roles"]}
        self.assertEqual(roles["0x6000:02"], "unknown")   # sensor_control은 RxPDO 역할
        self.assertEqual(roles["0x7000:01"], "unknown")   # force_x는 TxPDO 역할
        self.assertEqual(len(res["problems"]), 2)
        self.assertNotIn("rx", res["candidate"]["required"])

    def test_no_name_matches_by_product_code(self):
        chat, _ = fake_llm({"device_class": "force_torque_sensor", "reason": "-"},
                           {"e0": "force_x", "e1": "force_y", "e2": "force_z", "e3": "unknown"})
        with mock.patch.object(llm_step, "ollama_chat", chat):
            res = llm_step.process(dump_of("custom_no_profile.json"), RULES, VOCAB, HOST, "m")
        self.assertEqual(res["candidate"]["match"], {"vendor_id": 0x1234, "product_code": 0x10})

    def test_dry_run_builds_input_without_calling(self):
        with mock.patch.object(llm_step, "ollama_chat", side_effect=AssertionError("called")):
            res = llm_step.process(dump_of("ft_sensor_other_profile.json"), RULES, VOCAB, None, "m")
        self.assertEqual(res["steps"][0]["step"], "device_class")
        self.assertIn("Force X", res["steps"][0]["input"]["prompt"])
        self.assertIsNone(res["steps"][0]["output"])


class TestCia402(unittest.TestCase):
    def test_substitute_candidate(self):
        dump = dump_of("drive_csp_mapped.json")
        dump["od.profile_axis0"] = [o for o in dump["od.profile_axis0"] if o["index"] != "0x6077"]
        dump["od.vendor"] = {"objects": [{"index": "0x2077", "name": "Torque feedback"}], "total": 1, "truncated": False}
        chat, calls = fake_llm({"actual_torque": "0x2077"})
        with mock.patch.object(llm_step, "ollama_chat", chat):
            res = llm_step.process(dump, RULES, VOCAB, HOST, "m")
        self.assertEqual(calls[0]["schema"]["properties"]["actual_torque"]["enum"], ["0x2077", "none"])
        tx = res["candidate"]["modes"]["cst"]["tx"]
        self.assertIn(0x2077, [o["index"] for o in tx])
        self.assertEqual(res["candidate"]["base_rule"], "cia402")

    def test_none_gives_no_candidate(self):
        dump = dump_of("drive_csp_mapped.json")
        dump["od.profile_axis0"] = [o for o in dump["od.profile_axis0"] if o["index"] != "0x6077"]
        dump["od.vendor"] = {"objects": [{"index": "0x2000", "name": "Motor ID"}], "total": 1, "truncated": False}
        chat, _ = fake_llm({"actual_torque": "none"})
        with mock.patch.object(llm_step, "ollama_chat", chat):
            res = llm_step.process(dump, RULES, VOCAB, HOST, "m")
        self.assertIsNone(res["candidate"])
        self.assertTrue(res["problems"])


class TestApprovedCandidate(unittest.TestCase):
    def test_approved_candidate_used_as_rule(self):
        chat, _ = fake_llm({"device_class": "force_torque_sensor", "reason": "-"},
                           {"e0": "force_x", "e1": "force_y", "e2": "force_z", "e3": "sensor_control"})
        with mock.patch.object(llm_step, "ollama_chat", chat):
            cand = llm_step.process(dump_of("ft_sensor_other_profile.json"), RULES, VOCAB, HOST, "m")["candidate"]
        rules = copy.deepcopy(RULES)
        rules["models"] = [dict(cand, _file="simft6.yaml")]
        p = re_.evaluate(dump_of("ft_sensor_other_profile.json"), rules)
        self.assertEqual((p["status"], p["rule"], p["device_class"]), ("matched", "model:simft6.yaml", "force_torque_sensor"))
        self.assertFalse(p["needs_llm"])

    def test_unapproved_candidate_not_loaded(self):
        import tempfile, shutil
        with tempfile.TemporaryDirectory() as d:
            shutil.copytree(ROOT / "rules", Path(d) / "rules")
            (Path(d) / "rules/models/x.yaml").write_text("match: {vendor_id: 1}\nsource: {approved: false}\n")
            with mock.patch("sys.stderr"):
                self.assertEqual(re_.load_rules(Path(d) / "rules")["models"], [])


if __name__ == "__main__":
    unittest.main()
