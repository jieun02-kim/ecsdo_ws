"""rule 엔진 테스트 (python3 -m unittest discover -s tests)."""
import copy
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import rule_engine as re_  # noqa: E402

RULES = re_.load_rules(ROOT / "rules")


def dump_of(name):
    return json.loads((ROOT / "samples" / name).read_text(encoding="utf-8"))


def plan_of(name, mode=None, extras=None):
    return re_.evaluate(dump_of(name), RULES, mode, extras)


def indices(rows):
    return {r["index"] for r in rows}


class TestCia402(unittest.TestCase):
    def test_default_request_is_cst_without_extras(self):
        p = plan_of("drive_cst_mapped.json")
        self.assertEqual(p["request"], {"mode": "cst", "extras": []})
        self.assertEqual((p["status"], p["action"]), ("matched", "keep_current"))
        roles = {r["role"] for r in p["rx"] + p["tx"]}
        self.assertTrue({"control_word", "target_torque", "status_word", "actual_torque",
                         "actual_position", "actual_velocity"} <= roles)
        self.assertEqual(p["sdo_writes"], [])                    # 0x6060이 PDO에 있음

    def test_csp_mapping_needs_remap_for_cst(self):
        p = plan_of("drive_csp_mapped.json")
        self.assertEqual((p["status"], p["action"]), ("matched", "remap"))
        rx = [(r["index"], r["role"]) for r in p["rx"]]
        self.assertEqual(rx, [("0x6040", "control_word"), ("0x6071", "target_torque")])
        writes = [(w["index"], w["subindex"], w["value"]) for w in p["sdo_writes"]]
        self.assertIn(("0x1600", 1, "0x60400010"), writes)       # control_word 매핑 엔트리
        self.assertEqual(writes[-1], ("0x6060", 0, 10))           # CST 모드 설정 (init_sdo)

    def test_unrequested_extras_not_mapped(self):
        # 이전 엔진은 장치에 있는 선택 객체(0x603F 등)를 자동으로 넣었지만 v2는 요청한 것만 넣는다
        p = plan_of("drive_csp_mapped.json")
        self.assertNotIn("0x603F", indices(p["tx"]))
        self.assertNotIn("0x60B2", indices(p["rx"]))

    def test_requested_extras_added_in_order(self):
        p = plan_of("drive_csp_mapped.json", extras=["error_detect", "feedforward"])
        self.assertEqual(p["action"], "remap")
        self.assertEqual([r["index"] for r in p["tx"]][-1], "0x603F")
        self.assertEqual([r["index"] for r in p["rx"]], ["0x6040", "0x6071", "0x60B1", "0x60B2"])
        self.assertEqual([(e["index"], e["extra"]) for e in p["on_event"]], [("0x1001", "error_detect")])

    def test_missing_extra_objects_reported(self):
        p = plan_of("drive_cst_mapped.json", extras=["touch_probe", "state_monitor"])
        missing = {(m["extra"], m["index"]) for m in p["missing_extras"]}
        self.assertEqual(missing, {("touch_probe", "0x60B8"), ("touch_probe", "0x60B9"), ("touch_probe", "0x60BA")})
        self.assertEqual(p["status"], "matched")                  # 필수 항목은 모두 있음

    def test_unknown_extra_name_reported(self):
        p = plan_of("drive_cst_mapped.json", extras=["no_such_extra"])
        self.assertEqual(p["request"]["extras"], [])
        self.assertEqual(p["missing_extras"][0]["reason"], "rule에 없음")

    def test_same_dump_keeps_current_in_csp_mode(self):
        p = plan_of("drive_csp_mapped.json", mode="csp")
        self.assertEqual(p["action"], "keep_current")

    def test_mode_not_supported_fails(self):
        p = plan_of("drive_no_cst.json")
        self.assertEqual((p["status"], p["device_class"]), ("failed", "servo_drive"))

    def test_two_axis_offsets(self):
        p = plan_of("drive_2axis.json")
        self.assertEqual(p["count"], 2)
        self.assertTrue({"0x6871", "0x6877", "0x6864"} <= indices(p["rx"] + p["tx"]))   # 축 1 = 기본 + 0x800
        self.assertIn("0x1610", {r["pdo"] for r in p["rx"]})
        self.assertIn(("0x6860", 10), {(w["index"], w["value"]) for w in p["sdo_writes"]})

    def test_missing_required_object_fails(self):
        dump = dump_of("drive_csp_mapped.json")
        dump["od.profile_axis0"] = [o for o in dump["od.profile_axis0"] if o["index"] != "0x6071"]
        p = re_.evaluate(dump, RULES)
        self.assertEqual(p["status"], "failed")
        self.assertTrue(p["needs_llm"])
        self.assertTrue(any("0x6071" in m for m in p["missing_required"]))

    def test_model_rule_changes_axis_step(self):
        rules = copy.deepcopy(RULES)
        rules["models"] = [{"_file": "l7nh.yaml", "match": {"vendor_id": 0x7595, "device_name": "L7NH"},
                            "base_rule": "cia402", "remap": {"axis_step": 0x1}}]
        p = re_.evaluate(dump_of("drive_2axis.json"), rules)
        self.assertEqual(p["model_rule"], "l7nh.yaml")
        self.assertIn("0x1601", {r["pdo"] for r in p["rx"]})

    def test_model_rule_adds_extras(self):
        rules = copy.deepcopy(RULES)
        rules["models"] = [{"_file": "l7nh.yaml", "match": {"vendor_id": 0x7595, "device_name": "L7NH"},
                            "base_rule": "cia402",
                            "extras": {"vendor_alarm": {"on_event": [{"role": "vendor_alarm_code", "index": 0x2010}]}}}]
        p = re_.evaluate(dump_of("drive_cst_mapped.json"), rules, extras=["vendor_alarm"])
        self.assertEqual(p["on_event"][0]["role"], "vendor_alarm_code")

    def test_pdo_config_forces_new_mapping(self):
        dump = dump_of("drive_cst_mapped.json")
        dump["sii.coe_details"] = {"pdo_assign": True, "pdo_config": True}
        p = re_.evaluate(dump, RULES)
        self.assertEqual(p["action"], "remap")
        self.assertEqual(indices(p["rx"]), {"0x6040", "0x6071"})   # 0x6060은 빠지고 SDO로 설정
        self.assertEqual(p["sdo_writes"][-1]["index"], "0x6060")

    def test_assign_only_selects_fixed_sets(self):
        dump = dump_of("drive_csp_mapped.json")
        dump["sii.coe_details"] = {"pdo_assign": True, "pdo_config": False}
        ent = lambda i, n: {"index": i, "subindex": 0, "bit_len": n}  # noqa: E731
        dump["sii.pdo"] = {
            "rxpdo": [{"index": "0x1600", "entries": [ent("0x6040", 16), ent("0x607A", 32)]},
                   {"index": "0x1601", "entries": [ent("0x6040", 16), ent("0x6071", 16)]}],
            "txpdo": [{"index": "0x1A00", "entries": [ent("0x6041", 16), ent("0x6064", 32),
                                                   ent("0x606C", 32), ent("0x6077", 16)]}]}
        p = re_.evaluate(dump, RULES)
        self.assertEqual((p["status"], p["action"]), ("matched", "select_fixed"))
        writes = [(w["index"], w["subindex"], w["value"]) for w in p["sdo_writes"]]
        self.assertIn(("0x1C12", 1, 0x1601), writes)
        self.assertIn(("0x1C12", 0, 1), writes)                    # 0x1600은 0x6040이 겹쳐서 할당 안 함
        self.assertIn(("0x1C13", 1, 0x1A00), writes)

    def test_fixed_pdo_without_required_fails(self):
        dump = dump_of("drive_csp_mapped.json")
        dump["sii.coe_details"] = {"pdo_assign": False, "pdo_config": False}
        p = re_.evaluate(dump, RULES)
        self.assertEqual((p["status"], p["action"]), ("failed", "none"))


class TestIO(unittest.TestCase):
    def test_digital_input(self):
        p = plan_of("el1008_dig_in.json")
        self.assertEqual((p["rule"], p["status"], p["device_class"], p["count"]),
                         ("non_coe_io", "matched", "digital_input", 8))
        self.assertEqual(p["action"], "keep_sii")
        self.assertEqual(p["channels"][3]["index"], "0x6030")

    def test_digital_output(self):
        p = plan_of("el2008_dig_out.json")
        self.assertEqual((p["device_class"], p["count"]), ("digital_output", 8))

    def test_analog_input_status_bits_not_digital(self):
        p = plan_of("el3102_ana_in.json")
        self.assertEqual((p["rule"], p["device_class"], p["count"]), ("mdp5001", "analog_input", 2))
        self.assertEqual(p["tx_bits"], 64)
        self.assertFalse(p["needs_llm"])
        self.assertFalse(any(c["role"] == "channel_status" for c in p["channels"]))

    def test_channel_status_extra(self):
        p = plan_of("el3102_ana_in.json", extras=["channel_status", "module_info"])
        self.assertEqual((p["device_class"], p["count"]), ("analog_input", 2))
        self.assertTrue(any(c["role"] == "channel_status" for c in p["channels"]))
        self.assertEqual([e["index"] for e in p["on_event"]], ["0xF030", "0xF050"])

    def test_cia401(self):
        dump = {"position": 9, "branch": "other_profile", "sii.mailbox": ["CoE"],
                "od.device_type": {"profile": 401},
                "od.rxpdo": {"basis": "assigned", "pdos": [
                    {"index": "0x1600", "entries": [{"index": "0x6200", "subindex": 1, "bit_len": 8, "name": "DO 1-8"}]}]},
                "od.txpdo": {"basis": "assigned", "pdos": [
                    {"index": "0x1A00", "entries": [{"index": "0x6000", "subindex": 1, "bit_len": 8, "name": "DI 1-8"}]}]}}
        p = re_.evaluate(dump, RULES)
        self.assertEqual((p["rule"], p["device_class"], p["count"]), ("cia401", "digital_io", 16))


class TestOther(unittest.TestCase):
    def test_infrastructure(self):
        p = plan_of("ek1100_coupler.json")
        self.assertEqual((p["rule"], p["status"]), ("infrastructure", "no_process_data"))
        self.assertEqual(p["record"]["sii_order"], "EK1100")

    def test_other_profile_device_probe_dump(self):
        # device_probe(probe_sim) 출력: 프로파일 없음 → fallback, 엔트리 이름은 entry_name에서
        p = plan_of("ft_sensor_other_profile.json")
        self.assertEqual((p["status"], p["pdo_source"]), ("fallback", "od"))
        self.assertEqual([r["name"] for r in p["tx"]], ["Force X", "Force Y", "Force Z"])
        self.assertEqual(p["identity"]["device_name"], "SimFT6")

    def test_internal_format_still_accepted(self):
        dump = re_.normalize_dump(dump_of("drive_cst_mapped.json"))
        self.assertIn("sii", dump)
        self.assertEqual(re_.evaluate(dump, RULES)["action"], "keep_current")

    def test_fallback_labels(self):
        p = plan_of("custom_no_profile.json")
        self.assertEqual((p["status"], p["action"]), ("fallback", "keep_current"))
        self.assertTrue(p["needs_llm"])
        self.assertEqual([r["role"] for r in p["tx"]], ["in_0_32bit", "in_1_32bit", "in_2_32bit"])

    def test_mdp_without_pdo_fails(self):
        p = re_.evaluate({"position": 10, "branch": "mdp", "sii.mailbox": ["CoE"], "od.device_type": {"profile": 5001}}, RULES)
        self.assertEqual(p["status"], "failed")

    def test_example_model_not_loaded(self):
        self.assertEqual(RULES["models"], [])

    def test_vocabulary(self):
        v = re_.vocabulary(RULES)
        self.assertIn("touch_probe_status", v["roles"]["servo_drive"])
        self.assertEqual(v["requests"]["cia402"]["modes"], ["cst", "csp", "csv"])
        self.assertIn("channel_status", v["requests"]["mdp5001"]["extras"])

    def test_no_direction_warnings_on_samples(self):
        for f in (ROOT / "samples").glob("*.json"):
            p = re_.evaluate(json.loads(f.read_text(encoding="utf-8")), RULES, extras=None)
            self.assertFalse([n for n in p["notes"] if n.startswith("방향 경고")], f.name)


if __name__ == "__main__":
    unittest.main()
