from flask import Flask, request, jsonify
import sqlite3
import json
from datetime import datetime

app = Flask(__name__)

# Initialize database
def init_db():
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    c.execute('''
        CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER,
            current REAL,
            voltage REAL,
            power REAL,
            energy REAL,
            wifi_signal REAL,
            device_type INTEGER,
            startup_peak REAL,
            startup_duration INTEGER,
            steady_power REAL,
            current_variance REAL,
            phantom_power REAL
        )
    ''')
    conn.commit()
    conn.close()

@app.route('/data', methods=['POST'])
def receive_data():
    if request.is_json:
        data = request.get_json()
        # Insert into database
        conn = sqlite3.connect('sensor_data.db')
        c = conn.cursor()
        c.execute('''
            INSERT INTO readings (
                timestamp, current, voltage, power, energy,
                wifi_signal, device_type, startup_peak, startup_duration,
                steady_power, current_variance, phantom_power
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ''', (
            data.get('timestamp'), data.get('current'), data.get('voltage'),
            data.get('power'), data.get('energy'), data.get('wifi_signal'),
            data.get('device_type'), data.get('startup_peak'),
            data.get('startup_duration'), data.get('steady_power'),
            data.get('current_variance'), data.get('phantom_power')
        ))
        conn.commit()
        conn.close()
        return jsonify({'status': 'ok'}), 200
    else:
        return jsonify({'error': 'Request must be JSON'}), 400

if __name__ == '__main__':
    init_db()
    app.run(host='0.0.0.0', port=5000, debug=False)