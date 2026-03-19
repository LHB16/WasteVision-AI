from flask import Flask, jsonify, render_template, request, Response
import requests
import json
import os
import threading
import time
import csv
import io
from datetime import datetime, timedelta

app = Flask(__name__)

# --- SYSTEM CONFIGURATION ---
# Allow override by environment variable to avoid hard-coded IP.
# Example PowerShell:
#   $env:ESP32_IP="http://192.168.137.50"; python .\web.py
ESP32_IP = os.environ.get("ESP32_IP", "http://192.168.137.221")
# Detailed sensor history (for export, advanced statistics)
DATA_FILE = "sensor_history.json"
# Log of new waste received (for history + 3-column statistics)
LOG_FILE = "waste_logs.json"
TELEGRAM_BOT_TOKEN = "8030669263:AAE_yYxwTVv0IVFsh0t32a2L4sOKW9Ljx1o"
TELEGRAM_CHAT_ID = "5853966423"

# Load alert configuration from alert_config.json
CONFIG_FILE = "alert_config.json"
ALERT_CONFIG = {}
try:
    with open(CONFIG_FILE, 'r') as f:
        ALERT_CONFIG = json.load(f) or {}
        GAS_THRESHOLD = ALERT_CONFIG.get('gas_threshold', 300)
        TEMP_THRESHOLD = ALERT_CONFIG.get('temp_threshold', 45)
        FULL_THRESHOLD = ALERT_CONFIG.get('bin_threshold', 92)
except:
    # Fallback if file cannot be read
    ALERT_CONFIG = {"gas_threshold": 300, "temp_threshold": 50, "bin_threshold": 90, "lang": "vi"}
    GAS_THRESHOLD = 300
    TEMP_THRESHOLD = 50
    FULL_THRESHOLD = 90

# Anti-spam notification (Resend every 5 minutes)
last_alerts = {"gas": 0, "temp": 0}
alert_lock = threading.Lock()  # Lock to avoid duplicate sending

# Prevent duplicate waste detection notifications (ESP32 might call both /api/history AND /api/detect)
# Cooldown 15 seconds for each waste type
last_waste_telegram = {}  # {"PLASTIC": timestamp, ...}
waste_telegram_cooldown_sec = 15
waste_telegram_lock = threading.Lock()

# ADDED THIS SECTION: Variable to store cache data from ESP32
latest_data = {
    "plastic": 0, "paper": 0, "metal": 0,
    "temperature": 0, "gas": 0,
    "wifi_rssi": 0,
    "is_mock": False
}
data_lock = threading.Lock() # Lock to avoid errors when reading/writing simultaneously
is_esp_online = False        # Connection status

# --- REGULATE HISTORY SAVING FREQUENCY (based on active tab) ---
# When user is viewing History/Statistics: save every 10-15 seconds
# When not viewing (Dashboard) or tab hidden: relax to 5 minutes
REFRESH_ACTIVE_SAVE_SEC = 12        # ~10-15s/time
REFRESH_IDLE_SAVE_SEC = 300         # 5 min/time
_client_active_until_ts = 0.0       # time marker: until when considered "viewing history/statistics"
_last_history_save_ts = 0.0         # time marker of last history.json write
_history_save_lock = threading.Lock()


def _mark_client_active(ttl_sec: int = 20):
    """Mark client as active viewing History/Statistics for ttl_sec seconds."""
    global _client_active_until_ts
    now_ts = time.time()
    _client_active_until_ts = max(_client_active_until_ts, now_ts + float(ttl_sec))


def _get_history_save_interval_sec() -> int:
    """Select history save interval based on 'active' status."""
    now_ts = time.time()
    return REFRESH_ACTIVE_SAVE_SEC if now_ts < _client_active_until_ts else REFRESH_IDLE_SAVE_SEC

def get_now():
    return datetime.now().strftime("%H:%M:%S - %d/%m/%Y")

def get_now_short():
    """Short format for Telegram footer: 10:26 - 26/01/2026"""
    return datetime.now().strftime("%H:%M - %d/%m/%Y")

def _visual_bar(percent, segments=10):
    """Create status bar ▓░░░ (0-100% → 0-10 blocks)."""
    p = max(0, min(100, float(percent)))
    n = min(segments, max(0, round(p * segments / 100)))
    return "▓" * n + "░" * (segments - n)

def _level_label(pct):
    """Level label: High >= 90, Medium >= 50, Low < 50."""
    if pct >= 90: return "Cao"
    if pct >= 50: return "Trung bình"
    return "Thấp"

def send_telegram(message):
    try:
        full_msg = f"{message}\n\n🕒 {get_now_short()}"
        url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
        payload = {"chat_id": TELEGRAM_CHAT_ID, "text": full_msg, "parse_mode": "HTML"}
        response = requests.post(url, json=payload, timeout=5)
        
        if response.status_code == 200:
            print(f"✅ Đã gửi Telegram: {message[:50]}...")
            return True
        else:
            print(f"❌ Lỗi Telegram API: {response.status_code} - {response.text}")
            return False
    except Exception as e:
        print(f"❌ Lỗi gửi Telegram: {e}")
        return False


def send_waste_telegram(waste_type: str, entry_time: str = None):
    """
    Send new waste detection notification, with deduplication.
    Prevent double sending when ESP32 calls both /api/history and /api/detect.
    """
    global last_waste_telegram
    wtype = (waste_type or "UNKNOWN").upper()
    now_ts = time.time()
    with waste_telegram_lock:
        last_ts = last_waste_telegram.get(wtype, 0)
        if (now_ts - last_ts) < waste_telegram_cooldown_sec:
            print(f"⏭️ Bỏ qua Telegram trùng (cooldown): {wtype}")
            return False
        msg = f"""♻️ PHÁT HIỆN RÁC MỚI
➖➖➖➖➖➖➖➖➖➖
🗑 Loại: <b>{wtype}</b>
🕒 Lúc: {entry_time or get_now_short()}"""
        if send_telegram(msg):
            last_waste_telegram[wtype] = now_ts
            return True
    return False


def init_waste_log():
    """Initialize waste log file if not exists."""
    try:
        if (not os.path.exists(LOG_FILE)) or os.stat(LOG_FILE).st_size == 0:
            with open(LOG_FILE, "w", encoding="utf-8") as f:
                json.dump([], f)
    except Exception as e:
        print(f"Lỗi khởi tạo waste log: {e}")


def append_waste_log(waste_type: str):
    """Append a new waste record. Deduplicated (15s cooldown)."""
    try:
        wtype = (waste_type or "UNKNOWN").upper()
        now = datetime.now()
        entry = {
            "time": now.strftime("%H:%M:%S"),
            "date": now.strftime("%Y-%m-%d"),
            "type": wtype
        }
        with waste_telegram_lock:
            logs = []
            if os.path.exists(LOG_FILE) and os.stat(LOG_FILE).st_size > 0:
                with open(LOG_FILE, "r", encoding="utf-8") as f:
                    try:
                        logs = json.load(f)
                    except:
                        logs = []
            # Prevent duplicate write: if last record has same type and within last 15 seconds -> skip
            if logs:
                last = logs[-1]
                if last.get("type") == wtype:
                    try:
                        last_dt = datetime.strptime(f"{last.get('date','')} {last.get('time','')}", "%Y-%m-%d %H:%M:%S")
                        delta_sec = (now - last_dt).total_seconds()
                        if 0 <= delta_sec < waste_telegram_cooldown_sec:
                            print(f"⏭️ Bỏ qua ghi log trùng (cooldown): {wtype}")
                            return last
                    except (ValueError, KeyError):
                        pass
            logs.append(entry)
            # Limit max 5000 records to avoid large file
            if len(logs) > 5000:
                logs = logs[-5000:]
            with open(LOG_FILE, "w", encoding="utf-8") as f:
                json.dump(logs, f, ensure_ascii=False, indent=2)
        return entry
    except Exception as e:
        print(f"Lỗi ghi waste log: {e}")
        return None

# --- AUTO MONITOR THREAD (RUNS IN BACKGROUND) ---
def auto_monitor():
    global last_alerts, latest_data, is_esp_online
    while True:
        try:
            # 1. Send request to ESP32 (Only this thread does this)
            resp = requests.get(f"{ESP32_IP}/status", timeout=1.5)
            
            if resp.status_code == 200:
                data = resp.json()

                # Backward compatibility: accept both "battery" (old) and "metal" (new) keys.
                # Normalize: always have data["metal"] for UI/web.
                if isinstance(data, dict):
                    if ("metal" not in data) and ("battery" in data):
                        data["metal"] = data.get("battery", 0)
                    if ("battery" not in data) and ("metal" in data):
                        # Keep old field to avoid breaking client/old data
                        data["battery"] = data.get("metal", 0)
                    # Normalize GAS: ESP32 might send "gas" (number) OR "gas_raw" + "gas_level" (code_ngon.cpp)
                    # Always have data["gas"] (number) so Telegram + web can report.
                    if "gas" not in data or data.get("gas") is None:
                        raw = data.get("gas_raw", 0)
                        try:
                            data["gas"] = int(raw) if raw is not None else 0
                        except (TypeError, ValueError):
                            data["gas"] = 0
                
                # 2. UPDATE CACHE (Important)
                with data_lock:
                    latest_data = data
                    is_esp_online = True
                
                now_time = time.time()
                now = datetime.now()
                
                # 3. Save to History
                save_data_realtime(data, now)
                
                # 4. Alert Logic (use thresholds from ALERT_CONFIG to be effective via API)
                gas_threshold = ALERT_CONFIG.get('gas_threshold', 300)
                temp_threshold = ALERT_CONFIG.get('temp_threshold', 50)
                full_threshold = ALERT_CONFIG.get('bin_threshold', 90)
                gas_val = data.get('gas', 0)
                gas_level = (data.get('gas_level') or '').strip().upper()
                # Over threshold when: gas number > threshold OR ESP32 reports gas_level = DANGER (code_ngon.cpp)
                gas_over = gas_val > gas_threshold or gas_level == 'DANGER'
                # --- Gas Alert ---
                with alert_lock:
                    if not gas_over:
                        last_alerts['gas'] = 0
                    elif gas_over and (now_time - last_alerts.get('gas', 0) > 300):
                        pct = min(100, gas_val * 100 / gas_threshold) if gas_threshold else 100
                        if gas_level == 'DANGER' and gas_val <= gas_threshold:
                            pct = 100
                        msg = f"""🚨 CẢNH BÁO: PHÁT HIỆN KHÍ ĐỘC
➖➖➖➖➖➖➖➖➖➖➖
☠️ Nồng độ: {gas_val} ppm{f' (ESP: {gas_level})' if gas_level else ''}
{_visual_bar(pct)} ({_level_label(pct)})
📝 Hành động: Không đến gần, thông gió!"""
                        if send_telegram(msg):
                            last_alerts['gas'] = now_time
                
                # --- Temperature Alert ---
                with alert_lock:
                    if data.get('temperature', 0) <= temp_threshold:
                        last_alerts['temp'] = 0
                    elif data.get('temperature', 0) > temp_threshold and (now_time - last_alerts.get('temp', 0) > 300):
                        temp = data.get('temperature', 0)
                        pct = min(100, temp * 100 / temp_threshold) if temp_threshold else 100
                        msg = f"""🚨 CẢNH BÁO: QUÁ NHIỆT
➖➖➖➖➖➖➖➖➖➖➖
🔥 Nhiệt độ: {temp}°C
{_visual_bar(pct)} ({_level_label(pct)})
📝 Hành động: Kiểm tra, tránh cháy!"""
                        if send_telegram(msg):
                            last_alerts['temp'] = now_time

                # --- Bin Full Alert ---
                bins = {'plastic': 'NHỰA', 'paper': 'GIẤY', 'metal': 'LON'}
                for k, name in bins.items():
                    with alert_lock:
                        if data.get(k, 0) < full_threshold:
                            last_alerts[f'full_{k}'] = 0
                        elif data.get(k, 0) >= full_threshold and (now_time - last_alerts.get(f'full_{k}', 0) > 300):
                            pct = data.get(k, 0)
                            msg = f"""🚨 CẢNH BÁO: THÙNG {name} ĐẦY
➖➖➖➖➖➖➖➖➖➖➖
🗑 Mức độ: {pct}%
{_visual_bar(pct)} ({_level_label(pct)})
📝 Hành động: Cần thu gom ngay!"""
                            if send_telegram(msg):
                                last_alerts[f'full_{k}'] = now_time
            else:
                # If ESP returns error (non-200)
                with data_lock: is_esp_online = False

        except Exception as e:
            # If network connection to ESP lost
            with data_lock: is_esp_online = False
            # print(f"Monitor error: {e}") # Can uncomment to debug
            
        time.sleep(1) # Wait 1 second before next update


def save_data_realtime(data, now):
    """Save data by interval to reduce history.json writes"""
    try:
        global _last_history_save_ts
        interval_sec = _get_history_save_interval_sec()
        now_ts = time.time()

        # Prevent saving too frequently (but allow first time)
        if _last_history_save_ts and (now_ts - _last_history_save_ts) < interval_sec:
            return

        # Lock to avoid duplicate writes if multiple requests/threads
        with _history_save_lock:
            # Re-check inside lock
            now_ts = time.time()
            interval_sec = _get_history_save_interval_sec()
            if _last_history_save_ts and (now_ts - _last_history_save_ts) < interval_sec:
                return

        today = now.strftime("%Y-%m-%d")
        
        # Read current data
        history = {}
        if os.path.exists(DATA_FILE):
            with open(DATA_FILE, 'r') as f:
                try: history = json.load(f)
                except: history = {}

        if today not in history: 
            history[today] = {"records": []}
        
        record = {
            "timestamp": now.strftime("%H:%M:%S"),
            "plastic": data.get("plastic", 0),
            "paper": data.get("paper", 0),
            # Normalize new key: metal (simultaneously keep battery alias for reading old files)
            "metal": data.get("metal", data.get("battery", 0)),
            "battery": data.get("metal", data.get("battery", 0)),
            "temp": data.get("temperature", 0),
            "gas": data.get("gas", 0),
        }
        history[today]["records"].append(record)
        
        # Limit 1000 records/day
        if len(history[today]["records"]) > 1000:
            history[today]["records"] = history[today]["records"][-1000:]

        recs = history[today]["records"]
        # Statistics: number of times bin full (>= FULL_THRESHOLD)
        total_plastic = sum(1 for r in recs if r.get("plastic", 0) >= FULL_THRESHOLD)
        total_paper = sum(1 for r in recs if r.get("paper", 0) >= FULL_THRESHOLD)
        total_metal = sum(1 for r in recs if r.get("metal", r.get("battery", 0)) >= FULL_THRESHOLD)
        
        history[today]["summary"] = {
            "avg_temp": round(sum(r.get("temp", 0) for r in recs) / len(recs), 2) if recs else 0,
            "count": len(recs),
            "last_updated": now.strftime("%Y-%m-%d %H:%M:%S"),
            "total_plastic": total_plastic,
            "total_paper": total_paper,
            "total_metal": total_metal,
            # Alias for compatibility with old data/logic
            "total_battery": total_metal,
        }

        with open(DATA_FILE, 'w') as f:
            json.dump(history, f, indent=2)
            
        _last_history_save_ts = now_ts
        mode = "ACTIVE" if interval_sec == REFRESH_ACTIVE_SAVE_SEC else "IDLE"
        print(f"✅ Lưu dữ liệu ({mode}, mỗi {interval_sec}s): {len(recs)} bản ghi ({now.strftime('%H:%M:%S')})")
            
    except Exception as e:
        print(f"Lỗi lưu realtime: {e}")


@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/status')
def get_status():
    # No longer request to ESP32, only get from RAM
    with data_lock:
        if is_esp_online:
            # Always ensure new client has "metal" key
            if isinstance(latest_data, dict) and ("metal" not in latest_data) and ("battery" in latest_data):
                latest_data["metal"] = latest_data.get("battery", 0)
            return jsonify(latest_data)
        else:
            return jsonify({"error": "Offline"}), 503

@app.route('/api/history', methods=['GET', 'POST'])
def api_history():
    """
    Main API for log like Telegram:
    - POST: ESP32 (or client) sends new waste type -> write to history.json
    - GET: Web UI reads all logs to display.
    Format of each entry in history.json:
      { "time": "HH:MM:SS", "date": "YYYY-MM-DD", "type": "PLASTIC|PAPER|METAL|..." }
    """
    # Ensure file exists
    init_waste_log()

    if request.method == 'POST':
        try:
            data = request.json or {}
            waste_type = str(data.get("type", "unknown")).upper()

            # Write to history.json (LOG_FILE)
            entry = append_waste_log(waste_type)

            # Send Telegram (deduplicated if ESP32 calls /api/detect too)
            send_waste_telegram(waste_type, entry['time'] if entry else None)

            return jsonify({"status": "success", "entry": entry}), 200
        except Exception as e:
            print(f"Lỗi POST /api/history: {e}")
            return jsonify({"status": "error", "error": str(e)}), 500

    # GET: return all logs
    try:
        _mark_client_active()
        if not os.path.exists(LOG_FILE) or os.stat(LOG_FILE).st_size == 0:
            return jsonify([])
        with open(LOG_FILE, 'r', encoding='utf-8') as f:
            logs = json.load(f)
        # Always ensure it is a list
        if not isinstance(logs, list):
            return jsonify([])
        return jsonify(logs)
    except Exception as e:
        print(f"Lỗi GET /api/history: {e}")
        return jsonify([]), 200

@app.route('/api/history/detailed')
def get_detailed_history():
    """Get detailed history with limited records"""
    try:
        # Client viewing History tab -> increase save frequency
        _mark_client_active()
        days = int(request.args.get('days', 7))
        limit = int(request.args.get('limit', 100))
        
        with open(DATA_FILE, 'r') as f:
            history = json.load(f)
        
        sorted_keys = sorted(history.keys(), reverse=True)[:days]
        result = {}
        
        for key in sorted_keys:
            day_data = history[key]
            records = day_data.get("records", [])
            # Get latest records
            result[key] = {
                "summary": day_data.get("summary", {}),
                "records": records[-limit:] if len(records) > limit else records
            }
        
        return jsonify(result)
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route('/api/detect', methods=['POST'])
def api_detect():
    """
    API for ESP32 to call when waste detected/classified.
    Expected JSON Body: {"type": "plastic" | "paper" | "metal" | ...}
    """
    try:
        data = request.json or {}
        waste_type = str(data.get("type", "unknown")).upper()

        # Write to log
        entry = append_waste_log(waste_type)

        # Send Telegram (deduplicated if ESP32 calls /api/history too)
        send_waste_telegram(waste_type, entry['time'] if entry else None)

        return jsonify({"status": "success", "entry": entry}), 200
    except Exception as e:
        print(f"Lỗi /api/detect: {e}")
        return jsonify({"status": "error", "error": str(e)}), 500


@app.route('/api/history/waste')
def api_waste_history():
    """
    Old alias: kept for compatibility.
    Actually reads simple log file /api/history.
    """
    try:
        if not os.path.exists(LOG_FILE) or os.stat(LOG_FILE).st_size == 0:
            return jsonify([])
        with open(LOG_FILE, "r", encoding="utf-8") as f:
            logs = json.load(f)
        if not isinstance(logs, list):
            return jsonify([])
        return jsonify(logs)
    except Exception as e:
        print(f"Lỗi /api/history/waste: {e}")
        return jsonify([]), 200

@app.route('/api/statistics/week')
def get_week_statistics():
    """Weekly statistics based on waste log (waste_logs.json)"""
    try:
        # Client viewing stats tab -> increase frequency (for auto_monitor)
        _mark_client_active()
        # Read waste log
        if not os.path.exists(LOG_FILE) or os.stat(LOG_FILE).st_size == 0:
            week_totals = {'plastic': 0, 'paper': 0, 'metal': 0, 'battery': 0}
            return jsonify({'week_totals': week_totals, 'most_common': 'plastic'})

        with open(LOG_FILE, 'r', encoding='utf-8') as f:
            logs = json.load(f)

        # If file is not list (e.g. old format), return 0
        if not isinstance(logs, list):
            week_totals = {'plastic': 0, 'paper': 0, 'metal': 0, 'battery': 0}
            return jsonify({'week_totals': week_totals, 'most_common': 'plastic'})

        now = datetime.now()
        week_start = now - timedelta(days=7)

        week_totals = {'plastic': 0, 'paper': 0, 'metal': 0, 'battery': 0}

        for entry in logs:
            try:
                date_str = entry.get('date')
                if not date_str:
                    continue
                dt = datetime.strptime(date_str, '%Y-%m-%d')
                if dt < week_start:
                    continue

                t = str(entry.get('type', '')).upper()
                if t == 'PLASTIC':
                    week_totals['plastic'] += 1
                elif t == 'PAPER':
                    week_totals['paper'] += 1
                elif t in ('METAL', 'BATTERY'):
                    week_totals['metal'] += 1
                    week_totals['battery'] += 1  # alias key old
            except Exception:
                continue

        # Determine most common type
        most_common = 'plastic'
        max_count = -1
        for k in ['plastic', 'paper', 'metal']:
            if week_totals.get(k, 0) > max_count:
                max_count = week_totals.get(k, 0)
                most_common = k

        return jsonify({
            'week_totals': week_totals,
            'most_common': most_common
        })
    except Exception as e:
        return jsonify({"error": "LOAD THẤT BẠI", "title": "LỖI TẢI DỮ LIỆU"}), 500

@app.route('/api/alert/config', methods=['GET', 'POST'])
def alert_config_api():
    """API to view and update alert config"""
    global ALERT_CONFIG
    
    if request.method == 'GET':
        return jsonify(ALERT_CONFIG)
    
    elif request.method == 'POST':
        try:
            new_config = request.json
            ALERT_CONFIG.update(new_config)
            
            # Save to file
            with open(CONFIG_FILE, 'w') as f:
                json.dump(ALERT_CONFIG, f, indent=2)
            
            return jsonify({"success": True, "message": "Cấu hình đã được cập nhật", "config": ALERT_CONFIG})
        except Exception as e:
            return jsonify({"success": False, "error": str(e)}), 500

@app.route('/api/alert/telegram', methods=['POST'])
def manual_report():
    try:
        data = request.json
        t = data.get('type', 'KHÁC')
        m = data.get('custom_message', '') or '—'
        msg = f"""🚨 BÁO CÁO SỰ CỐ: {t}
➖➖➖➖➖➖➖➖➖➖➖
📝 Mô tả: {m}"""
        if send_telegram(msg):
            return jsonify({"success": True})
        else:
            return jsonify({"success": False, "error": "Failed to send Telegram message"}), 500
    except Exception as e:
        return jsonify({"success": False, "error": str(e)}), 500

@app.route('/api/export/csv')
def export_csv():
    """Export history data to CSV"""
    try:
        days = int(request.args.get('days', 7))
        
        with open(DATA_FILE, 'r') as f:
            history = json.load(f)
        
        # Create CSV in memory
        output = io.StringIO()
        writer = csv.writer(output)
        
        # Header (English)
        writer.writerow(['Date', 'Time', 'Plastic (%)', 'Paper (%)', 'Metal (%)', 'Temperature (°C)', 'Gas'])
        
        # Get data by days
        sorted_keys = sorted(history.keys(), reverse=True)[:days]
        
        for date in sorted_keys:
            day_data = history.get(date, {})
            records = day_data.get("records", [])
            
            for record in records:
                writer.writerow([
                    date,
                    record.get("timestamp", ""),
                    record.get("plastic", 0),
                    record.get("paper", 0),
                    record.get("metal", record.get("battery", 0)),
                    record.get("temp", 0),
                    record.get("gas", 0)
                ])
        
        # Create response with CSV
        output.seek(0)
        filename = f"wastevision_history_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        
        return Response(
            output.getvalue(),
            mimetype='text/csv',
            headers={
                'Content-Disposition': f'attachment; filename={filename}',
                'Content-Type': 'text/csv; charset=utf-8'
            }
        )
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/export/summary')
def export_summary():
    """Export daily summary to CSV"""
    try:
        days = int(request.args.get('days', 7))
        
        with open(DATA_FILE, 'r') as f:
            history = json.load(f)
        
        # Create CSV in memory
        output = io.StringIO()
        writer = csv.writer(output)
        
        # Header (English)
        writer.writerow(['Date', 'Total Plastic', 'Total Paper', 'Total Metal', 'Avg Temperature', 'Record Count', 'Last Updated'])
        
        # Get data by days
        sorted_keys = sorted(history.keys(), reverse=True)[:days]
        
        for date in sorted_keys:
            day_data = history.get(date, {})
            summary = day_data.get("summary", {})
            
            writer.writerow([
                date,
                summary.get("total_plastic", 0),
                summary.get("total_paper", 0),
                summary.get("total_metal", summary.get("total_battery", 0)),
                round(summary.get("avg_temp", 0), 2),
                summary.get("count", 0),
                summary.get("last_updated", "")
            ])
        
        # Create response with CSV
        output.seek(0)
        filename = f"wastevision_summary_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        
        return Response(
            output.getvalue(),
            mimetype='text/csv',
            headers={
                'Content-Disposition': f'attachment; filename={filename}',
                'Content-Type': 'text/csv; charset=utf-8'
            }
        )
    except Exception as e:
        return jsonify({"error": str(e)}), 500

# Variable to ensure monitor thread starts only once
_monitor_started = False
_monitor_lock = threading.Lock()
_monitor_thread = None

def start_monitor_if_needed():
    """Start monitor thread if needed"""
    global _monitor_started, _monitor_thread
    with _monitor_lock:
        if not _monitor_started:
            _monitor_thread = threading.Thread(target=auto_monitor, daemon=True)
            _monitor_thread.start()
            _monitor_started = True
            print("✅ Auto-monitor thread started - Telegram alerts enabled")
            return True
    return False

if __name__ == '__main__':
    print("🚀 WasteVision AI Web Server starting...")
    print(f"📡 ESP32 IP: {ESP32_IP}")
    print(f"🤖 Telegram Bot: {TELEGRAM_BOT_TOKEN[:15]}...")
    print(f"💬 Chat ID: {TELEGRAM_CHAT_ID}")
    print("🌐 Web Interface: http://localhost:5000")
    
    # Initialize waste log file
    init_waste_log()
    
    # Start auto monitor thread (once)
    start_monitor_if_needed()
    
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)