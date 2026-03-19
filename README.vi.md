# ♻️ WasteVision AI

> **Hệ thống Phân loại & Giám sát Rác thải Thông minh** — Nhóm 2 | Đại học FPT

![WasteVision Banner](./assets/banner.jpg)

[![Python](https://img.shields.io/badge/Python-3.8+-blue?logo=python)](https://python.org)
[![Flask](https://img.shields.io/badge/Flask-Web%20Server-black?logo=flask)](https://flask.palletsprojects.com)
[![ResNet50](https://img.shields.io/badge/Model-ResNet50-orange)](https://keras.io/api/applications/resnet/)
[![ESP32](https://img.shields.io/badge/Hardware-ESP32--S3-red)](https://www.espressif.com)
[![YouTube](https://img.shields.io/badge/Demo-YouTube-red?logo=youtube)](https://www.youtube.com/watch?v=EwzhzZWfLMA)
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)

[🇬🇧 English](./README.md) &nbsp;|&nbsp; 🇻🇳 Tiếng Việt

---

## 🎬 Video Demo

[![WasteVision AI Demo](https://img.youtube.com/vi/EwzhzZWfLMA/maxresdefault.jpg)](https://www.youtube.com/watch?v=EwzhzZWfLMA)

> ▶️ Bấm vào ảnh để xem video demo đầy đủ trên YouTube

---

## 📖 Giới thiệu

**WasteVision AI** là một hệ thống phân loại và giám sát rác thải thông minh, tích hợp Computer Vision, IoT và Web Dashboard thời gian thực. Hệ thống tự động phân loại rác tại nguồn với độ chính xác trên **90%**, giảm thiểu sự phụ thuộc vào con người và nâng cao hiệu quả quản lý rác thải.

### 🎯 Mục tiêu dự án

- Tự động hóa phân loại rác tại nguồn
- Đạt độ chính xác phân loại **>90%**
- Giảm thiểu lỗi do con người
- Giám sát thời gian thực và cảnh báo an toàn

---

## ✨ Tính năng nổi bật

| Tính năng | Mô tả |
|-----------|-------|
| 🤖 **AI Classification** | Phân loại Nhựa / Giấy / Kim loại bằng ResNet50 |
| ⚙️ **Auto Sorting** | Servo tự động phân loại theo góc (0° / 115° / 190°) |
| 📡 **IoT Monitoring** | Giám sát mức đầy, nhiệt độ, khí độc theo thời gian thực |
| 🌐 **Web Dashboard** | Giao diện trực quan với biểu đồ Chart.js |
| 🔔 **Smart Alerts** | Cảnh báo qua Buzzer, LCD và Telegram Bot |
| 🔍 **Explainability** | Grad-CAM visualization để giải thích quyết định AI |

---

## 🏗️ Kiến trúc hệ thống

```
┌─────────────┐    Image    ┌─────────────┐    Result   ┌─────────────┐
│  ESP32-S3   │ ──────────► │  AI Server  │ ──────────► │  Web Server │
│  (Hardware) │             │  (ResNet50) │             │   (Flask)   │
└──────┬──────┘             └─────────────┘             └──────┬──────┘
       │                                                        │
       │ Dữ liệu cảm biến (HTTP)                       REST API│
       │                                                        │
┌──────▼──────┐                                         ┌──────▼──────┐
│  Cảm biến   │                                         │  Dashboard  │
│ Ultrasonic  │                                         │  Chart.js   │
│ MQ2 / DHT11 │                                         │  Telegram   │
│ IR Array    │                                         └─────────────┘
└─────────────┘
```

### 🔧 Phần cứng

| Thành phần | Chi tiết |
|------------|----------|
| **MCU** | ESP32-S3 |
| **Camera** | OV2640 |
| **Cảm biến** | HC-SR04 (siêu âm), MQ-2 (khí/khói), DHT11 (nhiệt độ/độ ẩm), IR array |
| **Cơ cấu** | Servo motor × 2, Buzzer, ILI9341 TFT LCD |
| **Góc phân loại** | Nhựa → 0° · Giấy → 115° · Kim loại → 190° |

### 🧠 AI Stack

- **Model**: ResNet50 (transfer learning, pre-trained ImageNet)
- **Framework**: TensorFlow / Keras
- **Augmentation**: HorizontalFlip, VerticalFlip, Rotate, BrightnessContrast, ColorJitter, Coarse Dropout
- **Tỷ lệ Train/Test**: 80% / 20%
- **Giải thích AI**: Grad-CAM

### 🌐 Software Stack

- **Backend**: Python, Flask
- **Frontend**: HTML / CSS / JS, Chart.js
- **Thông báo**: Telegram Bot API
- **Firmware**: Arduino C++ (ESP32-S3)

---

## 📊 Dữ liệu huấn luyện

| Loại rác | Train | Test | Tổng | Tỷ lệ |
|----------|------:|-----:|-----:|:-----:|
| Kim loại | 851 | 213 | 1,064 | 34.1% |
| Giấy | 917 | 230 | 1,147 | 36.7% |
| Nhựa | 729 | 183 | 912 | 29.2% |
| **Tổng** | **2,497** | **626** | **3,123** | 100% |

---

## 📈 Kết quả

| Loại rác | Confidence (mẫu thực tế) |
|----------|--------------------------|
| Nhựa | 99.83% — 100.00% |
| Kim loại | 94.59% — 98.36% |
| Giấy | 93.21% — 95.49% |

- Hệ thống đạt **>90% accuracy** trên tập test
- Cảnh báo thùng đầy hoạt động chính xác khi mức đầy ≥ 92%
- Thời gian xử lý mỗi vật thể: ~4.5 giây (chụp → phân loại → phân loại cơ học)

---

## 🔄 Quy trình hoạt động

```
Cảm biến IR phát hiện rác
          │
          ▼
  Xác minh 3 giây (chống spam)
          │
          ▼
  Camera chụp ảnh → Gửi đến AI Server
          │
          ▼
  ResNet50 phân loại → Nhựa / Giấy / Kim loại
          │
          ▼
  Servo xoay đúng góc → Mở cửa → Chờ 4.5s → Đóng cửa
          │
          ▼
  Cập nhật Dashboard + Ghi log
          │
          ▼
  Hệ thống sẵn sàng cho chu kỳ tiếp theo
```

---

## 🚀 Hướng dẫn cài đặt

### Yêu cầu

- Python 3.8+
- Arduino IDE (để flash firmware ESP32-S3)
- Telegram Bot Token

### 0. Xem demo trước (tùy chọn)

🎬 [https://www.youtube.com/watch?v=EwzhzZWfLMA](https://www.youtube.com/watch?v=EwzhzZWfLMA)

### 1. Clone repository

```bash
git clone https://github.com/<your-username>/WasteVision-AI.git
cd WasteVision-AI
```

### 2. Cài đặt dependencies

```bash
pip install -r requirements.txt
```

### 3. Tải thư mục AI

> ⚠️ Thư mục `ai/` chứa model đã huấn luyện **không được đưa lên GitHub** do giới hạn dung lượng file.

**Tải xuống tại Google Drive:**

### 📁 [Bấm vào đây để tải thư mục AI](https://drive.google.com/drive/folders/1YwfRfeoxepan-EBAGZU-itxA6FhUekhH?usp=sharing)

Sau khi tải xong, đặt các file vào đúng vị trí trong project:

```
WasteVision-AI/
└── ai_server/
    └── model/
        ├── resnet50_waste.h5      ← đặt file vào đây
        └── ...
```

### 4. Cấu hình môi trường

Tạo file `.env` tại thư mục gốc:

```env
TELEGRAM_BOT_TOKEN=your_bot_token
TELEGRAM_CHAT_ID=your_chat_id
ESP32_IP=192.168.x.x
FLASK_SECRET_KEY=your_secret_key
```

### 5. Chạy AI Server

```bash
cd ai_server
python app.py
```

### 6. Chạy Web Server

```bash
cd web_server
python app.py
```

### 7. Flash firmware ESP32-S3

Mở `firmware/wastevision_esp32/wastevision_esp32.ino` bằng Arduino IDE và nạp vào board.

---

## 📁 Cấu trúc thư mục

```
WasteVision-AI/
├── ai_server/              # AI classification server (Flask + ResNet50)
│   ├── app.py
│   └── model/              # ← Đặt model tải từ Google Drive vào đây
├── web_server/             # Web dashboard (Flask)
│   ├── app.py
│   ├── templates/
│   └── static/
├── firmware/               # ESP32-S3 Arduino firmware
│   └── wastevision_esp32/
├── assets/                 # Ảnh, sơ đồ
├── requirements.txt
├── .env.example
├── README.md               # Tiếng Anh
└── README.vi.md            # Tiếng Việt (file này)
```

---

## ⚠️ Hạn chế & Hướng phát triển

### Hạn chế hiện tại

- **Single-View Dependency**: Góc camera cố định gây nhầm lẫn với vật thể trông giống nhau
- **Angle Bias**: Đáy lon (trông như nắp) hoặc nhãn dán (trông như giấy) có thể gây misclassification
- **Environmental Sensitivity**: Độ chính xác giảm trong điều kiện ánh sáng kém hoặc vật thể bị che khuất

### Roadmap

- [ ] **Multi-view Vision** — Tích hợp thêm camera hoặc gương để có góc nhìn 360°
- [ ] **Dataset Expansion** — Bổ sung ảnh đa góc, cận cảnh để tăng robustness
- [ ] **System Reliability** — Cải thiện độ chính xác trong môi trường thực tế phức tạp
- [ ] **Mobile App** — Ứng dụng di động để giám sát từ xa
- [ ] **Multi-bin Support** — Hỗ trợ giám sát nhiều thùng rác đồng thời

---

## 👥 Thành viên nhóm

| Họ tên | MSSV | Vai trò |
|--------|------|---------|
| Châu Quốc Inh | CE190593 | Trưởng nhóm |
| Trần Minh Phước | CE190754 | Phần cứng / Firmware |
| Lưu Hữu Bình | CE200315 | AI / ML |
| Trần Nguyễn Thiên Thanh | CE200089 | Web Backend |
| Nguyễn Hữu Phát | CE200437 | Web Frontend |

---

## 🏫 Thông tin học phần

- **Trường**: Đại học FPT, cơ sở Cần Thơ
- **Môn học**: Software Engineering Fundamentals
- **Kỳ học**: Spring 2025

---

## 📄 License

Dự án này được phân phối theo giấy phép [MIT](LICENSE).

---

---

**WasteVision AI — Nhóm 2 | Đại học FPT**

*Góp phần xây dựng môi trường xanh hơn 🌿*
