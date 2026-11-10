# Topic và payload TH04

MSSV: 23110149
Topic: `23110149/node01/telemetry`

| Trường | Ý nghĩa |
|---|---|
| `device_id` | Mã ESP32, dùng `node01`. |
| `ts_ms` | Thời điểm lấy mẫu, tính bằng mili giây từ lúc ESP32 khởi động. |
| `temp_c` | Nhiệt độ DHT11, đơn vị °C. |
| `humidity_pct` | Độ ẩm tương đối DHT11, đơn vị %RH. |
| `rain_adc` | Số đọc ADC thô của cảm biến mưa, đơn vị count. |
| `valid` | `true` khi cả ba số đo hợp lệ. |

Trường nào thiếu hoặc đọc lỗi thì gửi `null`. Số `0` có thể là giá trị đo hợp lệ.
