def get_device_type(cur, power, angle, max_var, peak_cur):
    if angle > 40 and angle < 70 and power > 15 and power < 21:
        return "Monitor"
    if angle < 63 and angle > 50 and power < 13 and power > 5 and peak_cur < 10:
        return "Phone Charger"
    if angle > 69 and power > 10 and peak_cur > 15:
        return "Mixer"
    return "Unknown"


limits = {
    "Monitor": 3000,
    "Phone Charger": 60,
    "Unknown": 3000,
    "Mixer": 32
}