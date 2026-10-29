#!/usr/bin/env python3
"""Mock of the device's portal API, for developing/testing the web UI without hardware.

Serves components/web_config/www and answers /api/* with the same JSON shapes as
components/web_config/web_api.c (fake data, simulated pairing and IR learning).
Security checks of the firmware that do not make sense on localhost (AP interface, Host header)
are skipped; the CSRF token, POST-only changes and the form encoding are enforced.

Usage: python3 test/web/mock_server.py [port]
"""

import json
import os
import secrets
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

WWW = os.path.join(os.path.dirname(__file__), "..", "..", "components", "web_config", "www")
TOKEN = secrets.token_hex(16)
T0 = time.time()

STATE = {
    "wifi": {"configured": True, "ssid": "NhaToi", "connected": True, "rssi": -58, "channel": 6,
             "ip4": "192.168.1.42", "ip6": ["fe80::1234:56ff:fe78:9abc", "fd12:3456::42"], "conflict": False,
             "portal_open": True, "ap_ssid": "AC-Matter-1A2B", "ap_clients": 1, "portal_remaining_s": 600,
             "job": 0, "job_message": "", "scanning": False},
    "scan": [],
    "scan_until": 0,
    "fabrics": [{"index": 1, "vendor_id": 0x1349, "label": ""}],
    "window_open": False,
    "binding": None,
    "pair": {"state": 0, "t": 0, "node": 0x1A2B3C4D5E6F7081},
    "profiles": [
        {"slot": 0, "present": True, "name": "Phòng ngủ", "locked": False, "driver": 1, "step_half": 2,
         "default_fan": 0, "default_swing": 0, "carrier_hz": 0},
        {"slot": 1, "present": False}, {"slot": 2, "present": False}, {"slot": 3, "present": False},
    ],
    "codes": {0: ["off", "m0t50f0s0", "m0t48f0s0", "m0t52f1s0"]},
    "active": 0,
    "learn": {"state": 0, "session": 0, "t": 0},
    "thermostat": {"mode": 3, "cool_sp": 2500, "heat_sp": 2200},
    "fan": 0, "swing": 0,
    "ir": {"sent": 3, "failed": 0, "last": "Lạnh 25°C, quạt tự động, đảo gió tắt", "t": time.time() - 90, "src": 0},
    "config": {"control_mode": 1, "allow_local_fallback": True, "deadband_centi": 50, "max_offset_centi": 300,
               "comp_interval_s": 600, "filter_tau_s": 180, "user_debounce_ms": 1500, "resend_on_boot": False,
               "stale_min_s": 900},
    "errors": [{"t": time.time() - 300, "module": "remote_temp", "msg": "CASE tới cảm biến thất bại, thử lại sau 10 s"}],
}

MODES = ["Lạnh", "Sưởi", "Khô", "Chỉ quạt", "Tự động"]
FANS = ["tự động", "mức 1", "mức 2", "mức 3", "mức 4", "mức 5", "êm", "mạnh"]


def describe(key):
    if key == "off":
        return "Tắt"
    m = key[1:].replace("t", " ").replace("f", " ").replace("s", " ").split()
    mode, t, f, s = (int(x) for x in m)
    temp = "" if mode == 3 else f" {t // 2}{',5' if t % 2 else ''}°C"
    return f"{MODES[mode]}{temp}, quạt {FANS[f]}, đảo gió {'bật' if s else 'tắt'}"


def state_obj(key):
    if key == "off":
        d = {"power": 0, "mode": 0, "temp_half": 0, "fan": 0, "swing": 0}
    else:
        m = key[1:].replace("t", " ").replace("f", " ").replace("s", " ").split()
        d = dict(zip(["mode", "temp_half", "fan", "swing"], (int(x) for x in m)))
        d["power"] = 1
    d["key"] = key
    d["desc"] = describe(key)
    return d


def caps(slot):
    codes = [state_obj(k) for k in STATE["codes"].get(slot, [])]
    on = [c for c in codes if c["power"]]
    modes = 0
    fans = 0
    swings = 0
    ranges = {}
    for c in on:
        modes |= 1 << c["mode"]
        fans |= 1 << c["fan"]
        swings |= 1 << c["swing"]
        lo, hi = ranges.get(c["mode"], (99, 0))
        ranges[c["mode"]] = (min(lo, c["temp_half"]), max(hi, c["temp_half"]))
    return {"has_off": any(not c["power"] for c in codes), "modes_mask": modes, "fans_mask": fans,
            "swing_mask": swings, "codes": len(codes),
            "ranges": [{"mode": m, "min_half": r[0], "max_half": r[1]} for m, r in sorted(ranges.items())]}


def climate():
    th = STATE["thermostat"]
    desired = "off" if th["mode"] == 0 else f"m0t{th['cool_sp'] // 50}f{STATE['fan']}s{STATE['swing']}"
    return {"running": True, "ready": STATE["profiles"][0].get("locked", False), "not_ready":
            "" if STATE["profiles"][0].get("locked") else "Profile chưa được khóa (xác nhận) sau khi học",
            "user_mode": th["mode"], "cool_target": th["cool_sp"], "heat_target": th["heat_sp"],
            "fan": STATE["fan"], "swing": STATE["swing"], "temp_valid": STATE["binding"] is not None,
            "filtered": 2643 if STATE["binding"] else None, "control_mode": STATE["config"]["control_mode"],
            "compensating": True, "offset": -100, "ir_setpoint": 2400, "reason": 5,
            "reason_text": "Đã chỉnh setpoint IR một bước", "desired": state_obj(desired), "has_sent": True,
            "last_sent": state_obj("m0t48f0s0"), "last_kind": 1, "last_sent_age_s": 90, "pending_ms": 0,
            "last_error": ""}


def ir():
    i = STATE["ir"]
    return {"ready": True, "busy": False, "sent": i["sent"], "failed": i["failed"], "dropped_duplicate": 1,
            "superseded": 0, "last_age_s": int(time.time() - i["t"]), "last_label": i["last"],
            "last_source": i["src"], "last_error": "", "carrier_hz": 38000}


def matter():
    return {"started": True, "commissioner_ready": True, "home_fabrics": len(STATE["fabrics"]),
            "window_open": STATE["window_open"], "endpoint": 1, "heat": False, "vendor_id": 0xFFF1,
            "product_id": 0x8000, "discriminator": 2345}


def pair_progress():
    p = STATE["pair"]
    st = p["state"]
    if st in (1, 2, 3, 4):
        elapsed = time.time() - p["t"]
        st = min(5, 1 + int(elapsed / 1.0))
        if st == 5:
            p["state"] = 5
    out = {"state": st, "stage": ["", "kFindOperational", "kSendPAKEParameters", "kSendNOC", "kReadCommissioningInfo",
                                  "kCleanup", "", "kRemoveFabric"][st],
           "error": "Hết thời gian chờ cảm biến" if st == 6 else "", "attestation_overridden": False,
           "attestation_note": "", "node_id": "0x%016X" % p["node"] if st else "0x0000000000000000",
           "already_on_fabric": False, "vendor_id": 0x115F if st >= 4 else 0, "product_id": 0x2002 if st >= 4 else 0,
           "vendor_name": "Aqara" if st >= 4 else "", "product_name": "Temperature and Humidity Sensor T1" if st >= 4 else "",
           "node_label": "Phòng khách" if st >= 4 else "", "endpoints": [], "started_age_s": 3, "updated_age_s": 0}
    if st == 5:
        out["endpoints"] = [{"endpoint": 1, "read_ok": True, "null": False, "value": 2651, "min": -2000, "max": 6000},
                            {"endpoint": 2, "read_ok": True, "null": True, "value": None, "min": None, "max": None}]
    return out


def sensor(full=True):
    b = STATE["binding"]
    s = {"bound": b is not None, "name": "Phòng khách" if b else "", "state": 3 if b else 0,
         "state_name": "Đã subscribe" if b else "Chưa ghép cảm biến", "stale": False, "error": ""}
    if full and b:
        s.update({"node_id": "0x%016X" % b["node"], "endpoint": b["endpoint"], "vendor_id": 0x115F,
                  "product_id": 0x2002, "vendor_name": "Aqara", "product_name": "Temperature and Humidity Sensor T1",
                  "label": "Phòng khách", "has_value": True, "value_null": False, "value_plausible": True,
                  "value": 2651, "value_age_s": 75, "alive_age_s": 12, "min": -2000, "max": 6000,
                  "min_interval_s": 30, "max_interval_s": 600, "stale_timeout_s": 1260, "retries": 0,
                  "next_retry_ms": 0})
    if full:
        s.update({"local_configured": False, "local_valid": False, "local": None, "local_age_s": None})
        s.setdefault("has_value", False)
    return s


def temp():
    b = STATE["binding"]
    return {"valid": b is not None, "centi": 2651 if b else None, "source": 1 if b else 0,
            "source_name": "Cảm biến Matter" if b else "Không có", "value_age_s": 75 if b else None,
            "alive_age_s": 12 if b else None, "simulated": False}


def learn_status():
    l = STATE["learn"]
    st = l["state"]
    if st in (1, 2) and time.time() - l["t"] > 2.5:
        l["state"] = st = 3
    msg = {0: "", 1: "Hướng remote vào đầu thu và bấm nút", 2: "Đang nhận", 3: "Đã nhận 2 đoạn", 4: "", 5: ""}[st]
    return {"state": st, "session": l["session"], "message": msg, "segments": 2 if st == 3 else 0,
            "levels": 439 if st == 3 else 0, "duration_ms": 412 if st == 3 else 0, "noise_discarded": 0,
            "result_size": 900 if st == 3 else 0}


class Fail(Exception):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status = status
        self.msg = msg


def require(cond, status, msg):
    if not cond:
        raise Fail(status, msg)


def route(method, path, q, f):
    s = STATE
    if method == "GET":
        if path == "session":
            return {"token": TOKEN, "device": "Máy lạnh IR", "version": "1.0.0", "test_mode": True}
        if path == "status":
            p = s["profiles"][s["active"]]
            return {"wifi": s["wifi"], "matter": matter(), "temp": temp(), "sensor": sensor(False),
                    "climate": climate(), "ir": ir(),
                    "profile": {"slot": s["active"], "name": p["name"], "locked": p["locked"],
                                "codes": len(s["codes"][s["active"]]), "restart_needed": False},
                    "thermostat": s["thermostat"], "uptime_s": int(time.time() - T0) + 3600,
                    "error_count": len(s["errors"]), "test_mode": True}
        if path == "wifi":
            w = dict(s["wifi"])
            w["scanning"] = time.time() < s["scan_until"]
            if not w["scanning"] and s["scan_until"]:
                s["scan"] = [{"ssid": "NhaToi", "rssi": -58, "channel": 6, "open": False},
                             {"ssid": "Hàng xóm <b>5G</b>", "rssi": -77, "channel": 11, "open": False},
                             {"ssid": "Quan Ca Phe", "rssi": -83, "channel": 1, "open": True}]
            if w["job"] == 1 and time.time() > s.get("job_t", 0) + 3:
                s["wifi"]["job"] = w["job"] = 2
                s["wifi"]["job_message"] = w["job_message"] = "Đã kết nối " + s["wifi"]["ssid"]
            return {"wifi": w, "scan": s["scan"]}
        if path == "matter":
            return {"matter": matter(), "fabrics": s["fabrics"], "window_max_s": 900}
        if path == "matter/onboarding":
            size = 25
            rows = []
            for y in range(size):
                bits = "".join("1" if (x * y + x + y) % 3 == 0 or (x < 7 and y < 7) else "0" for x in range(size))
                bits += "0" * ((4 - size % 4) % 4)
                rows.append("".join("%x" % int(bits[i:i + 4], 2) for i in range(0, len(bits), 4)))
            return {"qr": "MT:Y.K9042C00KA0648G00", "manual": "34970112332", "qr_size": size, "qr_rows": rows}
        if path == "sensor":
            return {"sensor": sensor(), "temp": temp(), "progress": pair_progress(),
                    "commissioner": {"ready": True, "init_error": "", "busy": s["pair"]["state"] in (1, 2, 3, 4)},
                    "test_mode": True}
        if path == "ac":
            profs = []
            for p in s["profiles"]:
                p = dict(p)
                if p["present"]:
                    p.update(caps(p["slot"]))
                profs.append(p)
            return {"active": s["active"], "profiles": profs, "ready": s["profiles"][0]["locked"],
                    "not_ready": "" if s["profiles"][0]["locked"] else "Profile chưa được khóa (xác nhận) sau khi học",
                    "restart_needed": False, "default_carrier_hz": 38000,
                    "thermostat": dict(s["thermostat"], heat=False, dry=False, fan_only=False, cool_min=2400,
                                       cool_max=2600, heat_min=1600, heat_max=3000),
                    "climate": climate(), "ir": ir(), "learn": learn_status()}
        if path == "ac/codes":
            slot = int(q.get("slot", ["0"])[0])
            return {"slot": slot, "codes": s["codes"].get(slot, [])}
        if path == "config":
            return {"config": s["config"], "local_configured": False, "climate": climate()}
        if path == "diag":
            return {"firmware": {"project": "ac_matter_ir", "version": "1.0.0", "idf": "v6.0.2",
                                 "esp_matter": "1.6.1", "date": "Oct 29 2026", "time": "05:47:00", "test_mode": True},
                    "chip": {"model": 9, "revision": 2, "cores": 2, "flash": 16777216, "psram": 8388608,
                             "mac": "AA:BB:CC:11:22:33"},
                    "uptime_s": int(time.time() - T0) + 3600, "reset_reason": "Cấp nguồn",
                    "heap_internal": {"total": 310000, "free": 92000, "min_free": 70000, "largest": 31000},
                    "heap_psram": {"total": 8300000, "free": 7600000, "min_free": 7500000, "largest": 7000000},
                    "nvs_main": {"used": 140, "free": 360, "total": 500}, "nvs_ir": {"used": 300, "free": 1700, "total": 2000},
                    "fabric_b": {"ready": True, "init_error": "", "fabric_index": 2, "fabric_id": "0x6D1C2E3F4A5B6C7D",
                                 "compressed_fabric_id": "0x1122334455667788", "controller_node_id": "0x0000000000ABCDEF",
                                 "paa_count": 75, "listen_port": 5542},
                    "matter": matter(), "ir": ir(), "error_count": len(s["errors"]),
                    "errors": [{"age_s": int(time.time() - e["t"]), "module": e["module"], "msg": e["msg"]} for e in s["errors"]]}
        raise Fail(404, "API không tồn tại")

    # POST
    g = lambda k, d=None: f.get(k, [d])[0]
    if path == "portal/close" or path == "portal/extend":
        return {"message": "Đã thoát cấu hình: Wi-Fi cấu hình sẽ tắt sau 1 giây"} if path == "portal/close" else {}
    if path == "wifi/scan":
        s["scan_until"] = time.time() + 2
        return {}
    if path == "wifi/sta":
        ssid = g("ssid", "")
        pw = g("password", "")
        require(1 <= len(ssid.encode()) <= 32, 400, "Tên Wi-Fi (SSID) phải dài 1-32 byte")
        require(pw == "" or 8 <= len(pw) <= 63, 400, "Mật khẩu Wi-Fi phải dài 8-63 ký tự")
        s["wifi"].update({"job": 1, "job_message": "Đang kết nối mạng mới...", "ssid": ssid})
        s["job_t"] = time.time()
        return {"message": "Đã lưu, đang kết nối..."}
    if path == "wifi/ap_password":
        require(8 <= len(g("password", "")) <= 63, 400, "Mật khẩu Wi-Fi cấu hình phải dài 8-63 ký tự")
        require(g("password") == g("confirm"), 400, "Hai mật khẩu không khớp")
        return {"message": "Đã lưu. Mật khẩu mới có hiệu lực từ lần mở cấu hình sau"}
    if path == "matter/window":
        s["window_open"] = g("action") == "open"
        return {}
    if path == "sensor/pair":
        code = g("code", "").replace("-", "").replace(" ", "")
        require(code.isdigit() and len(code) in (11, 21) or code.startswith("MT:"), 400,
                "Mã chia sẻ không hợp lệ (11 hoặc 21 chữ số, hoặc mã QR bắt đầu bằng MT:)")
        require(s["binding"] is None, 409, "Đang có cảm biến được ghép. Hãy gỡ cảm biến hiện tại trước")
        s["pair"].update({"state": 1, "t": time.time()})
        return {}
    if path == "sensor/cancel":
        s["pair"]["state"] = 0
        return {}
    if path == "sensor/bind":
        require(s["pair"]["state"] == 5, 409, "Hãy ghép hoặc đọc thử cảm biến trước khi lưu")
        require(int(g("node", "0"), 16) == s["pair"]["node"], 409, "Node không khớp")
        s["binding"] = {"node": s["pair"]["node"], "endpoint": int(g("endpoint"))}
        return {"message": "Đã lưu cảm biến, đang subscribe"}
    if path == "sensor/read":
        s["pair"].update({"state": 4, "t": time.time() - 3})
        return {}
    if path == "sensor/remove":
        s["binding"] = None
        s["pair"]["state"] = 0
        return {"message": "Đang gửi RemoveFabric: chỉ xóa fabric của ESP, liên kết với hub cũ giữ nguyên"}
    if path in ("sensor/forget",):
        s["binding"] = None
        return {}
    if path in ("sensor/retry", "sensor/simulate", "ac/resend", "ac/learn/cancel"):
        if path == "ac/learn/cancel":
            s["learn"]["state"] = 5
        return {}
    if path == "ac/profile/create":
        slot = int(g("slot"))
        require(not s["profiles"][slot]["present"], 409, "Ô profile này đã có dữ liệu")
        s["profiles"][slot] = {"slot": slot, "present": True, "name": g("name"), "locked": False, "driver": 1,
                               "step_half": int(g("step")), "default_fan": 0, "default_swing": 0,
                               "carrier_hz": int(g("carrier", "0"))}
        s["codes"][slot] = []
        return {}
    if path == "ac/profile/update":
        p = s["profiles"][int(g("slot"))]
        require(not p["locked"], 409, "Profile đã khóa: hãy mở khóa trước khi sửa")
        p.update({"name": g("name"), "step_half": int(g("step")), "default_fan": int(g("fan")),
                  "default_swing": int(g("swing")), "carrier_hz": int(g("carrier"))})
        return {}
    if path == "ac/profile/delete":
        slot = int(g("slot"))
        require(not s["profiles"][slot]["locked"], 409, "Profile đang khóa: hãy mở khóa trước khi xóa")
        s["profiles"][slot] = {"slot": slot, "present": False}
        return {}
    if path == "ac/profile/select":
        s["active"] = int(g("slot"))
        return {"restart_needed": False}
    if path == "ac/profile/lock":
        slot = int(g("slot"))
        lock = g("locked") == "1"
        c = caps(slot)
        require(not lock or (c["has_off"] and c["modes_mask"] & 1), 409,
                "Cần học lệnh Tắt và ít nhất một lệnh Làm lạnh trước khi khóa")
        s["profiles"][slot]["locked"] = lock
        return {"restart_needed": False}
    if path == "ac/learn/start":
        require(s["learn"]["state"] not in (1, 2), 409, "Đang học một lệnh khác")
        s["learn"].update({"state": 1, "session": s["learn"]["session"] + 1, "t": time.time()})
        return {}
    if path == "ac/learn/test":
        require(learn_status()["state"] == 3 and int(g("session")) == s["learn"]["session"], 409,
                "Không còn lệnh vừa học (đã học lệnh khác hoặc thất bại)")
        s["ir"].update({"sent": s["ir"]["sent"] + 1, "last": "Phát thử lệnh vừa học", "t": time.time(), "src": 2})
        return {}
    if path == "ac/learn/save":
        require(learn_status()["state"] == 3 and int(g("session")) == s["learn"]["session"], 409,
                "Không còn lệnh vừa học (đã học lệnh khác hoặc thất bại)")
        slot = int(g("slot"))
        require(not s["profiles"][slot]["locked"], 409, "Profile đã khóa: hãy mở khóa trước khi học thêm")
        if g("power") == "0":
            key = "off"
        else:
            t = 0 if g("mode") == "3" else int(g("temp_half"))
            key = f"m{g('mode')}t{t}f{g('fan')}s{g('swing')}"
        if key not in s["codes"][slot]:
            s["codes"][slot].append(key)
        return {"state": state_obj(key)}
    if path == "ac/code/test":
        s["ir"].update({"sent": s["ir"]["sent"] + 1, "last": "Thử: " + describe(g("key")), "t": time.time(), "src": 2})
        return {}
    if path == "ac/code/delete":
        slot = int(g("slot"))
        require(not s["profiles"][slot]["locked"], 409, "Profile đã khóa: hãy mở khóa trước khi xóa lệnh")
        s["codes"][slot].remove(g("key"))
        return {}
    if path == "ac/control":
        mode = int(g("mode"))
        require(mode in (0, 3), 409, "Chế độ này chưa có trong profile máy lạnh")
        s["thermostat"]["mode"] = mode
        if g("sp"):
            s["thermostat"]["cool_sp"] = int(g("sp"))
        if g("fan") is not None:
            s["fan"] = int(g("fan"))
            s["swing"] = int(g("swing"))
        return {}
    if path == "config":
        c = s["config"]
        for k in ("control_mode", "deadband_centi", "max_offset_centi", "comp_interval_s", "filter_tau_s",
                  "user_debounce_ms", "stale_min_s"):
            if g(k) is not None:
                c[k] = int(g(k))
        for k in ("allow_local_fallback", "resend_on_boot"):
            if g(k) is not None:
                c[k] = g(k) == "1"
        require(10 <= c["deadband_centi"] <= 300, 400, "Vùng chết phải từ 0,1 đến 3 °C")
        return {"config": c}
    if path == "system/reboot":
        return {"message": "Thiết bị đang khởi động lại..."}
    raise Fail(404, "API không tồn tại")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_api(self, method):
        u = urlsplit(self.path)
        path = u.path[len("/api/"):]
        try:
            if path != "session":
                require(self.headers.get("X-CSRF-Token") == TOKEN, 403, "Phiên không hợp lệ: hãy tải lại trang")
            form = {}
            if method == "POST":
                require(self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded"), 400,
                        "Content-Type không được hỗ trợ")
                n = int(self.headers.get("Content-Length", "0"))
                require(n <= 2048, 413, "Nội dung yêu cầu quá lớn")
                form = parse_qs(self.rfile.read(n).decode(), keep_blank_values=True)
            out = {"ok": True, "portal_left_s": 600}
            out.update(route(method, path, parse_qs(u.query), form) or {})
            self.send_json(200, out)
        except Fail as e:
            self.send_json(e.status, {"ok": False, "error": e.msg})

    def do_GET(self):
        if self.path.startswith("/api/"):
            return self.handle_api("GET")
        name = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/app.css": "app.css"}.get(
            urlsplit(self.path).path)
        if not name:
            self.send_response(404)
            self.end_headers()
            return
        with open(os.path.join(WWW, name), "rb") as fh:
            body = fh.read()
        ctype = {"html": "text/html", "js": "text/javascript", "css": "text/css"}[name.rsplit(".", 1)[1]]
        self.send_response(200)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Security-Policy",
                         "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                         "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.startswith("/api/"):
            return self.handle_api("POST")
        self.send_response(405)
        self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"mock portal on http://127.0.0.1:{port}/", flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
